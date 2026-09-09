#include "native_file_source.h"
#include "package_file_source.h"
#include "temp_directory.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <fstream>
#include <future>
#include <iostream>
#include <random>
#include <sstream>

using namespace luna::file;
namespace {
void Require(FileStatus s) {
  if (!s) {
    std::cerr << s.message() << '\n';
    std::abort();
  }
}
void Write(const std::filesystem::path &p, const std::string &s) {
  std::ofstream f(p, std::ios::binary);
  f.write(s.data(), s.size());
  assert(f);
}
std::string Utf8(const std::filesystem::path &p) {
  auto s = p.u8string();
  return {s.begin(), s.end()};
}
std::string Text(const MappedFile &m) {
  return m.empty()
      ? ""
      : std::string(reinterpret_cast<const char *>(m.data()), m.size());
}
uint32_t ReferenceCrc(std::string_view data) {
  uint32_t crc = 0xffffffff;
  for (unsigned char byte : data) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320U : 0);
  }
  return crc ^ 0xffffffff;
}
uint64_t GetLe(const std::string &s, size_t offset, size_t bytes) {
  uint64_t n = 0;
  for (size_t i = 0; i < bytes; ++i)
    n |= uint64_t(static_cast<unsigned char>(s[offset + i])) << (8 * i);
  return n;
}
void SetLe(std::string &s, size_t offset, uint64_t value, size_t bytes) {
  for (size_t i = 0; i < bytes; ++i)
    s[offset + i] = static_cast<char>(value >> (8 * i));
}
void RepairIndexCrc(std::string &s) {
  auto footer = s.size() - 48;
  auto offset = GetLe(s, footer + 8, 8), size = GetLe(s, footer + 16, 8);
  SetLe(s, footer + 36, ReferenceCrc(std::string_view(s).substr(offset, size)),
      4);
  SetLe(
      s, footer + 44, ReferenceCrc(std::string_view(s).substr(footer, 44)), 4);
}

// A mutable backing is used only to prove explicit verification bypasses the
// immutable-mount cache, without truncating an OS-mapped file in a test.
class TestMapping final : public FileSource {
public:
  explicit TestMapping(const std::string &content)
      : bytes(std::make_shared<std::vector<std::byte>>(content.size())) {
    std::memcpy(bytes->data(), content.data(), content.size());
  }
  std::string_view DebugName() const noexcept override { return "test-map"; }
  FileStatus Stat(std::string_view, FileInfo *) override {
    return Unsupported();
  }
  FileStatus List(std::string_view, std::vector<FileInfo> *) override {
    return Unsupported();
  }
  FileStatus OpenFile(std::string_view, const ReadOptions &,
      std::unique_ptr<FileStream> *) override {
    return Unsupported();
  }
  FileStatus MapFile(
      std::string_view, const ReadOptions &, MappedFile *out) override {
    *out = MappedFile::FromNativeMapping(
        *bytes, bytes, FileInfo{"test", FileKind::kFile, bytes->size()});
    return {};
  }
  std::shared_ptr<std::vector<std::byte>> bytes;

private:
  static FileStatus Unsupported() {
    return FileStatus::Error(
        FileStatusCode::kUnsupported, "test", "", "unsupported");
  }
};

} // namespace
int main() {
  luna::test::TempDirectory temp;
  auto root = temp.path() / "input";
  std::filesystem::create_directories(root / "text");
  std::string large(kPackageChunkSize * 2 + 97, 'a');
  for (size_t i = kPackageChunkSize; i < kPackageChunkSize * 2; ++i)
    large[i] = 'b';
  std::string noise(kPackageChunkSize + 1, '\0');
  std::mt19937 rng(1729);
  for (auto &c : noise)
    c = static_cast<char>(rng());
  Write(root / "large", large);
  Write(root / "noise", noise);
  Write(root / "empty", "");
  Write(root / "text" / "dialogue", "hello");
  std::shared_ptr<FileSource> input;
  Require(OpenNativeFileSource(Utf8(root), {}, &input));
  std::vector<std::string> paths{"noise", "large", "text/dialogue", "empty"};
  std::ostringstream packed(std::ios::binary);
  Require(WritePackage(packed, *input, paths));
  std::reverse(paths.begin(), paths.end());
  std::ostringstream again(std::ios::binary);
  Require(WritePackage(again, *input, paths));
  assert(packed.str() == again.str());
  assert(packed.str().size() < large.size() + noise.size());
  auto archive = temp.path() / "game.luna";
  Write(archive, packed.str());
  std::shared_ptr<FileSource> source;
  Require(OpenPackageFileSource(Utf8(archive), {}, &source));
  Require(VerifyPackage(*source));
  FileInfo info;
  Require(source->Stat("text", &info));
  assert(info.kind == FileKind::kDirectory);
  std::vector<FileInfo> list;
  Require(source->List("/", &list));
  assert(list.size() == 4);
  Require(source->List("text", &list));
  assert(list.size() == 1 && list[0].path == "text/dialogue");
  assert(source->List("large", &list).code() == FileStatusCode::kNotADirectory);
  assert(source->Stat("../x", &info).code() == FileStatusCode::kInvalidPath);
  MappedFile mapped;
  Require(source->MapFile("large", {}, &mapped));
  assert(Text(mapped) == large);
  Require(source->MapFile("noise", {}, &mapped));
  assert(Text(mapped) == noise);
  assert(mapped.kind() == MappingKind::kNativeMapping);
  ReadOptions native_only;
  native_only.allow_owned_fallback = false;
  Require(source->MapFile("noise", native_only, &mapped));
  auto pinned = mapped;
  ReadOptions owned;
  owned.prefer_native_mapping = false;
  Require(source->MapFile("noise", owned, &mapped));
  assert(mapped.kind() == MappingKind::kOwnedBuffer && Text(mapped) == noise);
  auto read_noise = [&] {
    std::unique_ptr<FileStream> s;
    Require(source->OpenFile("noise", {}, &s));
    std::vector<std::byte> b(noise.size());
    size_t n = 0;
    Require(s->Read(b, &n));
    assert(n == noise.size());
    assert(std::memcmp(b.data(), noise.data(), n) == 0);
  };
  auto first = std::async(std::launch::async, read_noise);
  auto second_reader = std::async(std::launch::async, read_noise);
  first.get();
  second_reader.get();
  PackageOpenOptions buffered;
  buffered.prefer_native_mapping = false;
  std::shared_ptr<FileSource> fallback;
  Require(OpenPackageFileSource(Utf8(archive), buffered, &fallback));
  Require(VerifyPackage(*fallback));
  Require(fallback->MapFile("noise", {}, &mapped));
  assert(mapped.kind() == MappingKind::kOwnedBuffer && Text(mapped) == noise);
  fallback.reset();
  std::shared_ptr<FileSource> unmapped_native;
  NativeSourceOptions no_mapping;
  no_mapping.prefer_native_mapping = false;
  Require(
      OpenNativeFileSource(Utf8(temp.path()), no_mapping, &unmapped_native));
  assert(unmapped_native->MapFile("game.luna", native_only, &mapped).code() ==
      FileStatusCode::kUnsupported);
  Require(OpenPackageFileSource(*unmapped_native, "game.luna", {}, &fallback));
  Require(fallback->MapFile("noise", {}, &mapped));
  assert(mapped.kind() == MappingKind::kOwnedBuffer && Text(mapped) == noise);
  fallback.reset();
  Require(source->MapFile("empty", {}, &mapped));
  assert(mapped.empty());
  ReadOptions limited;
  limited.max_size = 1;
  assert(source->MapFile("large", limited, &mapped).code() ==
      FileStatusCode::kBudgetExceeded);
  limited = {};
  limited.allow_owned_fallback = false;
  assert(source->MapFile("large", limited, &mapped).code() ==
      FileStatusCode::kUnsupported);
  PackageOpenOptions tiny;
  tiny.max_index_size = 1;
  std::shared_ptr<FileSource> other;
  assert(OpenPackageFileSource(Utf8(archive), tiny, &other).code() ==
      FileStatusCode::kBudgetExceeded);
  std::unique_ptr<FileStream> stream;
  Require(source->OpenFile("large", {}, &stream));
  std::array<std::byte, 100> buf;
  size_t n;
  Require(stream->Seek(kPackageChunkSize - 3, std::ios::beg));
  Require(stream->Read(buf, &n));
  assert(n == 100 &&
      std::memcmp(buf.data(), large.data() + kPackageChunkSize - 3, n) == 0);
  Require(stream->Seek(-7, std::ios::end));
  Require(stream->Read(buf, &n));
  assert(n == 7 && stream->eof());
  Require(stream->Seek(50, std::ios::end));
  Require(stream->Read(buf, &n));
  assert(n == 0);
  assert(!stream->Seek(INT64_MIN, std::ios::cur));
  Require(stream->Seek(0, std::ios::beg));
  // A stream owns the index and opened archive independently of the source.
  source.reset();
  Require(stream->Read(buf, &n));
  assert(n == 100 && buf[0] == std::byte{'a'});
  stream.reset();
  assert(Text(pinned) == noise); // Mapping outlives both source and streams.
  pinned = {};

  // Flip every bit in a compact archive: header, payload, trailers, index,
  // footer.
  std::ostringstream small(std::ios::binary);
  Require(WritePackage(small, *input, {"text/dialogue", "empty"}));
  auto original = small.str();
  assert(ReferenceCrc("123456789") == 0xcbf43926U);
  assert(GetLe(original, 28, 4) ==
      ReferenceCrc(std::string_view(original).substr(0, 28)));
  assert(GetLe(original, original.size() - 4, 4) ==
      ReferenceCrc(
          std::string_view(original).substr(original.size() - 48, 44)));
  // Repaired checksums must not hide malformed structural metadata.
  auto index_offset = GetLe(original, original.size() - 40, 8);
  auto second = index_offset + 16 + 5; // empty file record
  auto descriptor = second + 16 + std::string("text/dialogue").size();
  auto reject = [&](std::string bytes) {
    RepairIndexCrc(bytes);
    Write(archive, bytes);
    assert(!OpenPackageFileSource(Utf8(archive), {}, &other));
    other.reset();
  };
  auto malformed = original;
  SetLe(malformed, descriptor, 31, 8);
  reject(malformed); // overlaps header
  malformed = original;
  SetLe(malformed, descriptor + 16, 2, 4);
  reject(malformed); // unknown codec
  malformed = original;
  SetLe(malformed, descriptor + 28, 1, 4);
  reject(malformed); // reserved
  malformed = original;
  SetLe(malformed, second + 8, UINT64_MAX, 8);
  reject(malformed); // oversized file
  malformed = original;
  malformed.replace(second + 16, 12, "../x/dialogue");
  reject(malformed);

  for (size_t i = 0; i < original.size(); ++i) {
    for (unsigned bit = 0; bit < 8; ++bit) {
      auto broken = original;
      broken[i] ^= 1U << bit;
      Write(archive, broken);
      auto status = OpenPackageFileSource(Utf8(archive), {}, &other);
      if (status)
        status = VerifyPackage(*other);
      assert(!status);
      other.reset();
    }
  }
  for (size_t n = 0; n < original.size(); ++n) {
    Write(archive, original.substr(0, n));
    assert(!OpenPackageFileSource(Utf8(archive), {}, &other));
  }
  Write(archive, original + "junk");
  assert(!OpenPackageFileSource(Utf8(archive), {}, &other));
  // Payload corruption is discovered on access, including compressed chunks.
  auto broken = packed.str();
  // The empty file has a 32-byte member trailer before the first payload.
  broken[64] ^= 1;
  Write(archive, broken);
  Require(OpenPackageFileSource(Utf8(archive), {}, &other));
  assert(!other->MapFile("large", {}, &mapped));
  Require(other->MapFile("noise", {}, &mapped));
  assert(Text(mapped) == noise);
  other.reset();
  mapped = {};
  TestMapping mutable_backing(packed.str());
  std::shared_ptr<FileSource> cached;
  Require(OpenPackageFileSource(mutable_backing, "test", {}, &cached));
  Require(VerifyPackage(*cached));
  auto packed_bytes = packed.str();
  size_t record = GetLe(packed_bytes, packed_bytes.size() - 40, 8);
  uint64_t noise_offset = 0;
  for (int i = 0; i < 4; ++i) {
    size_t length = GetLe(packed_bytes, record, 4),
           chunks = GetLe(packed_bytes, record + 4, 4);
    if (packed_bytes.substr(record + 16, length) == "noise") {
      noise_offset = GetLe(packed_bytes, record + 16 + length, 8);
      assert(GetLe(packed_bytes, record + 16 + length + 20, 4) ==
          ReferenceCrc(std::string_view(noise).substr(0, kPackageChunkSize)));
    }
    record += 16 + length + chunks * 32;
  }
  assert(noise_offset);
  (*mutable_backing.bytes)[noise_offset] ^= std::byte{1};
  ReadOptions recheck;
  recheck.cache_policy = CachePolicy::kBypassCache;
  assert(!cached->MapFile("noise", recheck, &mapped));
  assert(!VerifyPackage(*cached));
  std::ostringstream bad;
  assert(!WritePackage(bad, *input, {"large", "large"}));
  assert(!WritePackage(bad, *input, {"../large"}));
  assert(!WritePackage(bad, *input, {"text", "text/dialogue"}));
  std::ostringstream empty(std::ios::binary);
  Require(WritePackage(empty, *input, {}));
  Write(archive, empty.str());
  Require(OpenPackageFileSource(Utf8(archive), {}, &other));
  Require(VerifyPackage(*other));
  std::cout
      << "Package roundtrip, corruption, truncation and stream tests passed\n";
}
