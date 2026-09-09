#include "file_vfs.h"
#include "package_file_source.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <ios>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using luna::file::CachePolicy;
using luna::file::FileInfo;
using luna::file::FileKind;
using luna::file::FileSource;
using luna::file::FileStatus;
using luna::file::FileStatusCode;
using luna::file::FileStream;
using luna::file::MappedFile;
using luna::file::ReadOptions;
using luna::file::Vfs;

void Require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void Require(const FileStatus &status, const std::string &message) {
  if (!status) {
    throw std::runtime_error(message + ": " + status.message());
  }
}

std::vector<std::byte> Bytes(std::string_view text) {
  std::vector<std::byte> bytes(text.size());
  std::memcpy(bytes.data(), text.data(), text.size());
  return bytes;
}

std::string Text(const MappedFile &file) {
  const auto bytes = file.bytes();
  return std::string(
      reinterpret_cast<const char *>(bytes.data()), bytes.size());
}

class MemoryStream final : public FileStream {
public:
  MemoryStream(std::vector<std::byte> bytes, FileInfo info)
      : bytes_(std::move(bytes)), info_(std::move(info)) {}

  const FileInfo &info() const noexcept override { return info_; }
  uint64_t position() const noexcept override { return position_; }
  bool eof() const noexcept override { return position_ >= bytes_.size(); }

  FileStatus Read(std::span<std::byte> output, size_t *bytes_read) override {
    if (!bytes_read) {
      return FileStatus::Error(
          FileStatusCode::kIoError, "read", info_.path, "output is null");
    }
    const size_t available = bytes_.size() - static_cast<size_t>(position_);
    const size_t read_count = std::min(output.size(), available);
    if (read_count > 0) {
      std::memcpy(output.data(), bytes_.data() + position_, read_count);
      position_ += read_count;
    }
    *bytes_read = read_count;
    return FileStatus::Ok();
  }

  FileStatus Seek(int64_t offset, std::ios_base::seekdir whence) override {
    int64_t base = 0;
    if (whence == std::ios_base::cur) {
      base = static_cast<int64_t>(position_);
    } else if (whence == std::ios_base::end) {
      base = static_cast<int64_t>(bytes_.size());
    }

    const int64_t next = base + offset;
    if (next < 0 || next > static_cast<int64_t>(bytes_.size())) {
      return FileStatus::Error(
          FileStatusCode::kIoError, "seek", info_.path, "bad seek");
    }
    position_ = static_cast<uint64_t>(next);
    return FileStatus::Ok();
  }

private:
  std::vector<std::byte> bytes_;
  FileInfo info_;
  uint64_t position_ = 0;
};

class FakeSource final : public FileSource {
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
  void ResetMapFileCount() noexcept { map_file_count_ = 0; }

  FileStatus Stat(std::string_view path, FileInfo *out) override {
    const std::string key(path);
    auto it = files_.find(key);
    if (it == files_.end()) {
      return FileStatus::Error(
          FileStatusCode::kNotFound, "stat", key, "not found");
    }

    out->path = key;
    out->kind = FileKind::kFile;
    out->size = static_cast<uint64_t>(it->second.bytes.size());
    out->modified_time_ns = it->second.modified_time_ns;
    return FileStatus::Ok();
  }

  FileStatus List(std::string_view path, std::vector<FileInfo> *out) override {
    const std::string prefix(path);
    out->clear();
    for (const auto &[name, file] : files_) {
      if (!prefix.empty() &&
          (name.size() <= prefix.size() ||
              name.compare(0, prefix.size(), prefix) != 0 ||
              name[prefix.size()] != '/')) {
        continue;
      }
      if (prefix.empty() ||
          name.find('/', prefix.size() + 1) == std::string::npos) {
        FileInfo info;
        info.path = name;
        info.kind = FileKind::kFile;
        info.size = static_cast<uint64_t>(file.bytes.size());
        info.modified_time_ns = file.modified_time_ns;
        out->push_back(std::move(info));
      }
    }
    if (out->empty()) {
      return FileStatus::Error(
          FileStatusCode::kNotFound, "list", prefix, "not found");
    }
    std::sort(out->begin(), out->end(),
        [](const FileInfo &a, const FileInfo &b) { return a.path < b.path; });
    return FileStatus::Ok();
  }

  FileStatus OpenFile(std::string_view path, const ReadOptions &options,
      std::unique_ptr<FileStream> *out) override {
    (void)options;
    FileInfo info;
    FileStatus status = Stat(path, &info);
    if (!status) {
      return status;
    }
    std::vector<std::byte> bytes = files_.at(std::string(path)).bytes;
    *out = std::make_unique<MemoryStream>(std::move(bytes), std::move(info));
    return FileStatus::Ok();
  }

  FileStatus MapFile(std::string_view path, const ReadOptions &options,
      MappedFile *out) override {
    (void)options;
    map_file_count_ += 1;
    FileInfo info;
    FileStatus status = Stat(path, &info);
    if (!status) {
      return status;
    }
    std::vector<std::byte> bytes = files_.at(std::string(path)).bytes;
    *out = MappedFile::FromOwnedBuffer(std::move(bytes), std::move(info));
    return FileStatus::Ok();
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

void CheckStatusOutParameterApi() {
  auto source = std::make_shared<FakeSource>("source", 0);
  source->AddFile("a.txt", "alpha");

  Vfs vfs;
  Require(vfs.Mount("/", source), "mount source");

  FileInfo info;
  Require(vfs.Stat("a.txt", &info), "stat file");
  Require(info.path == "a.txt", "stat path");
  std::vector<FileInfo> list;
  Require(vfs.List("/", &list), "list root");
  Require(!list.empty() && list[0].path == "a.txt", "list output");

  MappedFile mapped;
  Require(vfs.MapFile("a.txt", &mapped), "map file");
  Require(Text(mapped) == "alpha", "mapped file contents");

  std::unique_ptr<FileStream> stream;
  Require(vfs.OpenFile("a.txt", &stream), "open file");
  std::array<std::byte, 8> buffer;
  size_t bytes_read = 0;
  Require(stream->Read(buffer, &bytes_read), "read stream");
  Require(bytes_read == 5, "read byte count output");
}

void CheckPackageBeforeNativeFallback() {
  auto package = std::make_shared<FakeSource>(
      "package", luna::file::kPackageMountPriority);
  package->AddFile("images/a.txt", "package-a");

  auto native =
      std::make_shared<FakeSource>("native", luna::file::kNativeMountPriority);
  native->AddFile("images/a.txt", "native-a");
  native->AddFile("images/b.txt", "native-b");

  Vfs vfs;
  Require(vfs.Mount("/", native), "mount native");
  Require(vfs.Mount("/", package), "mount package");

  MappedFile from_package;
  Require(vfs.MapFile("images/a.txt", &from_package), "open package file");
  Require(Text(from_package) == "package-a", "package priority");

  MappedFile from_native;
  Require(vfs.MapFile("images/b.txt", &from_native), "open native fallback");
  Require(Text(from_native) == "native-b", "native fallback");
}

void CheckLongestPrefixAndUnmount() {
  auto root = std::make_shared<FakeSource>("root", 0);
  root->AddFile("mods/data.txt", "root");
  auto mods = std::make_shared<FakeSource>("mods", 0);
  mods->AddFile("data.txt", "mods");

  Vfs vfs;
  Require(vfs.Mount("/", root), "mount root");
  Require(vfs.Mount("mods", mods), "mount mods");

  MappedFile result;
  Require(vfs.MapFile("mods/data.txt", &result), "longest prefix file");
  Require(Text(result) == "mods", "longest prefix wins");
  Require(vfs.Unmount("mods", "mods"), "unmount mods");
  Require(vfs.MapFile("mods/data.txt", &result), "root after unmount");
  Require(Text(result) == "root", "unmount clears cache");
}

void CheckInvalidPathsAndCache() {
  Vfs vfs;
  MappedFile invalid_file;
  FileStatus invalid = vfs.MapFile("../escape.txt", &invalid_file);
  Require(!invalid, "invalid traversal rejected");
  Require(invalid.code() == FileStatusCode::kInvalidPath,
      "invalid traversal status");
  Require(
      !vfs.MapFile("/absolute.txt", &invalid_file), "absolute path rejected");
  Require(
      !vfs.MapFile("C:/absolute.txt", &invalid_file), "drive path rejected");
  Require(!vfs.MapFile("/", &invalid_file), "root-as-file rejected");
  Require(!vfs.MapFile(std::string_view("a.txt\0suffix", 12), &invalid_file),
      "embedded null rejected");

  auto source = std::make_shared<FakeSource>("cache", 0);
  source->AddFile("a.txt", "alpha");
  source->AddFile("dir/file.txt", "slash");
  source->AddFile("empty.txt", "");
  Require(vfs.Mount("/", source), "mount cache source");
  MappedFile backslash;
  Require(
      vfs.MapFile("dir\\file.txt", &backslash), "backslash path normalization");
  Require(Text(backslash) == "slash", "backslash normalized content");
  source->ResetMapFileCount();

  MappedFile first;
  Require(vfs.MapFile("a.txt", &first), "first map");
  MappedFile second;
  Require(vfs.MapFile("a.txt", &second), "second map");
  Require(source->map_file_count() == 1, "second map uses cache");
  Require(vfs.cache_stats().hit_count == 1, "cache hit counted");

  ReadOptions bypass;
  bypass.cache_policy = CachePolicy::kBypassCache;
  MappedFile bypassed;
  Require(vfs.MapFile("a.txt", bypass, &bypassed), "bypass map");
  Require(source->map_file_count() == 2, "bypass reads source");

  ReadOptions too_small;
  too_small.max_size = 4;
  MappedFile oversized;
  FileStatus status = vfs.MapFile("a.txt", too_small, &oversized);
  Require(!status && status.code() == FileStatusCode::kBudgetExceeded,
      "max size enforced on cached file");
  MappedFile empty;
  Require(vfs.MapFile("empty.txt", &empty), "empty file maps");
  Require(empty.size() == 0 &&
          empty.kind() == luna::file::MappingKind::kOwnedBuffer,
      "empty mapping kind");
}

void CheckMissingPackage() {
  std::shared_ptr<FileSource> source;
  auto status = luna::file::OpenPackageFileSource("missing-game.luna", {}, &source);
  Require(!status && status.code() == FileStatusCode::kNotFound,
      "missing package fails at mount");
}

void CheckCacheOptionsAndLifetime() {
  auto source = std::make_shared<FakeSource>("cache", 0);
  source->AddFile("a.txt", "alpha");
  source->AddFile("b.txt", "bravo");
  Vfs vfs;
  Require(vfs.Mount("/", source), "mount cache source");
  vfs.SetMemoryBudget(5);

  MappedFile first;
  Require(vfs.MapFile("a.txt", &first), "initial map");
  ReadOptions owned;
  owned.prefer_native_mapping = false;
  MappedFile next;
  Require(vfs.MapFile("a.txt", owned, &next), "change mapping preference");
  Require(source->map_file_count() == 2, "mapping preference reaches source");
  owned.allow_owned_fallback = false;
  Require(vfs.MapFile("a.txt", owned, &next), "change fallback option");
  Require(source->map_file_count() == 3, "fallback option reaches source");

  source->UpdateFile("a.txt", "new-a");
  Require(vfs.MapFile("a.txt", owned, &next), "refresh changed file");
  Require(Text(next) == "new-a", "cache refreshes changed content");
  Require(Text(first) == "alpha", "existing mapping survives refresh");
  Require(vfs.MapFile("b.txt", &next), "evict by budget");
  Require(vfs.cache_stats().resident_bytes == 5 &&
          vfs.cache_stats().entry_count == 1,
      "eviction maintains accounting");
  vfs.SetMemoryBudget(1);
  Require(vfs.cache_stats().resident_bytes == 0 &&
          vfs.cache_stats().entry_count == 0,
      "budget reduction evicts entries");
  Require(Text(next) == "bravo", "mapping survives eviction");
  Require(vfs.MapFile("a.txt", &next), "oversized entry remains readable");
  Require(vfs.cache_stats().entry_count == 0, "oversized entry is not cached");
}

void CheckProductionVfsDoesNotUseStdFilesystem() {
  const char *files[] = {
      "src/file_vfs.cpp",
      "src/native_file_source.h",
      "src/native_file_source_linux.cpp",
      "src/native_file_source_windows.cpp",
      "src/package_file_source.cpp",
      "src/vfs.h",
      "src/vfs.cpp",
  };
  for (const char *relative : files) {
    std::ifstream in(std::string(LUNA_SOURCE_DIR) + "/" + relative);
    Require(static_cast<bool>(in), std::string("open scan file ") + relative);
    const std::string text(
        (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Require(text.find("<filesystem>") == std::string::npos,
        std::string("no <filesystem> in ") + relative);
    Require(text.find("std::filesystem") == std::string::npos,
        std::string("no std::filesystem in ") + relative);
    Require(text.find("std::ifstream") == std::string::npos,
        std::string("no std::ifstream in ") + relative);
    Require(text.find("std::ofstream") == std::string::npos,
        std::string("no std::ofstream in ") + relative);
  }
}

} // namespace

int main() {
  CheckStatusOutParameterApi();
  CheckPackageBeforeNativeFallback();
  CheckLongestPrefixAndUnmount();
  CheckInvalidPathsAndCache();
  CheckCacheOptionsAndLifetime();
  CheckMissingPackage();
  CheckProductionVfsDoesNotUseStdFilesystem();
  return 0;
}
