#include "file_vfs.h"

#include <algorithm>
#include <cctype>

namespace luna::file {
namespace {

bool MountMatches(std::string_view mount_path, std::string_view path) {
  if (mount_path.empty()) {
    return true;
  }
  return path == mount_path ||
      (path.size() > mount_path.size() &&
          path.compare(0, mount_path.size(), mount_path) == 0 &&
          path[mount_path.size()] == '/');
}

std::string RelativeToMount(
    std::string_view mount_path, std::string_view path) {
  if (mount_path.empty()) {
    return std::string(path);
  }
  if (path == mount_path) {
    return "";
  }
  return std::string(path.substr(mount_path.size() + 1));
}

bool SameFile(const FileInfo &a, const FileInfo &b) {
  return a.kind == b.kind && a.size == b.size &&
      a.modified_time_ns == b.modified_time_ns && a.source_id == b.source_id;
}

} // namespace

FileStatus FileStatus::Error(FileStatusCode code, std::string operation,
    std::string path, std::string message, int64_t native_code) {
  FileStatus status;
  status.code_ = code;
  status.operation_ = std::move(operation);
  status.path_ = std::move(path);
  status.message_ = std::move(message);
  status.native_code_ = native_code;
  return status;
}

FileStatus NormalizePath(
    std::string_view path, bool allow_root, std::string *out) {
  if (!out) {
    return FileStatus::Error(
        FileStatusCode::kIoError, "normalize", "", "output argument is null");
  }

  std::string input(path);
  std::replace(input.begin(), input.end(), '\\', '/');

  if (input.find('\0') != std::string::npos) {
    return FileStatus::Error(FileStatusCode::kInvalidPath, "normalize", input,
        "path contains a null byte");
  }

  if (input.empty()) {
    if (allow_root) {
      out->clear();
      return FileStatus::Ok();
    }
    return FileStatus::Error(
        FileStatusCode::kInvalidPath, "normalize", input, "path is empty");
  }

  if (input == "/" && allow_root) {
    out->clear();
    return FileStatus::Ok();
  }

  if (!input.empty() && input.front() == '/') {
    return FileStatus::Error(FileStatusCode::kInvalidPath, "normalize", input,
        "absolute paths are not allowed");
  }

  if (input.size() >= 2 && std::isalpha(static_cast<unsigned char>(input[0])) &&
      input[1] == ':') {
    return FileStatus::Error(FileStatusCode::kInvalidPath, "normalize", input,
        "drive paths are not allowed");
  }

  std::vector<std::string_view> parts;
  size_t begin = 0;
  while (begin <= input.size()) {
    const size_t end = input.find('/', begin);
    const std::string_view part(input.data() + begin,
        (end == std::string::npos ? input.size() : end) - begin);
    if (!part.empty() && part != ".") {
      if (part == "..") {
        return FileStatus::Error(FileStatusCode::kInvalidPath, "normalize",
            input, "path traversal is not allowed");
      }
      parts.push_back(part);
    }
    if (end == std::string::npos) {
      break;
    }
    begin = end + 1;
  }

  if (parts.empty()) {
    if (allow_root) {
      out->clear();
      return FileStatus::Ok();
    }
    return FileStatus::Error(FileStatusCode::kInvalidPath, "normalize", input,
        "path resolves to root");
  }

  out->clear();
  for (std::string_view part : parts) {
    if (!out->empty()) {
      out->push_back('/');
    }
    out->append(part);
  }
  return FileStatus::Ok();
}

MappedFile::MappedFile(const std::byte *data, uint64_t size, MappingKind kind,
    std::shared_ptr<const void> owner, FileInfo info)
    : data_(data), size_(size), kind_(kind), owner_(std::move(owner)),
      info_(std::move(info)) {}

MappedFile MappedFile::FromOwnedBuffer(
    std::vector<std::byte> bytes, FileInfo info) {
  auto owner = std::make_shared<const std::vector<std::byte>>(std::move(bytes));
  const std::byte *data = owner->empty() ? nullptr : owner->data();
  return MappedFile(data, static_cast<uint64_t>(owner->size()),
      MappingKind::kOwnedBuffer, std::static_pointer_cast<const void>(owner),
      std::move(info));
}

MappedFile MappedFile::FromBorrowedView(std::span<const std::byte> bytes,
    std::shared_ptr<const void> owner, FileInfo info) {
  return MappedFile(bytes.data(), static_cast<uint64_t>(bytes.size()),
      MappingKind::kBorrowedView, std::move(owner), std::move(info));
}

MappedFile MappedFile::FromNativeMapping(std::span<const std::byte> bytes,
    std::shared_ptr<const void> owner, FileInfo info) {
  return MappedFile(bytes.data(), static_cast<uint64_t>(bytes.size()),
      MappingKind::kNativeMapping, std::move(owner), std::move(info));
}

std::span<const std::byte> MappedFile::bytes() const noexcept {
  return std::span<const std::byte>(data_, static_cast<size_t>(size_));
}

bool Vfs::MountLess(const MountPoint &a, const MountPoint &b) {
  if (a.path.size() != b.path.size()) {
    return a.path.size() > b.path.size();
  }
  if (a.priority != b.priority) {
    return a.priority > b.priority;
  }
  return a.sequence < b.sequence;
}

std::vector<Vfs::MountPoint> Vfs::MountSnapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return mounts_;
}

FileStatus Vfs::Mount(std::string_view vfs_mount_path,
    std::shared_ptr<FileSource> source, const MountOptions &options) {
  if (!source) {
    return FileStatus::Error(FileStatusCode::kInvalidPath, "mount",
        std::string(vfs_mount_path), "source is null");
  }

  std::string normalized;
  FileStatus status = NormalizePath(vfs_mount_path, true, &normalized);
  if (!status) {
    return status;
  }

  MountPoint mount;
  mount.path = std::move(normalized);
  mount.source = std::move(source);
  mount.priority =
      options.priority.value_or(mount.source->DefaultMountPriority());
  {
    std::lock_guard<std::mutex> lock(mutex_);
    mount.sequence = next_sequence_++;
    mounts_.push_back(std::move(mount));
    std::sort(mounts_.begin(), mounts_.end(), &Vfs::MountLess);
    ClearCacheLocked();
  }
  return FileStatus::Ok();
}

bool Vfs::Unmount(
    std::string_view vfs_mount_path, std::string_view debug_name) {
  std::string normalized;
  if (!NormalizePath(vfs_mount_path, true, &normalized)) {
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  const auto old_size = mounts_.size();
  mounts_.erase(std::remove_if(mounts_.begin(), mounts_.end(),
                    [&](const MountPoint &mount) {
                      return mount.path == normalized &&
                          mount.source->DebugName() == debug_name;
                    }),
      mounts_.end());
  const bool removed = mounts_.size() != old_size;
  if (removed) {
    ClearCacheLocked();
  }
  return removed;
}

FileStatus Vfs::Stat(std::string_view path, FileInfo *out) {
  if (!out) {
    return FileStatus::Error(FileStatusCode::kIoError, "stat",
        std::string(path), "output argument is null");
  }

  std::string normalized;
  FileStatus status = NormalizePath(path, true, &normalized);
  if (!status) {
    return status;
  }

  for (const MountPoint &mount : MountSnapshot()) {
    if (!MountMatches(mount.path, normalized)) {
      continue;
    }
    FileStatus result =
        mount.source->Stat(RelativeToMount(mount.path, normalized), out);
    if (result || result.code() != FileStatusCode::kNotFound) {
      if (result) {
        out->path = normalized;
      }
      return result;
    }
  }

  return FileStatus::Error(
      FileStatusCode::kNotFound, "stat", normalized, "file not found");
}

FileStatus Vfs::List(std::string_view path, std::vector<FileInfo> *out) {
  if (!out) {
    return FileStatus::Error(FileStatusCode::kIoError, "list",
        std::string(path), "output argument is null");
  }

  std::string normalized;
  FileStatus status = NormalizePath(path, true, &normalized);
  if (!status) {
    return status;
  }

  for (const MountPoint &mount : MountSnapshot()) {
    if (!MountMatches(mount.path, normalized)) {
      continue;
    }
    FileStatus result =
        mount.source->List(RelativeToMount(mount.path, normalized), out);
    if (result || result.code() != FileStatusCode::kNotFound) {
      if (result && !mount.path.empty()) {
        for (auto &entry : *out)
          entry.path = mount.path + "/" + entry.path;
      }
      return result;
    }
  }

  return FileStatus::Error(
      FileStatusCode::kNotFound, "list", normalized, "file not found");
}

FileStatus Vfs::OpenFile(std::string_view path, const ReadOptions &options,
    std::unique_ptr<FileStream> *out) {
  if (!out) {
    return FileStatus::Error(FileStatusCode::kIoError, "open",
        std::string(path), "output argument is null");
  }

  std::string normalized;
  FileStatus status = NormalizePath(path, false, &normalized);
  if (!status) {
    return status;
  }

  for (const MountPoint &mount : MountSnapshot()) {
    if (!MountMatches(mount.path, normalized)) {
      continue;
    }
    FileStatus result = mount.source->OpenFile(
        RelativeToMount(mount.path, normalized), options, out);
    if (result || result.code() != FileStatusCode::kNotFound) {
      return result;
    }
  }

  return FileStatus::Error(
      FileStatusCode::kNotFound, "open", normalized, "file not found");
}

FileStatus Vfs::MapFile(
    std::string_view path, const ReadOptions &options, MappedFile *out) {
  if (!out) {
    return FileStatus::Error(FileStatusCode::kIoError, "map", std::string(path),
        "output argument is null");
  }

  std::string normalized;
  FileStatus status = NormalizePath(path, false, &normalized);
  if (!status) {
    return status;
  }

  const std::vector<MountPoint> mounts = MountSnapshot();

  auto cache_matches = [&](const CacheEntry &cached) {
    if (cached.options.prefer_native_mapping != options.prefer_native_mapping ||
        cached.options.allow_owned_fallback != options.allow_owned_fallback) {
      return false;
    }
    for (const MountPoint &mount : mounts) {
      if (!MountMatches(mount.path, normalized)) {
        continue;
      }

      std::string source_path = RelativeToMount(mount.path, normalized);
      FileInfo info;
      FileStatus stat = mount.source->Stat(source_path, &info);
      if (stat.code() == FileStatusCode::kNotFound) {
        continue;
      }
      return stat.ok() && cached.source == mount.source &&
          cached.source_path == source_path &&
          SameFile(cached.file.info(), info);
    }
    return false;
  };

  if (options.cache_policy == CachePolicy::kDefault) {
    std::optional<CacheEntry> cached;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = cache_.find(normalized);
      if (it != cache_.end()) {
        cached = it->second;
      }
    }

    if (cached.has_value()) {
      if (cache_matches(*cached)) {
        if (options.max_size != 0 && cached->file.size() > options.max_size) {
          return FileStatus::Error(FileStatusCode::kBudgetExceeded, "map",
              normalized, "file exceeds requested maximum size");
        }

        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_.find(normalized);
        if (it != cache_.end()) {
          cache_lru_.splice(cache_lru_.begin(), cache_lru_, it->second.lru);
          cache_stats_.hit_count += 1;
        }
        *out = std::move(cached->file);
        return FileStatus::Ok();
      }

      std::lock_guard<std::mutex> lock(mutex_);
      auto it = cache_.find(normalized);
      if (it != cache_.end() &&
          it->second.source.get() == cached->source.get() &&
          it->second.source_path == cached->source_path &&
          SameFile(it->second.file.info(), cached->file.info())) {
        EraseCacheLocked(it);
      }
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      cache_stats_.miss_count += 1;
    }
  }

  for (const MountPoint &mount : mounts) {
    if (!MountMatches(mount.path, normalized)) {
      continue;
    }
    const std::string source_path = RelativeToMount(mount.path, normalized);
    MappedFile mapped;
    FileStatus result = mount.source->MapFile(source_path, options, &mapped);
    if (result) {
      if (options.cache_policy == CachePolicy::kDefault) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cache_stats_.memory_budget == 0 ||
            mapped.size() <= cache_stats_.memory_budget) {
          auto existing = cache_.find(normalized);
          if (existing != cache_.end()) {
            EraseCacheLocked(existing);
          }

          cache_lru_.push_front(normalized);
          CacheEntry entry;
          entry.file = mapped;
          entry.source = mount.source;
          entry.source_path = source_path;
          entry.options = options;
          entry.lru = cache_lru_.begin();
          cache_stats_.resident_bytes += mapped.size();
          cache_.emplace(normalized, std::move(entry));
          cache_stats_.entry_count = static_cast<uint64_t>(cache_.size());
          TrimCacheLocked();
        }
      }
      *out = std::move(mapped);
      return FileStatus::Ok();
    }
    if (result.code() != FileStatusCode::kNotFound) {
      return result;
    }
  }

  return FileStatus::Error(
      FileStatusCode::kNotFound, "map", normalized, "file not found");
}

void Vfs::SetMemoryBudget(uint64_t bytes) {
  std::lock_guard<std::mutex> lock(mutex_);
  cache_stats_.memory_budget = bytes;
  TrimCacheLocked();
}

uint64_t Vfs::memory_budget() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return cache_stats_.memory_budget;
}

CacheStats Vfs::cache_stats() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return cache_stats_;
}

void Vfs::EraseCacheLocked(Cache::iterator entry) {
  cache_stats_.resident_bytes -= entry->second.file.size();
  cache_lru_.erase(entry->second.lru);
  cache_.erase(entry);
  cache_stats_.entry_count = static_cast<uint64_t>(cache_.size());
}

void Vfs::TrimCacheLocked() {
  if (cache_stats_.memory_budget != 0) {
    while (cache_stats_.resident_bytes > cache_stats_.memory_budget &&
        !cache_lru_.empty()) {
      EraseCacheLocked(cache_.find(cache_lru_.back()));
    }
  }
  cache_stats_.pinned_bytes = 0;
  cache_stats_.entry_count = static_cast<uint64_t>(cache_.size());
}

void Vfs::ClearCacheLocked() {
  cache_.clear();
  cache_lru_.clear();
  cache_stats_.resident_bytes = 0;
  cache_stats_.pinned_bytes = 0;
  cache_stats_.entry_count = 0;
}

void Vfs::TrimCache() {
  std::lock_guard<std::mutex> lock(mutex_);
  TrimCacheLocked();
}

void Vfs::ClearCache() {
  std::lock_guard<std::mutex> lock(mutex_);
  ClearCacheLocked();
}

} // namespace luna::file
