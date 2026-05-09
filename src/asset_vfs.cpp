#include "asset_vfs.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <limits>
#include <system_error>

namespace luna::asset {
namespace {

AssetError MakeError(
    AssetErrorCode code, std::string path, std::string message) {
  return AssetError{code, std::move(path), std::move(message)};
}

AssetResult<std::string> NormalizePath(
    std::string_view path, bool allow_root) {
  std::string input(path);
  std::replace(input.begin(), input.end(), '\\', '/');

  if (input.empty()) {
    if (allow_root) {
      return AssetResult<std::string>::Ok("");
    }
    return AssetResult<std::string>::Err(
        MakeError(AssetErrorCode::kInvalidPath, input, "path is empty"));
  }

  if (input == "/" && allow_root) {
    return AssetResult<std::string>::Ok("");
  }

  if (!input.empty() && input.front() == '/') {
    return AssetResult<std::string>::Err(MakeError(
        AssetErrorCode::kInvalidPath, input, "absolute paths are not allowed"));
  }

  if (input.size() >= 2 && std::isalpha(static_cast<unsigned char>(input[0])) &&
      input[1] == ':') {
    return AssetResult<std::string>::Err(MakeError(
        AssetErrorCode::kInvalidPath, input, "drive paths are not allowed"));
  }

  std::vector<std::string_view> parts;
  size_t begin = 0;
  while (begin <= input.size()) {
    const size_t end = input.find('/', begin);
    const std::string_view part(input.data() + begin,
        (end == std::string::npos ? input.size() : end) - begin);
    if (!part.empty() && part != ".") {
      if (part == "..") {
        return AssetResult<std::string>::Err(MakeError(
            AssetErrorCode::kInvalidPath, input,
            "path traversal is not allowed"));
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
      return AssetResult<std::string>::Ok("");
    }
    return AssetResult<std::string>::Err(MakeError(
        AssetErrorCode::kInvalidPath, input, "path resolves to root"));
  }

  std::string normalized;
  for (std::string_view part : parts) {
    if (!normalized.empty()) {
      normalized.push_back('/');
    }
    normalized.append(part);
  }
  return AssetResult<std::string>::Ok(std::move(normalized));
}

bool MountMatches(std::string_view mount_path, std::string_view path) {
  if (mount_path.empty()) {
    return true;
  }
  return path == mount_path ||
         (path.size() > mount_path.size() &&
             path.compare(0, mount_path.size(), mount_path) == 0 &&
             path[mount_path.size()] == '/');
}

std::string RelativeToMount(std::string_view mount_path, std::string_view path) {
  if (mount_path.empty()) {
    return std::string(path);
  }
  if (path == mount_path) {
    return "";
  }
  return std::string(path.substr(mount_path.size() + 1));
}

uint64_t FileTimeToNs(std::filesystem::file_time_type time) {
  const auto duration = time.time_since_epoch();
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());
}

class NativeAssetStream final : public AssetStream {
public:
  NativeAssetStream(std::ifstream file, AssetInfo info)
      : file_(std::move(file)), info_(std::move(info)) {}

  const AssetInfo &info() const noexcept override { return info_; }
  uint64_t position() const noexcept override { return position_; }
  bool eof() const noexcept override { return eof_; }

  AssetResult<size_t> Read(std::span<std::byte> output) override {
    if (output.empty()) {
      return AssetResult<size_t>::Ok(0);
    }

    file_.read(reinterpret_cast<char *>(output.data()),
        static_cast<std::streamsize>(output.size()));
    const std::streamsize read_count = file_.gcount();
    if (read_count > 0) {
      position_ += static_cast<uint64_t>(read_count);
    }
    eof_ = file_.eof();
    if (file_.bad()) {
      return AssetResult<size_t>::Err(MakeError(
          AssetErrorCode::kIoError, info_.path, "failed to read stream"));
    }
    return AssetResult<size_t>::Ok(static_cast<size_t>(read_count));
  }

  AssetResult<void> Seek(
      int64_t offset, std::ios_base::seekdir whence) override {
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(offset), whence);
    if (!file_) {
      return AssetResult<void>::Err(MakeError(
          AssetErrorCode::kIoError, info_.path, "failed to seek stream"));
    }
    const std::streampos pos = file_.tellg();
    if (pos < 0) {
      return AssetResult<void>::Err(MakeError(
          AssetErrorCode::kIoError, info_.path, "failed to tell stream"));
    }
    position_ = static_cast<uint64_t>(pos);
    eof_ = false;
    return AssetResult<void>::Ok();
  }

private:
  std::ifstream file_;
  AssetInfo info_;
  uint64_t position_ = 0;
  bool eof_ = false;
};

class NativeAssetSource final : public ReadOnlyAssetSource {
public:
  NativeAssetSource(std::filesystem::path root, NativeSourceOptions options)
      : root_(std::move(root)), options_(options) {}

  std::string_view DebugName() const noexcept override { return debug_name_; }
  int DefaultMountPriority() const noexcept override {
    return kNativeMountPriority;
  }

  AssetResult<AssetInfo> Stat(std::string_view path) override {
    auto normalized = NormalizePath(path, true);
    if (!normalized) {
      return AssetResult<AssetInfo>::Err(std::move(normalized).error());
    }

    const std::filesystem::path full_path = Resolve(normalized.value());
    std::error_code ec;
    const std::filesystem::file_status status =
        options_.follow_symlinks ? std::filesystem::status(full_path, ec)
                                 : std::filesystem::symlink_status(full_path, ec);
    if (ec || !std::filesystem::exists(status)) {
      return AssetResult<AssetInfo>::Err(MakeError(
          AssetErrorCode::kNotFound, normalized.value(), "file not found"));
    }

    AssetInfo info;
    info.path = normalized.value();
    if (std::filesystem::is_directory(status)) {
      info.kind = EntryKind::kDirectory;
    } else if (std::filesystem::is_regular_file(status)) {
      info.kind = EntryKind::kFile;
      info.size = std::filesystem::file_size(full_path, ec);
      if (ec) {
        return AssetResult<AssetInfo>::Err(MakeError(
            AssetErrorCode::kIoError, info.path, ec.message()));
      }
    } else {
      return AssetResult<AssetInfo>::Err(MakeError(
          AssetErrorCode::kNotAFile, normalized.value(), "not a regular file"));
    }

    const auto modified = std::filesystem::last_write_time(full_path, ec);
    if (!ec) {
      info.modified_time_ns = FileTimeToNs(modified);
    }
    return AssetResult<AssetInfo>::Ok(std::move(info));
  }

  AssetResult<std::vector<AssetInfo>> List(std::string_view path) override {
    auto normalized = NormalizePath(path, true);
    if (!normalized) {
      return AssetResult<std::vector<AssetInfo>>::Err(
          std::move(normalized).error());
    }

    auto info = Stat(normalized.value());
    if (!info) {
      return AssetResult<std::vector<AssetInfo>>::Err(std::move(info).error());
    }
    if (info.value().kind != EntryKind::kDirectory) {
      return AssetResult<std::vector<AssetInfo>>::Err(MakeError(
          AssetErrorCode::kNotADirectory, normalized.value(),
          "not a directory"));
    }

    std::vector<AssetInfo> entries;
    std::error_code ec;
    for (const auto &entry :
        std::filesystem::directory_iterator(Resolve(normalized.value()), ec)) {
      if (ec) {
        return AssetResult<std::vector<AssetInfo>>::Err(MakeError(
            AssetErrorCode::kIoError, normalized.value(), ec.message()));
      }
      const std::string name = entry.path().filename().generic_string();
      const std::string child_path =
          normalized.value().empty() ? name : normalized.value() + "/" + name;
      auto child = Stat(child_path);
      if (child) {
        entries.push_back(std::move(child).value());
      }
    }
    std::sort(entries.begin(), entries.end(),
        [](const AssetInfo &a, const AssetInfo &b) { return a.path < b.path; });
    return AssetResult<std::vector<AssetInfo>>::Ok(std::move(entries));
  }

  AssetResult<std::unique_ptr<AssetStream>> OpenFile(
      std::string_view path, const ReadOptions &options) override {
    auto info = Stat(path);
    if (!info) {
      return AssetResult<std::unique_ptr<AssetStream>>::Err(
          std::move(info).error());
    }
    if (info.value().kind != EntryKind::kFile) {
      return AssetResult<std::unique_ptr<AssetStream>>::Err(MakeError(
          AssetErrorCode::kIsDirectory, info.value().path, "is a directory"));
    }
    if (options.max_size != 0 && info.value().size > options.max_size) {
      return AssetResult<std::unique_ptr<AssetStream>>::Err(MakeError(
          AssetErrorCode::kBudgetExceeded, info.value().path,
          "file exceeds requested maximum size"));
    }

    std::ifstream file(Resolve(info.value().path), std::ios::binary);
    if (!file) {
      return AssetResult<std::unique_ptr<AssetStream>>::Err(MakeError(
          AssetErrorCode::kIoError, info.value().path, "failed to open file"));
    }

    std::unique_ptr<AssetStream> stream =
        std::make_unique<NativeAssetStream>(
            std::move(file), std::move(info).value());
    return AssetResult<std::unique_ptr<AssetStream>>::Ok(std::move(stream));
  }

  AssetResult<MappedAsset> MapFile(
      std::string_view path, const ReadOptions &options) override {
    auto info = Stat(path);
    if (!info) {
      return AssetResult<MappedAsset>::Err(std::move(info).error());
    }
    if (info.value().kind != EntryKind::kFile) {
      return AssetResult<MappedAsset>::Err(MakeError(
          AssetErrorCode::kIsDirectory, info.value().path, "is a directory"));
    }
    if (options.max_size != 0 && info.value().size > options.max_size) {
      return AssetResult<MappedAsset>::Err(MakeError(
          AssetErrorCode::kBudgetExceeded, info.value().path,
          "file exceeds requested maximum size"));
    }
    if (info.value().size >
        static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
      return AssetResult<MappedAsset>::Err(MakeError(
          AssetErrorCode::kBudgetExceeded, info.value().path,
          "file exceeds addressable memory size"));
    }

    std::ifstream file(Resolve(info.value().path), std::ios::binary);
    if (!file) {
      return AssetResult<MappedAsset>::Err(MakeError(
          AssetErrorCode::kIoError, info.value().path, "failed to open file"));
    }

    std::vector<std::byte> bytes(static_cast<size_t>(info.value().size));
    if (!bytes.empty()) {
      file.read(reinterpret_cast<char *>(bytes.data()),
          static_cast<std::streamsize>(bytes.size()));
      if (!file.good()) {
        return AssetResult<MappedAsset>::Err(MakeError(
            AssetErrorCode::kIoError, info.value().path, "failed to read file"));
      }
    }
    return AssetResult<MappedAsset>::Ok(
        MappedAsset::FromOwnedBuffer(std::move(bytes), std::move(info).value()));
  }

private:
  std::filesystem::path Resolve(std::string_view normalized) const {
    std::filesystem::path path = root_;
    if (!normalized.empty()) {
      path /= std::filesystem::path(std::string(normalized));
    }
    return path;
  }

  std::filesystem::path root_;
  NativeSourceOptions options_;
  std::string debug_name_ = "native";
};

} // namespace

MappedAsset::MappedAsset(const std::byte *data, uint64_t size, MappingKind kind,
    std::shared_ptr<const void> owner, AssetInfo info)
    : data_(data),
      size_(size),
      kind_(kind),
      owner_(std::move(owner)),
      info_(std::move(info)) {}

MappedAsset MappedAsset::FromOwnedBuffer(
    std::vector<std::byte> bytes, AssetInfo info) {
  auto owner = std::make_shared<const std::vector<std::byte>>(std::move(bytes));
  const std::byte *data = owner->empty() ? nullptr : owner->data();
  const uint64_t size = static_cast<uint64_t>(owner->size());
  return MappedAsset(data, size, MappingKind::kOwnedBuffer,
      std::static_pointer_cast<const void>(owner), std::move(info));
}

MappedAsset MappedAsset::FromBorrowedView(std::span<const std::byte> bytes,
    std::shared_ptr<const void> owner, AssetInfo info) {
  return MappedAsset(bytes.data(), static_cast<uint64_t>(bytes.size()),
      MappingKind::kBorrowedView, std::move(owner), std::move(info));
}

MappedAsset MappedAsset::FromOsFileMapping(std::span<const std::byte> bytes,
    std::shared_ptr<const void> owner, AssetInfo info) {
  return MappedAsset(bytes.data(), static_cast<uint64_t>(bytes.size()),
      MappingKind::kOsFileMapping, std::move(owner), std::move(info));
}

std::span<const std::byte> MappedAsset::bytes() const noexcept {
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

AssetResult<void> Vfs::Mount(std::string vfs_mount_path,
    std::shared_ptr<ReadOnlyAssetSource> source, MountOptions options) {
  if (!source) {
    return AssetResult<void>::Err(MakeError(
        AssetErrorCode::kInvalidPath, vfs_mount_path, "source is null"));
  }

  auto normalized = NormalizePath(vfs_mount_path, true);
  if (!normalized) {
    return AssetResult<void>::Err(std::move(normalized).error());
  }

  MountPoint mount;
  mount.path = std::move(normalized).value();
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
  return AssetResult<void>::Ok();
}

bool Vfs::Unmount(
    std::string_view vfs_mount_path, std::string_view debug_name) {
  auto normalized = NormalizePath(vfs_mount_path, true);
  if (!normalized) {
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  const auto old_size = mounts_.size();
  mounts_.erase(std::remove_if(mounts_.begin(), mounts_.end(),
                    [&](const MountPoint &mount) {
                      return mount.path == normalized.value() &&
                             mount.source->DebugName() == debug_name;
                    }),
      mounts_.end());
  const bool removed = mounts_.size() != old_size;
  if (removed) {
    ClearCacheLocked();
  }
  return removed;
}

AssetResult<AssetInfo> Vfs::Stat(std::string_view path) {
  auto normalized = NormalizePath(path, true);
  if (!normalized) {
    return AssetResult<AssetInfo>::Err(std::move(normalized).error());
  }

  for (const MountPoint &mount : MountSnapshot()) {
    if (!MountMatches(mount.path, normalized.value())) {
      continue;
    }
    auto result = mount.source->Stat(
        RelativeToMount(mount.path, normalized.value()));
    if (result || result.error().code != AssetErrorCode::kNotFound) {
      return result;
    }
  }

  return AssetResult<AssetInfo>::Err(MakeError(
      AssetErrorCode::kNotFound, normalized.value(), "asset not found"));
}

AssetResult<std::vector<AssetInfo>> Vfs::List(std::string_view path) {
  auto normalized = NormalizePath(path, true);
  if (!normalized) {
    return AssetResult<std::vector<AssetInfo>>::Err(
        std::move(normalized).error());
  }

  for (const MountPoint &mount : MountSnapshot()) {
    if (!MountMatches(mount.path, normalized.value())) {
      continue;
    }
    auto result = mount.source->List(
        RelativeToMount(mount.path, normalized.value()));
    if (result || result.error().code != AssetErrorCode::kNotFound) {
      return result;
    }
  }

  return AssetResult<std::vector<AssetInfo>>::Err(MakeError(
      AssetErrorCode::kNotFound, normalized.value(), "asset not found"));
}

AssetResult<std::unique_ptr<AssetStream>> Vfs::OpenFile(
    std::string_view path, const ReadOptions &options) {
  auto normalized = NormalizePath(path, true);
  if (!normalized) {
    return AssetResult<std::unique_ptr<AssetStream>>::Err(
        std::move(normalized).error());
  }

  for (const MountPoint &mount : MountSnapshot()) {
    if (!MountMatches(mount.path, normalized.value())) {
      continue;
    }
    auto result = mount.source->OpenFile(
        RelativeToMount(mount.path, normalized.value()), options);
    if (result) {
      return result;
    }
    if (result.error().code != AssetErrorCode::kNotFound) {
      return AssetResult<std::unique_ptr<AssetStream>>::Err(
          std::move(result).error());
    }
  }

  return AssetResult<std::unique_ptr<AssetStream>>::Err(MakeError(
      AssetErrorCode::kNotFound, normalized.value(), "asset not found"));
}

AssetResult<MappedAsset> Vfs::MapFile(
    std::string_view path, const ReadOptions &options) {
  auto normalized = NormalizePath(path, true);
  if (!normalized) {
    return AssetResult<MappedAsset>::Err(std::move(normalized).error());
  }

  const std::string normalized_path = normalized.value();
  const std::vector<MountPoint> mounts = MountSnapshot();

  struct CurrentAsset {
    MountPoint mount;
    std::string source_path;
    AssetInfo info;
  };

  auto resolve_current = [&]() -> AssetResult<CurrentAsset> {
    for (const MountPoint &mount : mounts) {
      if (!MountMatches(mount.path, normalized_path)) {
        continue;
      }

      std::string source_path = RelativeToMount(mount.path, normalized_path);
      auto stat = mount.source->Stat(source_path);
      if (!stat) {
        if (stat.error().code == AssetErrorCode::kNotFound) {
          continue;
        }
        return AssetResult<CurrentAsset>::Err(std::move(stat).error());
      }
      return AssetResult<CurrentAsset>::Ok(CurrentAsset{
          mount, std::move(source_path), std::move(stat).value()});
    }

    return AssetResult<CurrentAsset>::Err(MakeError(
        AssetErrorCode::kNotFound, normalized_path, "asset not found"));
  };

  if (options.cache_policy == CachePolicy::kDefault) {
    std::optional<CacheEntry> cached;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = cache_.find(normalized_path);
      if (it != cache_.end()) {
        cached = it->second;
      }
    }

    if (cached.has_value()) {
      auto current = resolve_current();
      bool valid = false;
      if (current) {
        valid = cached->source.get() == current.value().mount.source.get() &&
                cached->source_path == current.value().source_path &&
                cached->info.kind == current.value().info.kind &&
                cached->info.size == current.value().info.size &&
                cached->info.modified_time_ns ==
                    current.value().info.modified_time_ns;
      }

      if (valid) {
        if (options.max_size != 0 && cached->info.size > options.max_size) {
          return AssetResult<MappedAsset>::Err(MakeError(
              AssetErrorCode::kBudgetExceeded, cached->info.path,
              "file exceeds requested maximum size"));
        }

        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_.find(normalized_path);
        if (it != cache_.end()) {
          cache_lru_.splice(cache_lru_.begin(), cache_lru_, it->second.lru);
          cache_stats_.hit_count += 1;
        }
        return AssetResult<MappedAsset>::Ok(std::move(cached->asset));
      }

      std::lock_guard<std::mutex> lock(mutex_);
      auto it = cache_.find(normalized_path);
      if (it != cache_.end() && it->second.source.get() == cached->source.get() &&
          it->second.source_path == cached->source_path &&
          it->second.info.size == cached->info.size &&
          it->second.info.modified_time_ns == cached->info.modified_time_ns) {
        cache_stats_.resident_bytes -= it->second.info.size;
        cache_lru_.erase(it->second.lru);
        cache_.erase(it);
        cache_stats_.entry_count = static_cast<uint64_t>(cache_.size());
      }
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      cache_stats_.miss_count += 1;
    }
  }

  for (const MountPoint &mount : mounts) {
    if (!MountMatches(mount.path, normalized.value())) {
      continue;
    }
    const std::string source_path = RelativeToMount(mount.path, normalized_path);
    auto result = mount.source->MapFile(source_path, options);
    if (result) {
      if (options.cache_policy == CachePolicy::kDefault &&
          (cache_stats().memory_budget == 0 ||
              result.value().info().size <= cache_stats().memory_budget)) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto existing = cache_.find(normalized_path);
        if (existing != cache_.end()) {
          cache_stats_.resident_bytes -= existing->second.info.size;
          cache_lru_.erase(existing->second.lru);
          cache_.erase(existing);
        }

        cache_lru_.push_front(normalized_path);
        CacheEntry entry;
        entry.asset = result.value();
        entry.source = mount.source;
        entry.source_path = source_path;
        entry.info = result.value().info();
        entry.lru = cache_lru_.begin();
        cache_stats_.resident_bytes += entry.info.size;
        cache_.emplace(normalized_path, std::move(entry));
        cache_stats_.entry_count = static_cast<uint64_t>(cache_.size());
        TrimCacheLocked();
      }
      return result;
    }
    if (result.error().code != AssetErrorCode::kNotFound) {
      return AssetResult<MappedAsset>::Err(std::move(result).error());
    }
  }

  return AssetResult<MappedAsset>::Err(MakeError(
      AssetErrorCode::kNotFound, normalized_path, "asset not found"));
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

AssetCacheStats Vfs::cache_stats() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return cache_stats_;
}

void Vfs::TrimCacheLocked() {
  if (cache_stats_.memory_budget != 0) {
    while (cache_stats_.resident_bytes > cache_stats_.memory_budget &&
           !cache_lru_.empty()) {
      const std::string key = cache_lru_.back();
      auto it = cache_.find(key);
      if (it != cache_.end()) {
        cache_stats_.resident_bytes -= it->second.info.size;
        cache_.erase(it);
      }
      cache_lru_.pop_back();
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

AssetResult<std::shared_ptr<ReadOnlyAssetSource>> OpenNativeAssetSource(
    std::filesystem::path root, NativeSourceOptions options) {
  std::error_code ec;
  if (!std::filesystem::exists(root, ec)) {
    return AssetResult<std::shared_ptr<ReadOnlyAssetSource>>::Err(MakeError(
        AssetErrorCode::kNotFound, root.generic_string(), "root not found"));
  }
  if (!std::filesystem::is_directory(root, ec)) {
    return AssetResult<std::shared_ptr<ReadOnlyAssetSource>>::Err(MakeError(
        AssetErrorCode::kNotADirectory, root.generic_string(),
        "root is not a directory"));
  }
  return AssetResult<std::shared_ptr<ReadOnlyAssetSource>>::Ok(
      std::make_shared<NativeAssetSource>(std::move(root), options));
}

AssetResult<std::shared_ptr<ReadOnlyAssetSource>> OpenPackageAssetSource(
    std::filesystem::path package_file, PackageOpenOptions options) {
  (void)options;
  return AssetResult<std::shared_ptr<ReadOnlyAssetSource>>::Err(MakeError(
      AssetErrorCode::kUnsupported, package_file.generic_string(),
      "package asset source parsing is not implemented yet"));
}

} // namespace luna::asset
