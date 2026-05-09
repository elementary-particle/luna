#ifndef LUNA_ASSET_VFS_H
#define LUNA_ASSET_VFS_H

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <ios>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace luna::asset {

inline constexpr int kDefaultMountPriority = 0;
inline constexpr int kPackageMountPriority = 100;
inline constexpr int kNativeMountPriority = -100;

enum class AssetErrorCode {
  kNotFound,
  kInvalidPath,
  kNotAFile,
  kIsDirectory,
  kNotADirectory,
  kPermissionDenied,
  kIoError,
  kFormatError,
  kUnsupported,
  kBudgetExceeded,
};

struct AssetError {
  AssetErrorCode code = AssetErrorCode::kIoError;
  std::string path;
  std::string message;
};

template <typename T>
class AssetResult {
public:
  static AssetResult Ok(T value) { return AssetResult(std::move(value)); }
  static AssetResult Err(AssetError error) { return AssetResult(std::move(error)); }

  bool ok() const { return std::holds_alternative<T>(data_); }
  explicit operator bool() const { return ok(); }

  T &value() & { return std::get<T>(data_); }
  const T &value() const & { return std::get<T>(data_); }
  T &&value() && { return std::move(std::get<T>(data_)); }

  const AssetError &error() const & { return std::get<AssetError>(data_); }
  AssetError &&error() && { return std::move(std::get<AssetError>(data_)); }

private:
  explicit AssetResult(T value) : data_(std::move(value)) {}
  explicit AssetResult(AssetError error) : data_(std::move(error)) {}

  std::variant<T, AssetError> data_;
};

template <>
class AssetResult<void> {
public:
  static AssetResult Ok() { return AssetResult(); }
  static AssetResult Err(AssetError error) { return AssetResult(std::move(error)); }

  bool ok() const { return std::holds_alternative<std::monostate>(data_); }
  explicit operator bool() const { return ok(); }

  const AssetError &error() const & { return std::get<AssetError>(data_); }
  AssetError &&error() && { return std::move(std::get<AssetError>(data_)); }

private:
  AssetResult() = default;
  explicit AssetResult(AssetError error) : data_(std::move(error)) {}

  std::variant<std::monostate, AssetError> data_;
};

enum class EntryKind {
  kFile,
  kDirectory,
};

struct AssetInfo {
  std::string path;
  EntryKind kind = EntryKind::kFile;
  uint64_t size = 0;
  uint64_t modified_time_ns = 0;
  uint64_t source_id = 0;
};

enum class MappingKind {
  kOsFileMapping,
  kOwnedBuffer,
  kBorrowedView,
};

enum class CachePolicy {
  kDefault,
  kBypassCache,
};

struct ReadOptions {
  CachePolicy cache_policy = CachePolicy::kDefault;
  bool prefer_os_mapping = true;
  uint64_t max_size = 0;
};

struct NativeSourceOptions {
  bool prefer_os_mapping = true;
  bool follow_symlinks = false;
};

struct PackageOpenOptions {
  bool prefer_os_mapping = true;
};

struct MountOptions {
  std::optional<int> priority;
};

struct AssetCacheStats {
  uint64_t memory_budget = 0;
  uint64_t resident_bytes = 0;
  uint64_t pinned_bytes = 0;
  uint64_t entry_count = 0;
  uint64_t hit_count = 0;
  uint64_t miss_count = 0;
};

class MappedAsset {
public:
  MappedAsset() = default;

  static MappedAsset FromOwnedBuffer(
      std::vector<std::byte> bytes, AssetInfo info);
  static MappedAsset FromBorrowedView(std::span<const std::byte> bytes,
      std::shared_ptr<const void> owner, AssetInfo info);
  static MappedAsset FromOsFileMapping(std::span<const std::byte> bytes,
      std::shared_ptr<const void> owner, AssetInfo info);

  std::span<const std::byte> bytes() const noexcept;
  const std::byte *data() const noexcept { return data_; }
  uint64_t size() const noexcept { return size_; }
  MappingKind kind() const noexcept { return kind_; }
  const AssetInfo &info() const noexcept { return info_; }
  bool empty() const noexcept { return size_ == 0; }

private:
  MappedAsset(const std::byte *data, uint64_t size, MappingKind kind,
      std::shared_ptr<const void> owner, AssetInfo info);

  const std::byte *data_ = nullptr;
  uint64_t size_ = 0;
  MappingKind kind_ = MappingKind::kOwnedBuffer;
  std::shared_ptr<const void> owner_;
  AssetInfo info_;
};

class AssetStream {
public:
  virtual ~AssetStream() = default;

  virtual const AssetInfo &info() const noexcept = 0;
  virtual uint64_t position() const noexcept = 0;
  virtual bool eof() const noexcept = 0;

  virtual AssetResult<size_t> Read(std::span<std::byte> output) = 0;
  virtual AssetResult<void> Seek(int64_t offset, std::ios_base::seekdir whence) = 0;
};

class ReadOnlyAssetSource {
public:
  virtual ~ReadOnlyAssetSource() = default;

  virtual std::string_view DebugName() const noexcept = 0;
  virtual int DefaultMountPriority() const noexcept {
    return kDefaultMountPriority;
  }

  virtual AssetResult<AssetInfo> Stat(std::string_view path) = 0;
  virtual AssetResult<std::vector<AssetInfo>> List(std::string_view path) = 0;
  virtual AssetResult<std::unique_ptr<AssetStream>> OpenFile(
      std::string_view path, const ReadOptions &options = {}) = 0;
  virtual AssetResult<MappedAsset> MapFile(
      std::string_view path, const ReadOptions &options = {}) = 0;
};

class Vfs final : public ReadOnlyAssetSource {
public:
  std::string_view DebugName() const noexcept override { return "asset-vfs"; }

  AssetResult<void> Mount(std::string vfs_mount_path,
      std::shared_ptr<ReadOnlyAssetSource> source,
      MountOptions options = {});
  bool Unmount(std::string_view vfs_mount_path, std::string_view debug_name);

  AssetResult<AssetInfo> Stat(std::string_view path) override;
  AssetResult<std::vector<AssetInfo>> List(std::string_view path) override;
  AssetResult<std::unique_ptr<AssetStream>> OpenFile(
      std::string_view path, const ReadOptions &options = {}) override;
  AssetResult<MappedAsset> MapFile(
      std::string_view path, const ReadOptions &options = {}) override;

  void SetMemoryBudget(uint64_t bytes);
  uint64_t memory_budget() const noexcept;
  AssetCacheStats cache_stats() const noexcept;
  void TrimCache();
  void ClearCache();

private:
  struct MountPoint {
    std::string path;
    std::shared_ptr<ReadOnlyAssetSource> source;
    int priority = kDefaultMountPriority;
    uint64_t sequence = 0;
  };

  struct CacheEntry {
    MappedAsset asset;
    std::shared_ptr<ReadOnlyAssetSource> source;
    std::string source_path;
    AssetInfo info;
    std::list<std::string>::iterator lru;
  };

  static bool MountLess(const MountPoint &a, const MountPoint &b);
  std::vector<MountPoint> MountSnapshot() const;
  void ClearCacheLocked();
  void TrimCacheLocked();

  std::vector<MountPoint> mounts_;
  uint64_t next_sequence_ = 0;
  AssetCacheStats cache_stats_;
  std::list<std::string> cache_lru_;
  std::unordered_map<std::string, CacheEntry> cache_;
  mutable std::mutex mutex_;
};

AssetResult<std::shared_ptr<ReadOnlyAssetSource>> OpenNativeAssetSource(
    std::filesystem::path root, NativeSourceOptions options = {});

AssetResult<std::shared_ptr<ReadOnlyAssetSource>> OpenPackageAssetSource(
    std::filesystem::path package_file, PackageOpenOptions options = {});

} // namespace luna::asset

#endif
