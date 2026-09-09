#ifndef LUNA_FILE_VFS_H
#define LUNA_FILE_VFS_H

#include <cstddef>
#include <cstdint>
#include <ios>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace luna::file {

inline constexpr int kDefaultMountPriority = 0;
inline constexpr int kPackageMountPriority = 100;
inline constexpr int kNativeMountPriority = -100;

enum class FileStatusCode {
  kOk,
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

class FileStatus {
public:
  FileStatus() = default;

  static FileStatus Ok() { return FileStatus(); }
  static FileStatus Error(FileStatusCode code, std::string operation,
      std::string path, std::string message, int64_t native_code = 0);

  bool ok() const noexcept { return code_ == FileStatusCode::kOk; }
  explicit operator bool() const noexcept { return ok(); }

  FileStatusCode code() const noexcept { return code_; }
  const std::string &operation() const noexcept { return operation_; }
  const std::string &path() const noexcept { return path_; }
  const std::string &message() const noexcept { return message_; }
  int64_t native_code() const noexcept { return native_code_; }

private:
  FileStatusCode code_ = FileStatusCode::kOk;
  std::string operation_;
  std::string path_;
  std::string message_;
  int64_t native_code_ = 0;
};

enum class FileKind {
  kFile,
  kDirectory,
};

struct FileInfo {
  std::string path;
  FileKind kind = FileKind::kFile;
  uint64_t size = 0;
  uint64_t modified_time_ns = 0;
  uint64_t source_id = 0;
};

enum class MappingKind {
  kNativeMapping,
  kOwnedBuffer,
  kBorrowedView,
};

enum class CachePolicy {
  kDefault,
  kBypassCache,
};

struct ReadOptions {
  CachePolicy cache_policy = CachePolicy::kDefault;
  bool prefer_native_mapping = true;
  bool allow_owned_fallback = true;
  uint64_t max_size = 0;
};

struct NativeSourceOptions {
  bool prefer_native_mapping = true;
  bool follow_symlinks = false;
};

struct PackageOpenOptions {
  bool prefer_native_mapping = true;
  uint64_t max_index_size = 64 * 1024 * 1024;
};

struct MountOptions {
  std::optional<int> priority;
};

struct CacheStats {
  uint64_t memory_budget = 0;
  uint64_t resident_bytes = 0;
  uint64_t pinned_bytes = 0;
  uint64_t entry_count = 0;
  uint64_t hit_count = 0;
  uint64_t miss_count = 0;
};

class MappedFile {
public:
  MappedFile() = default;

  static MappedFile FromOwnedBuffer(
      std::vector<std::byte> bytes, FileInfo info);
  static MappedFile FromBorrowedView(std::span<const std::byte> bytes,
      std::shared_ptr<const void> owner, FileInfo info);
  static MappedFile FromNativeMapping(std::span<const std::byte> bytes,
      std::shared_ptr<const void> owner, FileInfo info);

  std::span<const std::byte> bytes() const noexcept;
  const std::byte *data() const noexcept { return data_; }
  uint64_t size() const noexcept { return size_; }
  MappingKind kind() const noexcept { return kind_; }
  const FileInfo &info() const noexcept { return info_; }
  bool empty() const noexcept { return size_ == 0; }

private:
  MappedFile(const std::byte *data, uint64_t size, MappingKind kind,
      std::shared_ptr<const void> owner, FileInfo info);

  const std::byte *data_ = nullptr;
  uint64_t size_ = 0;
  MappingKind kind_ = MappingKind::kOwnedBuffer;
  std::shared_ptr<const void> owner_;
  FileInfo info_;
};

class FileStream {
public:
  virtual ~FileStream() = default;

  virtual const FileInfo &info() const noexcept = 0;
  virtual uint64_t position() const noexcept = 0;
  virtual bool eof() const noexcept = 0;

  virtual FileStatus Read(std::span<std::byte> output, size_t *bytes_read) = 0;
  virtual FileStatus Seek(int64_t offset, std::ios_base::seekdir whence) = 0;
};

class FileSource {
public:
  virtual ~FileSource() = default;

  virtual std::string_view DebugName() const noexcept = 0;
  virtual int DefaultMountPriority() const noexcept {
    return kDefaultMountPriority;
  }

  virtual FileStatus Stat(std::string_view path, FileInfo *out) = 0;
  virtual FileStatus List(
      std::string_view path, std::vector<FileInfo> *out) = 0;
  virtual FileStatus OpenFile(std::string_view path, const ReadOptions &options,
      std::unique_ptr<FileStream> *out) = 0;
  virtual FileStatus MapFile(
      std::string_view path, const ReadOptions &options, MappedFile *out) = 0;
};

FileStatus NormalizePath(
    std::string_view path, bool allow_root, std::string *out);

class Vfs final : public FileSource {
public:
  std::string_view DebugName() const noexcept override { return "file-vfs"; }

  FileStatus Mount(std::string_view vfs_mount_path,
      std::shared_ptr<FileSource> source, const MountOptions &options = {});
  bool Unmount(std::string_view vfs_mount_path, std::string_view debug_name);

  FileStatus Stat(std::string_view path, FileInfo *out) override;
  FileStatus List(std::string_view path, std::vector<FileInfo> *out) override;
  FileStatus OpenFile(std::string_view path, const ReadOptions &options,
      std::unique_ptr<FileStream> *out) override;
  FileStatus MapFile(std::string_view path, const ReadOptions &options,
      MappedFile *out) override;

  FileStatus OpenFile(std::string_view path, std::unique_ptr<FileStream> *out) {
    return OpenFile(path, ReadOptions{}, out);
  }
  FileStatus MapFile(std::string_view path, MappedFile *out) {
    return MapFile(path, ReadOptions{}, out);
  }

  void SetMemoryBudget(uint64_t bytes);
  uint64_t memory_budget() const noexcept;
  CacheStats cache_stats() const noexcept;
  void TrimCache();
  void ClearCache();

private:
  struct MountPoint {
    std::string path;
    std::shared_ptr<FileSource> source;
    int priority = kDefaultMountPriority;
    uint64_t sequence = 0;
  };

  struct CacheEntry {
    MappedFile file;
    std::shared_ptr<FileSource> source;
    std::string source_path;
    ReadOptions options;
    std::list<std::string>::iterator lru;
  };

  using Cache = std::unordered_map<std::string, CacheEntry>;

  static bool MountLess(const MountPoint &a, const MountPoint &b);
  std::vector<MountPoint> MountSnapshot() const;
  void ClearCacheLocked();
  void TrimCacheLocked();
  void EraseCacheLocked(Cache::iterator entry);

  std::vector<MountPoint> mounts_;
  uint64_t next_sequence_ = 0;
  CacheStats cache_stats_;
  std::list<std::string> cache_lru_;
  Cache cache_;
  mutable std::mutex mutex_;
};

} // namespace luna::file

#endif
