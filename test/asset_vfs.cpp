#include "asset_vfs.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <ios>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using luna::asset::AssetError;
using luna::asset::AssetErrorCode;
using luna::asset::AssetInfo;
using luna::asset::AssetResult;
using luna::asset::AssetStream;
using luna::asset::CachePolicy;
using luna::asset::EntryKind;
using luna::asset::MappedAsset;
using luna::asset::ReadOnlyAssetSource;
using luna::asset::ReadOptions;
using luna::asset::Vfs;

void Require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

std::vector<std::byte> Bytes(std::string_view text) {
  std::vector<std::byte> bytes(text.size());
  std::memcpy(bytes.data(), text.data(), text.size());
  return bytes;
}

std::string Text(const MappedAsset &asset) {
  const auto bytes = asset.bytes();
  return std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size());
}

class MemoryStream final : public AssetStream {
public:
  MemoryStream(std::vector<std::byte> bytes, AssetInfo info)
      : bytes_(std::move(bytes)), info_(std::move(info)) {}

  const AssetInfo &info() const noexcept override { return info_; }
  uint64_t position() const noexcept override { return position_; }
  bool eof() const noexcept override { return position_ >= bytes_.size(); }

  AssetResult<size_t> Read(std::span<std::byte> output) override {
    const size_t available = bytes_.size() - static_cast<size_t>(position_);
    const size_t read_count = std::min(output.size(), available);
    if (read_count > 0) {
      std::memcpy(output.data(), bytes_.data() + position_, read_count);
      position_ += read_count;
    }
    return AssetResult<size_t>::Ok(read_count);
  }

  AssetResult<void> Seek(
      int64_t offset, std::ios_base::seekdir whence) override {
    int64_t base = 0;
    if (whence == std::ios_base::cur) {
      base = static_cast<int64_t>(position_);
    } else if (whence == std::ios_base::end) {
      base = static_cast<int64_t>(bytes_.size());
    }

    const int64_t next = base + offset;
    if (next < 0 || next > static_cast<int64_t>(bytes_.size())) {
      return AssetResult<void>::Err(
          AssetError{AssetErrorCode::kIoError, info_.path, "bad seek"});
    }
    position_ = static_cast<uint64_t>(next);
    return AssetResult<void>::Ok();
  }

private:
  std::vector<std::byte> bytes_;
  AssetInfo info_;
  uint64_t position_ = 0;
};

class FakeSource final : public ReadOnlyAssetSource {
public:
  FakeSource(std::string name, int default_priority)
      : name_(std::move(name)), default_priority_(default_priority) {}

  std::string_view DebugName() const noexcept override { return name_; }
  int DefaultMountPriority() const noexcept override {
    return default_priority_;
  }

  void AddFile(std::string path, std::string contents) {
    files_[std::move(path)] = File{Bytes(contents), next_modified_time_ns_++};
  }

  void UpdateFile(std::string path, std::string contents) {
    AddFile(std::move(path), contents);
  }

  int map_file_count() const noexcept { return map_file_count_; }

  void ResetMapFileCount() noexcept {
    map_file_count_ = 0;
  }

  AssetResult<AssetInfo> Stat(std::string_view path) override {
    const std::string key(path);
    auto it = files_.find(key);
    if (it == files_.end()) {
      return AssetResult<AssetInfo>::Err(
          AssetError{AssetErrorCode::kNotFound, key, "not found"});
    }

    AssetInfo info;
    info.path = key;
    info.kind = EntryKind::kFile;
    info.size = static_cast<uint64_t>(it->second.bytes.size());
    info.modified_time_ns = it->second.modified_time_ns;
    return AssetResult<AssetInfo>::Ok(std::move(info));
  }

  AssetResult<std::vector<AssetInfo>> List(std::string_view path) override {
    return AssetResult<std::vector<AssetInfo>>::Err(
        AssetError{AssetErrorCode::kUnsupported, std::string(path),
            "list not implemented in fake"});
  }

  AssetResult<std::unique_ptr<AssetStream>> OpenFile(
      std::string_view path, const ReadOptions &options = {}) override {
    (void)options;
    auto info = Stat(path);
    if (!info) {
      return AssetResult<std::unique_ptr<AssetStream>>::Err(
          std::move(info).error());
    }
    std::vector<std::byte> bytes = files_.at(std::string(path)).bytes;
    std::unique_ptr<AssetStream> stream =
        std::make_unique<MemoryStream>(std::move(bytes), std::move(info).value());
    return AssetResult<std::unique_ptr<AssetStream>>::Ok(std::move(stream));
  }

  AssetResult<MappedAsset> MapFile(
      std::string_view path, const ReadOptions &options = {}) override {
    (void)options;
    map_file_count_ += 1;
    auto info = Stat(path);
    if (!info) {
      return AssetResult<MappedAsset>::Err(std::move(info).error());
    }
    std::vector<std::byte> bytes = files_.at(std::string(path)).bytes;
    return AssetResult<MappedAsset>::Ok(
        MappedAsset::FromOwnedBuffer(std::move(bytes), std::move(info).value()));
  }

private:
  struct File {
    std::vector<std::byte> bytes;
    uint64_t modified_time_ns = 0;
  };

  std::string name_;
  int default_priority_ = 0;
  uint64_t next_modified_time_ns_ = 1;
  int map_file_count_ = 0;
  std::unordered_map<std::string, File> files_;
};

void CheckPackageBeforeNativeFallback() {
  auto package = std::make_shared<FakeSource>(
      "package", luna::asset::kPackageMountPriority);
  package->AddFile("images/a.txt", "package-a");

  auto native =
      std::make_shared<FakeSource>("native", luna::asset::kNativeMountPriority);
  native->AddFile("images/a.txt", "native-a");
  native->AddFile("images/b.txt", "native-b");

  Vfs vfs;
  Require(vfs.Mount("/", native).ok(), "mount native");
  Require(vfs.Mount("/", package).ok(), "mount package");

  auto from_package = vfs.MapFile("images/a.txt");
  Require(from_package.ok(), "open package asset");
  Require(Text(from_package.value()) == "package-a",
      "package should override native at same path");

  auto from_native = vfs.MapFile("images/b.txt");
  Require(from_native.ok(), "open native fallback asset");
  Require(Text(from_native.value()) == "native-b",
      "native should satisfy package miss");

  auto stream_result = vfs.OpenFile("images/b.txt");
  Require(stream_result.ok(), "open native fallback stream");
  std::array<std::byte, 16> buffer;
  auto read_result = stream_result.value()->Read(buffer);
  Require(read_result.ok(), "read stream");
  const std::string stream_text(
      reinterpret_cast<const char *>(buffer.data()), read_result.value());
  Require(stream_text == "native-b", "stream should read fallback asset");
}

void CheckLongestPrefixWins() {
  auto root = std::make_shared<FakeSource>("root", 0);
  root->AddFile("mods/data.txt", "root");

  auto mods = std::make_shared<FakeSource>("mods", 0);
  mods->AddFile("data.txt", "mods");

  Vfs vfs;
  Require(vfs.Mount("/", root).ok(), "mount root");
  Require(vfs.Mount("mods", mods).ok(), "mount mods");

  auto result = vfs.MapFile("mods/data.txt");
  Require(result.ok(), "open longest-prefix asset");
  Require(Text(result.value()) == "mods", "longest prefix should win");
}

void CheckInvalidPathsAndCacheBudget() {
  Vfs vfs;
  auto invalid = vfs.MapFile("../escape.txt");
  Require(!invalid, "invalid traversal rejected");
  Require(invalid.error().code == AssetErrorCode::kInvalidPath,
      "invalid traversal reports InvalidPath");

  vfs.SetMemoryBudget(4096);
  Require(vfs.memory_budget() == 4096, "memory budget stored");
  vfs.ClearCache();
  Require(vfs.cache_stats().resident_bytes == 0, "cache clear keeps no data");
}

void CheckMappedAssetCacheHitsAndBypass() {
  auto source = std::make_shared<FakeSource>("cache", 0);
  source->AddFile("a.txt", "alpha");

  Vfs vfs;
  Require(vfs.Mount("/", source).ok(), "mount cache source");

  auto first = vfs.MapFile("a.txt");
  Require(first.ok(), "first cache map");
  Require(Text(first.value()) == "alpha", "first cache contents");
  Require(source->map_file_count() == 1, "first map should read source");

  auto second = vfs.MapFile("a.txt");
  Require(second.ok(), "second cache map");
  Require(Text(second.value()) == "alpha", "second cache contents");
  Require(source->map_file_count() == 1, "second map should hit cache");
  Require(vfs.cache_stats().miss_count == 1, "cache miss should be counted");
  Require(vfs.cache_stats().hit_count == 1, "cache hit should be counted");
  Require(vfs.cache_stats().resident_bytes == 5, "cache should track bytes");
  Require(vfs.cache_stats().entry_count == 1, "cache should track entries");

  ReadOptions bypass;
  bypass.cache_policy = CachePolicy::kBypassCache;
  auto bypassed = vfs.MapFile("a.txt", bypass);
  Require(bypassed.ok(), "bypass cache map");
  Require(source->map_file_count() == 2, "bypass should read source");
  Require(vfs.cache_stats().hit_count == 1, "bypass should not count hit");
  Require(vfs.cache_stats().miss_count == 1, "bypass should not count miss");

  vfs.ClearCache();
  Require(vfs.cache_stats().resident_bytes == 0, "clear resets resident bytes");
  Require(vfs.cache_stats().entry_count == 0, "clear resets entry count");

  auto after_clear = vfs.MapFile("a.txt");
  Require(after_clear.ok(), "map after clear");
  Require(source->map_file_count() == 3, "clear should force source read");
}

void CheckCacheBudgetEvictsLruAndSkipsOversizedAssets() {
  auto source = std::make_shared<FakeSource>("budget", 0);
  source->AddFile("a.txt", "aaaa");
  source->AddFile("b.txt", "bbbb");
  source->AddFile("big.txt", "1234567");

  Vfs vfs;
  vfs.SetMemoryBudget(6);
  Require(vfs.Mount("/", source).ok(), "mount budget source");

  Require(vfs.MapFile("a.txt").ok(), "map budget a");
  Require(vfs.MapFile("b.txt").ok(), "map budget b");
  Require(vfs.cache_stats().resident_bytes == 4, "budget should evict LRU");
  Require(vfs.cache_stats().entry_count == 1, "budget should keep one entry");

  source->ResetMapFileCount();
  Require(vfs.MapFile("a.txt").ok(), "remap evicted a");
  Require(source->map_file_count() == 1, "evicted entry should read source");

  source->ResetMapFileCount();
  Require(vfs.MapFile("big.txt").ok(), "map oversized asset");
  Require(source->map_file_count() == 1, "oversized first map reads source");
  Require(vfs.cache_stats().resident_bytes <= 6, "oversized asset not cached");
  Require(vfs.MapFile("big.txt").ok(), "remap oversized asset");
  Require(source->map_file_count() == 2, "oversized remap should read source");
}

void CheckCacheMaxSizeAndStaleValidation() {
  auto source = std::make_shared<FakeSource>("stale", 0);
  source->AddFile("a.txt", "alpha");

  Vfs vfs;
  Require(vfs.Mount("/", source).ok(), "mount stale source");

  Require(vfs.MapFile("a.txt").ok(), "prime stale cache");
  source->ResetMapFileCount();

  ReadOptions too_small;
  too_small.max_size = 4;
  auto oversized = vfs.MapFile("a.txt", too_small);
  Require(!oversized, "cached max_size should be enforced");
  Require(oversized.error().code == AssetErrorCode::kBudgetExceeded,
      "cached max_size should report budget exceeded");
  Require(source->map_file_count() == 0, "cached max_size should not read");

  source->UpdateFile("a.txt", "bravo");
  auto updated = vfs.MapFile("a.txt");
  Require(updated.ok(), "map stale-updated asset");
  Require(Text(updated.value()) == "bravo", "stale cache should refresh");
  Require(source->map_file_count() == 1, "stale cache should read source");
}

void CheckMountChangesClearMappedAssetCache() {
  auto first = std::make_shared<FakeSource>("first", 0);
  first->AddFile("a.txt", "first");
  auto second = std::make_shared<FakeSource>("second", 10);
  second->AddFile("a.txt", "second");

  Vfs vfs;
  Require(vfs.Mount("/", first).ok(), "mount first source");
  auto initial = vfs.MapFile("a.txt");
  Require(initial.ok(), "map first source");
  Require(Text(initial.value()) == "first", "first source contents");

  Require(vfs.Mount("/", second).ok(), "mount second source");
  auto overridden = vfs.MapFile("a.txt");
  Require(overridden.ok(), "map overridden source");
  Require(Text(overridden.value()) == "second", "mount should clear cache");
  Require(second->map_file_count() == 1, "second source should be read");

  Require(vfs.Unmount("/", "second"), "unmount second source");
  auto restored = vfs.MapFile("a.txt");
  Require(restored.ok(), "map restored source");
  Require(Text(restored.value()) == "first", "unmount should clear cache");
  Require(first->map_file_count() == 2, "first source should be read again");
}

void CheckMapsRealFontFile() {
  auto native = luna::asset::OpenNativeAssetSource(
      std::filesystem::path(LUNA_SOURCE_DIR));
  Require(native.ok(), "open source dir native source");

  Vfs vfs;
  Require(vfs.Mount("/", std::move(native).value()).ok(),
      "mount source dir native source");

  auto mapped = vfs.MapFile("test/assets/ABeeZee-Regular.ttf");
  Require(mapped.ok(), "map real test font");
  Require(mapped.value().size() > 4, "mapped font should not be empty");

  const auto bytes = mapped.value().bytes();
  Require(bytes[0] == std::byte{0x00} && bytes[1] == std::byte{0x01} &&
          bytes[2] == std::byte{0x00} && bytes[3] == std::byte{0x00},
      "mapped font should preserve TrueType header");
}

} // namespace

int main() {
  CheckPackageBeforeNativeFallback();
  CheckLongestPrefixWins();
  CheckInvalidPathsAndCacheBudget();
  CheckMappedAssetCacheHitsAndBypass();
  CheckCacheBudgetEvictsLruAndSkipsOversizedAssets();
  CheckCacheMaxSizeAndStaleValidation();
  CheckMountChangesClearMappedAssetCache();
  CheckMapsRealFontFile();
  return 0;
}
using luna::asset::Vfs;
