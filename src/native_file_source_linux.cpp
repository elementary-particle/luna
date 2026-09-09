#include "native_file_source.h"
#include <atomic>

#if defined(__linux__)

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace luna::file {
namespace {

FileStatusCode CodeFromErrno(int value) {
  switch (value) {
  case ENOENT:
    return FileStatusCode::kNotFound;
  case EACCES:
  case EPERM:
    return FileStatusCode::kPermissionDenied;
  case ENOTDIR:
    return FileStatusCode::kNotADirectory;
  case EISDIR:
    return FileStatusCode::kIsDirectory;
  default:
    return FileStatusCode::kIoError;
  }
}

FileStatus NativeError(
    std::string operation, std::string path, std::string message, int value) {
  return FileStatus::Error(CodeFromErrno(value), std::move(operation),
      std::move(path), message + ": " + std::strerror(value), value);
}

uint64_t ModifiedTimeNs(const struct stat &st) {
  return static_cast<uint64_t>(st.st_mtim.tv_sec) * 1000000000ull +
      static_cast<uint64_t>(st.st_mtim.tv_nsec);
}

FileInfo InfoFromStat(std::string path, const struct stat &st) {
  FileInfo info;
  info.path = std::move(path);
  info.kind = S_ISDIR(st.st_mode) ? FileKind::kDirectory : FileKind::kFile;
  info.size = S_ISREG(st.st_mode) ? static_cast<uint64_t>(st.st_size) : 0;
  info.modified_time_ns = ModifiedTimeNs(st);
  info.source_id =
      static_cast<uint64_t>(st.st_dev) << 32 ^ static_cast<uint64_t>(st.st_ino);
  return info;
}

class UniqueFd {
public:
  UniqueFd() = default;
  explicit UniqueFd(int fd) : fd_(fd) {}
  ~UniqueFd() {
    if (fd_ >= 0) {
      close(fd_);
    }
  }

  UniqueFd(const UniqueFd &) = delete;
  UniqueFd &operator=(const UniqueFd &) = delete;

  UniqueFd(UniqueFd &&other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
  UniqueFd &operator=(UniqueFd &&other) noexcept {
    if (this != &other) {
      if (fd_ >= 0) {
        close(fd_);
      }
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }

  int get() const noexcept { return fd_; }
  int release() noexcept {
    const int fd = fd_;
    fd_ = -1;
    return fd;
  }
  explicit operator bool() const noexcept { return fd_ >= 0; }

private:
  int fd_ = -1;
};

// Walk using directory descriptors so a checked component cannot be swapped
// for a symlink before the subsequent open. The supplied root is trusted.
FileStatus OpenUnderRoot(std::string_view root, std::string_view path,
    bool follow_symlinks, int flags, bool create_parents, UniqueFd *out) {
  std::string normalized;
  FileStatus status = NormalizePath(path, true, &normalized);
  if (!status) {
    return status;
  }
  UniqueFd parent(open(std::string(root.empty() ? "." : root).c_str(),
      O_PATH | O_DIRECTORY | O_CLOEXEC));
  if (!parent) {
    return NativeError("open", std::string(root), "failed to open root", errno);
  }
  size_t begin = 0;
  while (true) {
    const size_t end = normalized.find('/', begin);
    const bool last = end == std::string::npos;
    const std::string part = normalized.empty()
        ? "."
        : normalized.substr(begin, last ? std::string::npos : end - begin);
    if ((!last || (flags & O_DIRECTORY)) && create_parents &&
        mkdirat(parent.get(), part.c_str(), 0777) != 0 && errno != EEXIST) {
      return NativeError(
          "mkdir", normalized, "failed to create directory", errno);
    }
    const int open_flags = (last ? flags : O_PATH | O_DIRECTORY) | O_CLOEXEC |
        (follow_symlinks ? 0 : O_NOFOLLOW);
    UniqueFd next(openat(parent.get(), part.c_str(), open_flags, 0666));
    if (!next) {
      return NativeError(
          "open", normalized, "failed to open path component", errno);
    }
    if (last) {
      *out = std::move(next);
      return FileStatus::Ok();
    }
    parent = std::move(next);
    begin = end + 1;
  }
}

class MappingOwner {
public:
  MappingOwner(void *address, size_t size, UniqueFd fd)
      : address_(address), size_(size), fd_(std::move(fd)) {}
  ~MappingOwner() {
    if (address_ != MAP_FAILED && address_ != nullptr && size_ > 0) {
      munmap(address_, size_);
    }
  }

private:
  void *address_ = MAP_FAILED;
  size_t size_ = 0;
  UniqueFd fd_;
};

class NativeFileStream final : public FileStream {
public:
  NativeFileStream(UniqueFd fd, FileInfo info)
      : fd_(std::move(fd)), info_(std::move(info)) {}

  const FileInfo &info() const noexcept override { return info_; }
  uint64_t position() const noexcept override { return position_; }
  bool eof() const noexcept override { return eof_; }

  FileStatus Read(std::span<std::byte> output, size_t *bytes_read) override {
    if (!bytes_read) {
      return FileStatus::Error(FileStatusCode::kIoError, "read", info_.path,
          "output argument is null");
    }
    *bytes_read = 0;
    if (output.empty()) {
      return FileStatus::Ok();
    }

    ssize_t count = read(fd_.get(), output.data(), output.size());
    if (count < 0) {
      return NativeError("read", info_.path, "failed to read file", errno);
    }
    *bytes_read = static_cast<size_t>(count);
    position_ += static_cast<uint64_t>(count);
    eof_ = count == 0 || position_ >= info_.size;
    return FileStatus::Ok();
  }

  FileStatus Seek(int64_t offset, std::ios_base::seekdir whence) override {
    int native_whence = SEEK_SET;
    if (whence == std::ios_base::cur) {
      native_whence = SEEK_CUR;
    } else if (whence == std::ios_base::end) {
      native_whence = SEEK_END;
    }

    off_t next = lseek(fd_.get(), static_cast<off_t>(offset), native_whence);
    if (next < 0) {
      return NativeError("seek", info_.path, "failed to seek file", errno);
    }
    position_ = static_cast<uint64_t>(next);
    eof_ = false;
    return FileStatus::Ok();
  }

private:
  UniqueFd fd_;
  FileInfo info_;
  uint64_t position_ = 0;
  bool eof_ = false;
};

class NativeFileSource final : public FileSource {
public:
  NativeFileSource(std::string root, NativeSourceOptions options)
      : root_(std::move(root)), options_(options) {}

  std::string_view DebugName() const noexcept override { return "native"; }
  int DefaultMountPriority() const noexcept override {
    return kNativeMountPriority;
  }

  FileStatus Stat(std::string_view path, FileInfo *out) override {
    if (!out) {
      return FileStatus::Error(FileStatusCode::kIoError, "stat",
          std::string(path), "output argument is null");
    }

    std::string normalized;
    FileStatus status = NormalizePath(path, true, &normalized);
    if (!status) {
      return status;
    }

    UniqueFd fd;
    status = OpenUnderRoot(
        root_, normalized, options_.follow_symlinks, O_PATH, false, &fd);
    if (!status) {
      return status;
    }
    struct stat st{};
    if (fstat(fd.get(), &st) != 0) {
      return NativeError("stat", normalized, "failed to stat file", errno);
    }
    if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) {
      return FileStatus::Error(
          FileStatusCode::kNotAFile, "stat", normalized, "not a regular file");
    }

    *out = InfoFromStat(std::move(normalized), st);
    return FileStatus::Ok();
  }

  FileStatus List(std::string_view path, std::vector<FileInfo> *out) override {
    if (!out) {
      return FileStatus::Error(FileStatusCode::kIoError, "list",
          std::string(path), "output argument is null");
    }
    out->clear();

    FileInfo dir_info;
    FileStatus status = Stat(path, &dir_info);
    if (!status) {
      return status;
    }
    if (dir_info.kind != FileKind::kDirectory) {
      return FileStatus::Error(FileStatusCode::kNotADirectory, "list",
          dir_info.path, "not a directory");
    }

    UniqueFd fd;
    status = OpenUnderRoot(root_, dir_info.path, options_.follow_symlinks,
        O_RDONLY | O_DIRECTORY, false, &fd);
    if (!status) {
      return status;
    }
    DIR *dir = fdopendir(fd.get());
    if (!dir) {
      return NativeError(
          "list", dir_info.path, "failed to open directory", errno);
    }
    fd.release(); // fdopendir owns the descriptor after success.
    std::unique_ptr<DIR, int (*)(DIR *)> dir_guard(dir, closedir);

    while (dirent *entry = readdir(dir)) {
      std::string name(entry->d_name);
      if (name == "." || name == "..") {
        continue;
      }
      std::string child_path =
          dir_info.path.empty() ? name : dir_info.path + "/" + name;
      FileInfo child;
      if (Stat(child_path, &child)) {
        out->push_back(std::move(child));
      }
    }
    std::sort(out->begin(), out->end(),
        [](const FileInfo &a, const FileInfo &b) { return a.path < b.path; });
    return FileStatus::Ok();
  }

  FileStatus OpenFile(std::string_view path, const ReadOptions &options,
      std::unique_ptr<FileStream> *out) override {
    if (!out) {
      return FileStatus::Error(FileStatusCode::kIoError, "open",
          std::string(path), "output argument is null");
    }
    out->reset();

    std::string normalized;
    FileStatus status = NormalizePath(path, false, &normalized);
    if (!status) {
      return status;
    }
    UniqueFd fd;
    status = OpenUnderRoot(root_, normalized, options_.follow_symlinks,
        O_RDONLY | O_NONBLOCK, false, &fd);
    if (!status) {
      return status;
    }
    struct stat st{};
    if (fstat(fd.get(), &st) != 0) {
      return NativeError("stat", normalized, "failed to stat file", errno);
    }
    if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) {
      return FileStatus::Error(
          FileStatusCode::kNotAFile, "open", normalized, "not a regular file");
    }
    FileInfo info = InfoFromStat(normalized, st);
    if (info.kind != FileKind::kFile) {
      return FileStatus::Error(
          FileStatusCode::kIsDirectory, "open", info.path, "is a directory");
    }
    if (options.max_size != 0 && info.size > options.max_size) {
      return FileStatus::Error(FileStatusCode::kBudgetExceeded, "open",
          info.path, "file exceeds requested maximum size");
    }

    *out = std::make_unique<NativeFileStream>(std::move(fd), std::move(info));
    return FileStatus::Ok();
  }

  FileStatus MapFile(std::string_view path, const ReadOptions &options,
      MappedFile *out) override {
    if (!out) {
      return FileStatus::Error(FileStatusCode::kIoError, "map",
          std::string(path), "output argument is null");
    }
    *out = MappedFile();

    std::string normalized;
    FileStatus status = NormalizePath(path, false, &normalized);
    if (!status) {
      return status;
    }
    UniqueFd fd;
    status = OpenUnderRoot(root_, normalized, options_.follow_symlinks,
        O_RDONLY | O_NONBLOCK, false, &fd);
    if (!status) {
      return status;
    }
    struct stat st{};
    if (fstat(fd.get(), &st) != 0) {
      return NativeError("stat", normalized, "failed to stat file", errno);
    }
    if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) {
      return FileStatus::Error(
          FileStatusCode::kNotAFile, "open", normalized, "not a regular file");
    }
    FileInfo info = InfoFromStat(normalized, st);
    if (info.kind != FileKind::kFile) {
      return FileStatus::Error(
          FileStatusCode::kIsDirectory, "map", info.path, "is a directory");
    }
    if (options.max_size != 0 && info.size > options.max_size) {
      return FileStatus::Error(FileStatusCode::kBudgetExceeded, "map",
          info.path, "file exceeds requested maximum size");
    }
    if (info.size > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
      return FileStatus::Error(FileStatusCode::kBudgetExceeded, "map",
          info.path, "file exceeds addressable memory size");
    }
    if (info.size == 0) {
      *out = MappedFile::FromOwnedBuffer({}, std::move(info));
      return FileStatus::Ok();
    }

    if (options.prefer_native_mapping && options_.prefer_native_mapping) {
      void *address = mmap(nullptr, static_cast<size_t>(info.size), PROT_READ,
          MAP_PRIVATE, fd.get(), 0);
      if (address != MAP_FAILED) {
        auto owner = std::make_shared<MappingOwner>(
            address, static_cast<size_t>(info.size), std::move(fd));
        *out = MappedFile::FromNativeMapping(
            std::span<const std::byte>(static_cast<const std::byte *>(address),
                static_cast<size_t>(info.size)),
            std::static_pointer_cast<const void>(owner), std::move(info));
        return FileStatus::Ok();
      }
      if (!options.allow_owned_fallback) {
        return NativeError("map", info.path, "failed to map file", errno);
      }
    }

    if (!options.allow_owned_fallback) {
      return FileStatus::Error(FileStatusCode::kUnsupported, "map", info.path,
          "native mapping is disabled and owned fallback is forbidden");
    }
    std::vector<std::byte> bytes;
    status = ReadAll(fd.get(), info, &bytes);
    if (!status) {
      return status;
    }
    *out = MappedFile::FromOwnedBuffer(std::move(bytes), std::move(info));
    return FileStatus::Ok();
  }

private:
  static FileStatus ReadAll(
      int fd, const FileInfo &info, std::vector<std::byte> *out) {
    out->assign(static_cast<size_t>(info.size), std::byte{});
    size_t offset = 0;
    while (offset < out->size()) {
      ssize_t count = read(fd, out->data() + offset, out->size() - offset);
      if (count < 0) {
        return NativeError("read", info.path, "failed to read file", errno);
      }
      if (count == 0) {
        break;
      }
      offset += static_cast<size_t>(count);
    }
    if (offset != out->size()) {
      return FileStatus::Error(
          FileStatusCode::kIoError, "read", info.path, "short read");
    }
    return FileStatus::Ok();
  }

  std::string root_;
  NativeSourceOptions options_;
};

std::string ParentPath(std::string_view path) {
  const size_t slash = path.find_last_of('/');
  if (slash == std::string_view::npos) {
    return "";
  }
  if (slash == 0) {
    return "/";
  }
  return std::string(path.substr(0, slash));
}

} // namespace

FileStatus OpenNativeFileSource(std::string root,
    const NativeSourceOptions &options, std::shared_ptr<FileSource> *out) {
  if (!out) {
    return FileStatus::Error(FileStatusCode::kIoError, "open_native", root,
        "output argument is null");
  }
  struct stat st{};
  if (stat(root.empty() ? "." : root.c_str(), &st) != 0) {
    return NativeError("open_native", root, "failed to stat root", errno);
  }
  if (!S_ISDIR(st.st_mode)) {
    return FileStatus::Error(FileStatusCode::kNotADirectory, "open_native",
        root, "root is not a directory");
  }
  *out = std::make_shared<NativeFileSource>(std::move(root), options);
  return FileStatus::Ok();
}

FileStatus NativeCreateDirectories(std::string_view host_path) {
  if (host_path.empty()) {
    return FileStatus::Ok();
  }
  std::string current;
  if (host_path.front() == '/') {
    current = "/";
  }

  size_t begin = host_path.front() == '/' ? 1 : 0;
  while (begin <= host_path.size()) {
    const size_t end = host_path.find('/', begin);
    std::string_view part(host_path.data() + begin,
        (end == std::string_view::npos ? host_path.size() : end) - begin);
    if (!part.empty()) {
      if (!current.empty() && current.back() != '/') {
        current.push_back('/');
      }
      current.append(part);
      if (mkdir(current.c_str(), 0777) != 0 && errno != EEXIST) {
        return NativeError(
            "mkdir", current, "failed to create directory", errno);
      }
    }
    if (end == std::string_view::npos) {
      break;
    }
    begin = end + 1;
  }
  return FileStatus::Ok();
}

FileStatus NativeWriteFile(
    std::string_view host_path, std::span<const std::byte> bytes) {
  const std::string path(host_path);
  FileStatus status = NativeCreateDirectories(ParentPath(path));
  if (!status) {
    return status;
  }

  UniqueFd fd(
      open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666));
  if (!fd) {
    return NativeError("write", path, "failed to open file", errno);
  }
  size_t offset = 0;
  while (offset < bytes.size()) {
    ssize_t count =
        write(fd.get(), bytes.data() + offset, bytes.size() - offset);
    if (count < 0) {
      return NativeError("write", path, "failed to write file", errno);
    }
    offset += static_cast<size_t>(count);
  }
  return FileStatus::Ok();
}

FileStatus NativeAtomicWriteUnderRoot(std::string_view root,
    std::string_view path, std::span<const std::byte> bytes) {
  std::string normalized;
  auto status = NormalizePath(path, false, &normalized);
  if (!status)
    return status;
  auto slash = normalized.rfind('/');
  auto parent_path =
      slash == std::string::npos ? "" : normalized.substr(0, slash);
  auto name = normalized.substr(slash == std::string::npos ? 0 : slash + 1);
  UniqueFd parent;
  status = OpenUnderRoot(
      root, parent_path, false, O_RDONLY | O_DIRECTORY, true, &parent);
  if (!status)
    return status;
  struct stat st{};
  if (fstatat(parent.get(), name.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0) {
    if (!S_ISREG(st.st_mode))
      return FileStatus::Error(
          FileStatusCode::kNotAFile, "write", normalized, "not a regular file");
  } else if (errno != ENOENT) {
    return NativeError(
        "write", normalized, "failed to stat destination", errno);
  }
  // O_EXCL prevents clobbering stale temporaries or following planted symlinks.
  static std::atomic<uint64_t> serial{0};
  std::string temporary;
  UniqueFd fd;
  for (int attempt = 0; attempt < 128; ++attempt) {
    temporary = ".luna-write-" + std::to_string(getpid()) + "-" +
        std::to_string(serial++);
    fd = UniqueFd(openat(parent.get(), temporary.c_str(),
        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (fd || errno != EEXIST)
      break;
  }
  if (!fd)
    return NativeError(
        "write", normalized, "failed to create temporary", errno);
  auto cleanup = [&] { unlinkat(parent.get(), temporary.c_str(), 0); };
  size_t offset = 0;
  while (offset < bytes.size()) {
    auto count = write(fd.get(), bytes.data() + offset, bytes.size() - offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0) {
      auto error = NativeError("write", normalized, "failed to write temporary",
          count < 0 ? errno : EIO);
      cleanup();
      return error;
    }
    offset += static_cast<size_t>(count);
  }
  // Check close errors before publishing; a failed write leaves the old file
  // intact.
  if (close(fd.release()) != 0) {
    auto error =
        NativeError("write", normalized, "failed to close temporary", errno);
    cleanup();
    return error;
  }
  if (renameat(parent.get(), temporary.c_str(), parent.get(), name.c_str()) !=
      0) {
    auto error = NativeError(
        "write", normalized, "failed to replace destination", errno);
    cleanup();
    return error;
  }
  return {};
}

FileStatus NativeRemoveFileUnderRoot(
    std::string_view root, std::string_view path) {
  std::string normalized;
  auto status = NormalizePath(path, false, &normalized);
  if (!status)
    return status;
  auto slash = normalized.rfind('/');
  auto parent_path =
      slash == std::string::npos ? "" : normalized.substr(0, slash);
  auto name = normalized.substr(slash == std::string::npos ? 0 : slash + 1);
  UniqueFd parent;
  status = OpenUnderRoot(
      root, parent_path, false, O_RDONLY | O_DIRECTORY, false, &parent);
  if (!status)
    return status;
  struct stat st{};
  if (fstatat(parent.get(), name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0)
    return NativeError("remove", normalized, "failed to stat file", errno);
  if (!S_ISREG(st.st_mode))
    return FileStatus::Error(
        FileStatusCode::kNotAFile, "remove", normalized, "not a regular file");
  if (unlinkat(parent.get(), name.c_str(), 0) != 0)
    return NativeError("remove", normalized, "failed to remove file", errno);
  return {};
}

FileStatus NativeReadFile(
    std::string_view host_path, std::vector<std::byte> *out) {
  if (!out) {
    return FileStatus::Error(FileStatusCode::kIoError, "read",
        std::string(host_path), "output argument is null");
  }
  out->clear();
  const std::string path(host_path);
  UniqueFd fd(open(path.c_str(), O_RDONLY | O_CLOEXEC));
  if (!fd) {
    return NativeError("read", path, "failed to open file", errno);
  }
  struct stat st{};
  if (fstat(fd.get(), &st) != 0) {
    return NativeError("read", path, "failed to stat file", errno);
  }
  if (!S_ISREG(st.st_mode)) {
    return FileStatus::Error(
        FileStatusCode::kNotAFile, "read", path, "not a regular file");
  }
  FileInfo info = InfoFromStat(path, st);
  out->assign(static_cast<size_t>(info.size), std::byte{});
  size_t offset = 0;
  while (offset < out->size()) {
    ssize_t count = read(fd.get(), out->data() + offset, out->size() - offset);
    if (count < 0) {
      return NativeError("read", path, "failed to read file", errno);
    }
    if (count == 0) {
      break;
    }
    offset += static_cast<size_t>(count);
  }
  if (offset != out->size()) {
    return FileStatus::Error(
        FileStatusCode::kIoError, "read", path, "short read");
  }
  return FileStatus::Ok();
}

} // namespace luna::file

#endif
