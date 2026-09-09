#include "native_file_source.h"
#include <atomic>

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace luna::file {
namespace {

FileStatusCode CodeFromWin32(DWORD value) {
  switch (value) {
  case ERROR_FILE_NOT_FOUND:
  case ERROR_PATH_NOT_FOUND:
    return FileStatusCode::kNotFound;
  case ERROR_ACCESS_DENIED:
    return FileStatusCode::kPermissionDenied;
  default:
    return FileStatusCode::kIoError;
  }
}

FileStatus Win32Error(std::string operation, std::string path,
    std::string message, DWORD value = GetLastError()) {
  return FileStatus::Error(CodeFromWin32(value), std::move(operation),
      std::move(path), std::move(message), static_cast<int64_t>(value));
}

std::wstring Utf8ToWide(std::string_view text) {
  if (text.empty()) {
    return {};
  }
  int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
      static_cast<int>(text.size()), nullptr, 0);
  if (count <= 0) {
    return {};
  }
  std::wstring wide(static_cast<size_t>(count), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
      static_cast<int>(text.size()), wide.data(), count);
  return wide;
}

uint64_t FileTimeNs(const FILETIME &time) {
  ULARGE_INTEGER value;
  value.LowPart = time.dwLowDateTime;
  value.HighPart = time.dwHighDateTime;
  return value.QuadPart * 100ull;
}

class UniqueHandle {
public:
  UniqueHandle() = default;
  explicit UniqueHandle(HANDLE handle) : handle_(handle) {}
  ~UniqueHandle() {
    if (handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr) {
      CloseHandle(handle_);
    }
  }
  UniqueHandle(const UniqueHandle &) = delete;
  UniqueHandle &operator=(const UniqueHandle &) = delete;
  UniqueHandle(UniqueHandle &&other) noexcept : handle_(other.handle_) {
    other.handle_ = INVALID_HANDLE_VALUE;
  }
  UniqueHandle &operator=(UniqueHandle &&other) noexcept {
    if (this != &other) {
      if (handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr) {
        CloseHandle(handle_);
      }
      handle_ = other.handle_;
      other.handle_ = INVALID_HANDLE_VALUE;
    }
    return *this;
  }
  HANDLE get() const noexcept { return handle_; }
  explicit operator bool() const noexcept {
    return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr;
  }

private:
  HANDLE handle_ = INVALID_HANDLE_VALUE;
};

FileStatus HandlePath(HANDLE handle, std::wstring *out) {
  DWORD size =
      GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED);
  if (!size) {
    return Win32Error("open", "", "failed to resolve directory handle");
  }
  out->resize(size);
  DWORD length = GetFinalPathNameByHandleW(
      handle, out->data(), size, FILE_NAME_NORMALIZED);
  if (!length || length >= size) {
    return Win32Error("open", "", "failed to resolve directory handle");
  }
  out->resize(length);
  return FileStatus::Ok();
}

// Keep directories open without write/delete sharing through the final open.
// This prevents replacing a component or changing its reparse data after
// checking it.
FileStatus OpenUnderRoot(std::string_view root, std::string_view path,
    bool follow_symlinks, DWORD access, bool create_parents,
    std::vector<UniqueHandle> *parents, UniqueHandle *out,
    bool exclusive = false) {
  std::string normalized;
  FileStatus status = NormalizePath(path, true, &normalized);
  if (!status) {
    return status;
  }
  UniqueHandle current(
      CreateFileW(Utf8ToWide(root.empty() ? "." : root).c_str(),
          FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
          FILE_FLAG_BACKUP_SEMANTICS, nullptr));
  if (!current) {
    return Win32Error("open", std::string(root), "failed to open root");
  }
  std::wstring host;
  status = HandlePath(current.get(), &host);
  if (!status) {
    return status;
  }
  if (normalized.empty()) {
    *out = std::move(current);
    return FileStatus::Ok();
  }
  size_t begin = 0;
  while (true) {
    parents->push_back(std::move(current));
    const size_t end = normalized.find('/', begin);
    const bool last = end == std::string::npos;
    const std::string part =
        normalized.substr(begin, last ? std::string::npos : end - begin);
    // Reject alternate streams and Win32 component aliases.
    if (part.find(':') != std::string::npos || part.back() == '.' ||
        part.back() == ' ') {
      return FileStatus::Error(FileStatusCode::kInvalidPath, "open", normalized,
          "invalid Windows path component");
    }
    const std::wstring wide_part = Utf8ToWide(part);
    if (wide_part.empty()) {
      return FileStatus::Error(FileStatusCode::kInvalidPath, "open", normalized,
          "path is not valid UTF-8");
    }
    host += L"\\" + wide_part;
    if (!last && create_parents && !CreateDirectoryW(host.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
      return Win32Error("mkdir", normalized, "failed to create directory");
    }
    current = UniqueHandle(
        CreateFileW(host.c_str(), last ? access : FILE_READ_ATTRIBUTES,
            last && access == GENERIC_READ ? FILE_SHARE_READ | FILE_SHARE_DELETE
                : last && (access & GENERIC_WRITE) ? 0
                                                   : FILE_SHARE_READ,
            nullptr,
            last && exclusive            ? CREATE_NEW
                : last && create_parents ? OPEN_ALWAYS
                                         : OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS |
                (follow_symlinks ? 0 : FILE_FLAG_OPEN_REPARSE_POINT),
            nullptr));
    if (!current) {
      return Win32Error("open", normalized, "failed to open path component");
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(current.get(), &info)) {
      return Win32Error("stat", normalized, "failed to stat path component");
    }
    if (!follow_symlinks &&
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
      return FileStatus::Error(FileStatusCode::kInvalidPath, "open", normalized,
          "reparse points are not allowed");
    }
    if (!last && !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
      return FileStatus::Error(FileStatusCode::kNotADirectory, "open",
          normalized, "not a directory");
    }
    if (last) {
      *out = std::move(current);
      return FileStatus::Ok();
    }
    begin = end + 1;
  }
}

class MappingOwner {
public:
  MappingOwner(void *view, UniqueHandle mapping, UniqueHandle file)
      : view_(view), mapping_(std::move(mapping)), file_(std::move(file)) {}
  ~MappingOwner() {
    if (view_) {
      UnmapViewOfFile(view_);
    }
  }

private:
  void *view_ = nullptr;
  UniqueHandle mapping_;
  UniqueHandle file_;
};

FileInfo InfoFromData(std::string path, const WIN32_FILE_ATTRIBUTE_DATA &data) {
  FileInfo info;
  info.path = std::move(path);
  info.kind = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
      ? FileKind::kDirectory
      : FileKind::kFile;
  ULARGE_INTEGER size;
  size.LowPart = data.nFileSizeLow;
  size.HighPart = data.nFileSizeHigh;
  info.size = info.kind == FileKind::kFile ? size.QuadPart : 0;
  info.modified_time_ns = FileTimeNs(data.ftLastWriteTime);
  return info;
}

FileStatus InfoFromHandle(HANDLE handle, std::string path, FileInfo *out) {
  BY_HANDLE_FILE_INFORMATION data{};
  if (!GetFileInformationByHandle(handle, &data)) {
    return Win32Error("stat", path, "failed to stat file");
  }
  WIN32_FILE_ATTRIBUTE_DATA attributes{};
  attributes.dwFileAttributes = data.dwFileAttributes;
  attributes.ftLastWriteTime = data.ftLastWriteTime;
  attributes.nFileSizeHigh = data.nFileSizeHigh;
  attributes.nFileSizeLow = data.nFileSizeLow;
  *out = InfoFromData(std::move(path), attributes);
  out->source_id =
      (static_cast<uint64_t>(data.nFileIndexHigh) << 32) | data.nFileIndexLow;
  return FileStatus::Ok();
}

class NativeFileStream final : public FileStream {
public:
  NativeFileStream(UniqueHandle handle, FileInfo info)
      : handle_(std::move(handle)), info_(std::move(info)) {}

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
    DWORD count = 0;
    if (!ReadFile(handle_.get(), output.data(),
            static_cast<DWORD>(std::min<size_t>(
                output.size(), std::numeric_limits<DWORD>::max())),
            &count, nullptr)) {
      return Win32Error("read", info_.path, "failed to read file");
    }
    *bytes_read = count;
    position_ += count;
    eof_ = count == 0 || position_ >= info_.size;
    return FileStatus::Ok();
  }

  FileStatus Seek(int64_t offset, std::ios_base::seekdir whence) override {
    LARGE_INTEGER distance;
    distance.QuadPart = offset;
    DWORD method = FILE_BEGIN;
    if (whence == std::ios_base::cur) {
      method = FILE_CURRENT;
    } else if (whence == std::ios_base::end) {
      method = FILE_END;
    }
    LARGE_INTEGER next;
    if (!SetFilePointerEx(handle_.get(), distance, &next, method)) {
      return Win32Error("seek", info_.path, "failed to seek file");
    }
    position_ = static_cast<uint64_t>(next.QuadPart);
    eof_ = false;
    return FileStatus::Ok();
  }

private:
  UniqueHandle handle_;
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
    std::vector<UniqueHandle> parents;
    UniqueHandle handle;
    status = OpenUnderRoot(root_, normalized, options_.follow_symlinks,
        FILE_READ_ATTRIBUTES, false, &parents, &handle);
    if (!status) {
      return status;
    }
    return InfoFromHandle(handle.get(), normalized, out);
  }

  FileStatus List(std::string_view path, std::vector<FileInfo> *out) override {
    if (!out) {
      return FileStatus::Error(FileStatusCode::kIoError, "list",
          std::string(path), "output argument is null");
    }
    out->clear();
    FileInfo dir;
    FileStatus status = Stat(path, &dir);
    if (!status) {
      return status;
    }
    if (dir.kind != FileKind::kDirectory) {
      return FileStatus::Error(
          FileStatusCode::kNotADirectory, "list", dir.path, "not a directory");
    }
    std::vector<UniqueHandle> parents;
    UniqueHandle directory;
    status = OpenUnderRoot(root_, dir.path, options_.follow_symlinks,
        FILE_READ_ATTRIBUTES, false, &parents, &directory);
    if (!status) {
      return status;
    }
    std::wstring pattern;
    status = HandlePath(directory.get(), &pattern);
    if (!status) {
      return status;
    }
    pattern += L"\\*";
    WIN32_FIND_DATAW data{};
    HANDLE raw = FindFirstFileW(pattern.c_str(), &data);
    if (raw == INVALID_HANDLE_VALUE) {
      return Win32Error("list", dir.path, "failed to list directory");
    }
    // FindFirstFile handles must be released with FindClose.
    std::unique_ptr<void, decltype(&FindClose)> handle(raw, &FindClose);
    do {
      char name_utf8[MAX_PATH * 4] = {};
      WideCharToMultiByte(CP_UTF8, 0, data.cFileName, -1, name_utf8,
          sizeof(name_utf8), nullptr, nullptr);
      std::string name(name_utf8);
      if (name == "." || name == "..") {
        continue;
      }
      std::string child = dir.path.empty() ? name : dir.path + "/" + name;
      FileInfo info;
      if (Stat(child, &info)) {
        out->push_back(std::move(info));
      }
    } while (FindNextFileW(handle.get(), &data));
    const DWORD error = GetLastError();
    if (error != ERROR_NO_MORE_FILES) {
      return Win32Error("list", dir.path, "failed to list directory", error);
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
    std::vector<UniqueHandle> parents;
    UniqueHandle handle;
    status = OpenUnderRoot(root_, normalized, options_.follow_symlinks,
        GENERIC_READ, false, &parents, &handle);
    if (!status) {
      return status;
    }
    FileInfo info;
    status = InfoFromHandle(handle.get(), normalized, &info);
    if (!status) {
      return status;
    }
    if (info.kind != FileKind::kFile) {
      return FileStatus::Error(
          FileStatusCode::kIsDirectory, "open", info.path, "is a directory");
    }
    if (options.max_size != 0 && info.size > options.max_size) {
      return FileStatus::Error(FileStatusCode::kBudgetExceeded, "open",
          info.path, "file exceeds requested maximum size");
    }
    *out =
        std::make_unique<NativeFileStream>(std::move(handle), std::move(info));
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
    std::vector<UniqueHandle> parents;
    UniqueHandle handle;
    status = OpenUnderRoot(root_, normalized, options_.follow_symlinks,
        GENERIC_READ, false, &parents, &handle);
    if (!status) {
      return status;
    }
    FileInfo info;
    status = InfoFromHandle(handle.get(), normalized, &info);
    if (!status) {
      return status;
    }
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
    UniqueHandle file = std::move(handle);
    if (options.prefer_native_mapping && options_.prefer_native_mapping) {
      UniqueHandle mapping(CreateFileMappingW(
          file.get(), nullptr, PAGE_READONLY, 0, 0, nullptr));
      if (mapping) {
        void *view = MapViewOfFile(mapping.get(), FILE_MAP_READ, 0, 0, 0);
        if (view) {
          auto owner = std::make_shared<MappingOwner>(
              view, std::move(mapping), std::move(file));
          *out = MappedFile::FromNativeMapping(
              std::span<const std::byte>(static_cast<const std::byte *>(view),
                  static_cast<size_t>(info.size)),
              std::static_pointer_cast<const void>(owner), std::move(info));
          return FileStatus::Ok();
        }
      }
      if (!options.allow_owned_fallback) {
        return Win32Error("map", info.path, "failed to map file");
      }
    }
    if (!options.allow_owned_fallback) {
      return FileStatus::Error(FileStatusCode::kUnsupported, "map", info.path,
          "native mapping is disabled and owned fallback is forbidden");
    }
    NativeFileStream stream(std::move(file), info);
    std::vector<std::byte> bytes(static_cast<size_t>(info.size));
    size_t offset = 0;
    while (offset < bytes.size()) {
      size_t count = 0;
      status = stream.Read(std::span<std::byte>(bytes).subspan(offset), &count);
      if (!status) {
        return status;
      }
      if (count == 0) {
        return FileStatus::Error(
            FileStatusCode::kIoError, "read", info.path, "short read");
      }
      offset += count;
    }
    *out = MappedFile::FromOwnedBuffer(std::move(bytes), std::move(info));
    return FileStatus::Ok();
  }

private:
  std::string root_;
  NativeSourceOptions options_;
};

std::string ParentPath(std::string_view path) {
  const size_t slash = path.find_last_of("/\\");
  if (slash == std::string_view::npos) {
    return "";
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
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!GetFileAttributesExW(Utf8ToWide(root.empty() ? "." : root).c_str(),
          GetFileExInfoStandard, &data)) {
    return Win32Error("open_native", root, "failed to stat root");
  }
  if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
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
  const std::wstring input = Utf8ToWide(host_path);
  DWORD size = GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
  if (!size) {
    return Win32Error(
        "mkdir", std::string(host_path), "failed to resolve path");
  }
  std::wstring full(size, L'\0');
  DWORD length = GetFullPathNameW(input.c_str(), size, full.data(), nullptr);
  if (!length || length >= size) {
    return Win32Error(
        "mkdir", std::string(host_path), "failed to resolve path");
  }
  full.resize(length);
  std::replace(full.begin(), full.end(), L'/', L'\\');
  size_t begin = 0;
  const size_t colon = full.find(L':');
  if (colon != std::wstring::npos) {
    begin =
        colon + 2; // Skip the drive root, including an extended-path prefix.
  } else if (full.starts_with(L"\\\\")) {
    // A UNC server/share is an existing root, not a directory to create.
    const size_t server_begin = full.starts_with(L"\\\\?\\UNC\\") ? 8 : 2;
    const size_t server_end = full.find(L'\\', server_begin);
    const size_t share_end = server_end == std::wstring::npos
        ? std::wstring::npos
        : full.find(L'\\', server_end + 1);
    begin = share_end == std::wstring::npos ? full.size() : share_end + 1;
  }
  while (begin < full.size()) {
    const size_t end = full.find(L'\\', begin);
    const std::wstring current = full.substr(0, end);
    if (!CreateDirectoryW(current.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
      return Win32Error(
          "mkdir", std::string(host_path), "failed to create directory");
    }
    if (end == std::wstring::npos) {
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
  UniqueHandle handle(CreateFileW(Utf8ToWide(path).c_str(), GENERIC_WRITE, 0,
      nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!handle) {
    return Win32Error("write", path, "failed to open file");
  }
  size_t offset = 0;
  while (offset < bytes.size()) {
    DWORD written = 0;
    if (!WriteFile(handle.get(), bytes.data() + offset,
            static_cast<DWORD>(std::min<size_t>(
                bytes.size() - offset, std::numeric_limits<DWORD>::max())),
            &written, nullptr)) {
      return Win32Error("write", path, "failed to write file");
    }
    offset += written;
  }
  return FileStatus::Ok();
}

FileStatus NativeAtomicWriteUnderRoot(std::string_view root,
    std::string_view path, std::span<const std::byte> bytes) {
  std::string normalized;
  auto status = NormalizePath(path, false, &normalized);
  if (!status)
    return status;
  const auto slash = normalized.rfind('/');
  const auto prefix =
      slash == std::string::npos ? "" : normalized.substr(0, slash + 1);
  const auto name = normalized.substr(prefix.size());
  // Validate the final component using the same rules as OpenUnderRoot.
  if (name.find(':') != std::string::npos || name.back() == '.' ||
      name.back() == ' ')
    return FileStatus::Error(FileStatusCode::kInvalidPath, "write", normalized,
        "invalid Windows component");
  static std::atomic<uint64_t> serial{0};
  std::vector<UniqueHandle> parents;
  UniqueHandle handle;
  for (int attempt = 0; attempt < 128; ++attempt) {
    parents.clear();
    auto temporary = prefix + ".luna-write-" +
        std::to_string(GetCurrentProcessId()) + "-" + std::to_string(serial++);
    status = OpenUnderRoot(root, temporary, false, GENERIC_WRITE | DELETE, true,
        &parents, &handle, true);
    if (status || status.native_code() != ERROR_FILE_EXISTS)
      break;
  }
  if (!status)
    return status;
  auto cleanup = [&] {
    FILE_DISPOSITION_INFO info{TRUE};
    SetFileInformationByHandle(
        handle.get(), FileDispositionInfo, &info, sizeof(info));
  };
  std::wstring parent_path;
  status = HandlePath(parents.back().get(), &parent_path);
  if (!status) {
    cleanup();
    return status;
  }
  const auto destination = parent_path + L"\\" + Utf8ToWide(name);
  const DWORD attributes = GetFileAttributesW(destination.c_str());
  if (attributes != INVALID_FILE_ATTRIBUTES &&
      (attributes &
          (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
    cleanup();
    return FileStatus::Error(
        FileStatusCode::kNotAFile, "write", normalized, "not a regular file");
  }
  size_t offset = 0;
  while (offset < bytes.size()) {
    DWORD written = 0;
    if (!WriteFile(handle.get(), bytes.data() + offset,
            static_cast<DWORD>(
                std::min<size_t>(bytes.size() - offset, MAXDWORD)),
            &written, nullptr) ||
        !written) {
      auto error = Win32Error("write", normalized, "failed to write temporary");
      cleanup();
      return error;
    }
    offset += written;
  }
  const size_t length = destination.size() * sizeof(wchar_t);
  std::vector<std::byte> buffer(sizeof(FILE_RENAME_INFO) + length);
  auto *rename = reinterpret_cast<FILE_RENAME_INFO *>(buffer.data());
  rename->ReplaceIfExists = TRUE;
  rename->RootDirectory = nullptr;
  rename->FileNameLength = static_cast<DWORD>(length);
  std::memcpy(rename->FileName, destination.data(), length);
  if (!SetFileInformationByHandle(handle.get(), FileRenameInfo, rename,
          static_cast<DWORD>(buffer.size()))) {
    auto error =
        Win32Error("write", normalized, "failed to replace destination");
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
  std::vector<UniqueHandle> parents;
  UniqueHandle handle;
  status = OpenUnderRoot(root, normalized, false, DELETE | FILE_READ_ATTRIBUTES,
      false, &parents, &handle);
  if (!status)
    return status;
  FileInfo info;
  status = InfoFromHandle(handle.get(), normalized, &info);
  if (!status)
    return status;
  if (info.kind != FileKind::kFile)
    return FileStatus::Error(
        FileStatusCode::kNotAFile, "remove", normalized, "not a regular file");
  FILE_DISPOSITION_INFO disposition{TRUE};
  if (!SetFileInformationByHandle(
          handle.get(), FileDispositionInfo, &disposition, sizeof(disposition)))
    return Win32Error("remove", normalized, "failed to remove file");
  return {};
}

FileStatus NativeReadFile(
    std::string_view host_path, std::vector<std::byte> *out) {
  if (!out) {
    return FileStatus::Error(FileStatusCode::kIoError, "read",
        std::string(host_path), "output argument is null");
  }
  std::shared_ptr<FileSource> source;
  FileStatus status = OpenNativeFileSource(ParentPath(host_path), {}, &source);
  if (!status) {
    return status;
  }
  MappedFile mapped;
  const std::string name(
      std::string(host_path).substr(ParentPath(host_path).size() + 1));
  status = source->MapFile(name, {}, &mapped);
  if (!status) {
    return status;
  }
  out->assign(mapped.bytes().begin(), mapped.bytes().end());
  return FileStatus::Ok();
}

} // namespace luna::file

#endif
