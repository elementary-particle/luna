#include "package_file_source.h"
#include "native_file_source.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <lz4.h>
#include <map>
#include <stdexcept>

namespace luna::file {
namespace {
using Bytes = std::vector<std::byte>;
constexpr uint64_t kHeader = 32, kFooter = 48, kTrailer = 32;
constexpr uint64_t kIndexLimit = 64 * 1024 * 1024;
constexpr size_t kEntryLimit = 1000000;
struct Failure {
  FileStatus status;
};
[[noreturn]] void Fail(
    std::string message, FileStatusCode code = FileStatusCode::kFormatError) {
  throw Failure{FileStatus::Error(code, "package", "", std::move(message))};
}
void Check(FileStatus status) {
  if (!status)
    throw Failure{std::move(status)};
}
uint32_t Crc(std::span<const std::byte> bytes) {
  // Portable slicing-by-8 CRC-32/ISO-HDLC; no CPU/endian-specific intrinsics.
  static const auto table = [] {
    std::array<std::array<uint32_t, 256>, 8> t{};
    for (uint32_t n = 0; n < 256; ++n) {
      uint32_t c = n;
      for (int k = 0; k < 8; ++k)
        c = (c >> 1) ^ ((c & 1) ? 0xedb88320U : 0);
      t[0][n] = c;
    }
    for (int k = 1; k < 8; ++k)
      for (uint32_t n = 0; n < 256; ++n) {
        uint32_t c = t[k - 1][n];
        t[k][n] = t[0][c & 255] ^ (c >> 8);
      }
    return t;
  }();
  uint32_t c = ~0U;
  auto byte = [](std::byte b) { return std::to_integer<uint32_t>(b); };
  while (bytes.size() >= 8) {
    c ^= byte(bytes[0]) | (byte(bytes[1]) << 8) | (byte(bytes[2]) << 16) |
        (byte(bytes[3]) << 24);
    c = table[7][c & 255] ^ table[6][(c >> 8) & 255] ^
        table[5][(c >> 16) & 255] ^ table[4][c >> 24] ^
        table[3][byte(bytes[4])] ^ table[2][byte(bytes[5])] ^
        table[1][byte(bytes[6])] ^ table[0][byte(bytes[7])];
    bytes = bytes.subspan(8);
  }
  for (auto b : bytes)
    c = table[0][(c ^ byte(b)) & 255] ^ (c >> 8);
  return ~c;
}
void Put(Bytes &b, uint64_t value, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    b.push_back(std::byte(value & 255));
    value >>= 8;
  }
}
void Magic(Bytes &b, std::string_view s) {
  for (char c : s)
    b.push_back(std::byte(static_cast<unsigned char>(c)));
}
struct Cursor {
  std::span<const std::byte> b;
  size_t pos = 0;
  std::span<const std::byte> Take(size_t n) {
    if (n > b.size() - pos)
      Fail("truncated metadata");
    auto s = b.subspan(pos, n);
    pos += n;
    return s;
  }
  uint64_t Get(size_t n) {
    uint64_t value = 0;
    auto s = Take(n);
    for (size_t i = 0; i < n; ++i)
      value |= uint64_t(std::to_integer<uint8_t>(s[i])) << (i * 8);
    return value;
  }
  uint32_t U32() { return static_cast<uint32_t>(Get(4)); }
  void Expect(std::string_view s) {
    auto bytes = Take(s.size());
    if (std::memcmp(bytes.data(), s.data(), s.size()))
      Fail("bad magic");
  }
};
// UTF-8 scalar values only; no platform-specific normalization or case folding.
bool ValidPath(std::string_view s) {
  std::string normalized;
  if (s.empty() || s.size() > 4096 || !NormalizePath(s, false, &normalized) ||
      normalized != s)
    return false;
  for (size_t i = 0; i < s.size();) {
    auto c = static_cast<unsigned char>(s[i++]);
    if (c < 0x80) {
      if (c < 32 || c == 127 || c == ':' || c == '\\')
        return false;
      continue;
    }
    unsigned n = c >= 0xc2 && c <= 0xdf ? 1
        : c >= 0xe0 && c <= 0xef        ? 2
        : c >= 0xf0 && c <= 0xf4        ? 3
                                        : 0;
    if (!n || n > s.size() - i)
      return false;
    uint32_t cp = c & ((1U << (6 - n)) - 1);
    for (unsigned k = 0; k < n; ++k) {
      auto d = static_cast<unsigned char>(s[i++]);
      if ((d & 0xc0) != 0x80)
        return false;
      cp = (cp << 6) | (d & 63);
    }
    if (cp < (n == 1 ? 0x80U
                     : n == 2 ? 0x800U
                              : 0x10000U) ||
        cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
      return false;
  }
  return true;
}
void ReadExact(FileStream &stream, std::span<std::byte> bytes) {
  while (!bytes.empty()) {
    size_t n = 0;
    Check(stream.Read(bytes, &n));
    if (!n || n > bytes.size())
      Fail("truncated package or input");
    bytes = bytes.subspan(n);
  }
}
struct Chunk {
  uint64_t offset;
  uint32_t stored, raw, codec, raw_crc, stored_crc;
  uint32_t slot = 0;
};
struct Entry {
  FileInfo info;
  std::vector<Chunk> chunks;
  uint64_t begin = 0, table_offset = 0, end = 0;
  size_t index_table = 0;
  uint32_t slot = 0;
  bool stored = true;
};
struct State {
  std::unique_ptr<FileStream> stream;
  std::shared_ptr<MappedFile> mapping;
  std::mutex mutex;
  std::map<std::string, Entry, std::less<>> entries;
  Bytes index;
  uint32_t slots = 0;
  std::unique_ptr<std::atomic<bool>[]> verified;
  uint64_t Size() const {
    return mapping ? mapping->size() : stream->info().size;
  }
  Bytes ReadAt(uint64_t offset, size_t n) {
    Bytes b(n);
    if (mapping) {
      if (n)
        std::memcpy(b.data(), mapping->data() + offset, n);
    } else {
      std::lock_guard lock(mutex);
      Check(stream->Seek(static_cast<int64_t>(offset), std::ios::beg));
      ReadExact(*stream, b);
    }
    return b;
  }
  std::span<const std::byte> View(uint64_t offset, size_t n, Bytes &scratch) {
    if (mapping)
      return mapping->bytes().subspan(static_cast<size_t>(offset), n);
    scratch = ReadAt(offset, n);
    return scratch;
  }
  void CheckFile(const Entry &e, bool force) {
    if (!force && verified[e.slot].load(std::memory_order_relaxed))
      return;
    size_t table_size = e.chunks.size() * 32;
    Bytes scratch;
    auto bytes = View(e.table_offset, table_size + kTrailer, scratch);
    if (table_size &&
        std::memcmp(bytes.data(), index.data() + e.index_table, table_size))
      Fail("file chunk table disagrees with index");
    auto trailer = bytes.subspan(table_size);
    Cursor t{trailer};
    t.Expect("LUNAFIL2");
    if (t.Get(8) != e.info.size || t.Get(8) != e.end - e.begin ||
        t.Get(4) != e.chunks.size() || t.Get(4) != Crc(trailer.first(28)))
      Fail("file trailer lengths or checksum mismatch");
    verified[e.slot].store(true, std::memory_order_relaxed);
  }
  std::span<const std::byte> StoredView(const Chunk &c, bool force) {
    auto bytes = mapping->bytes().subspan(static_cast<size_t>(c.offset), c.raw);
    if (force || !verified[c.slot].load(std::memory_order_relaxed)) {
      if (Crc(bytes) != c.raw_crc)
        Fail("stored chunk checksum mismatch");
      verified[c.slot].store(true, std::memory_order_relaxed);
    }
    return bytes;
  }
  Bytes Decode(const Chunk &c) {
    Bytes scratch;
    auto b = View(c.offset, c.stored, scratch);
    if (Crc(b) != c.stored_crc)
      Fail("stored chunk checksum mismatch");
    if (c.codec == 0)
      return Bytes(b.begin(), b.end());
    Bytes raw(c.raw);
    if (LZ4_decompress_safe(reinterpret_cast<const char *>(b.data()),
            reinterpret_cast<char *>(raw.data()), static_cast<int>(c.stored),
            static_cast<int>(c.raw)) != static_cast<int>(c.raw))
      Fail("invalid LZ4 block or decoded length");
    if (Crc(raw) != c.raw_crc)
      Fail("decoded chunk checksum mismatch");
    return raw;
  }
};
template <class F> FileStatus Guard(F f) {
  try {
    f();
    return {};
  } catch (const Failure &e) {
    return e.status;
  } catch (const std::bad_alloc &) {
    return FileStatus::Error(
        FileStatusCode::kBudgetExceeded, "package", "", "allocation failed");
  } catch (const std::exception &e) {
    return FileStatus::Error(FileStatusCode::kIoError, "package", "", e.what());
  }
}
class PackageStream final : public FileStream {
public:
  PackageStream(std::shared_ptr<State> state, const Entry *entry, bool force)
      : state_(std::move(state)), entry_(entry), force_(force) {}
  const FileInfo &info() const noexcept override { return entry_->info; }
  uint64_t position() const noexcept override { return pos_; }
  bool eof() const noexcept override { return pos_ >= info().size; }
  FileStatus Seek(int64_t offset, std::ios_base::seekdir whence) override {
    return Guard([&] {
      uint64_t base = whence == std::ios::beg ? 0
          : whence == std::ios::cur           ? pos_
                                              : info().size;
      if (whence != std::ios::beg && whence != std::ios::cur &&
          whence != std::ios::end)
        Fail("invalid seek", FileStatusCode::kIoError);
      uint64_t magnitude =
          offset < 0 ? uint64_t(-(offset + 1)) + 1 : uint64_t(offset);
      if ((offset < 0 && magnitude > base) ||
          (offset >= 0 && magnitude > uint64_t(INT64_MAX) - base))
        Fail("seek out of range", FileStatusCode::kIoError);
      pos_ = offset < 0 ? base - magnitude : base + magnitude;
    });
  }
  FileStatus Read(std::span<std::byte> output, size_t *read) override {
    if (read)
      *read = 0;
    return Guard([&] {
      if (!read)
        Fail("null output", FileStatusCode::kIoError);
      while (!output.empty() && pos_ < info().size) {
        size_t index = pos_ / kPackageChunkSize;
        const auto &chunk = entry_->chunks[index];
        std::span<const std::byte> bytes;
        if (state_->mapping && chunk.codec == 0) {
          bytes = state_->StoredView(chunk, force_);
        } else {
          if (cached_ != index) {
            cache_ = state_->Decode(chunk);
            cached_ = index;
          }
          bytes = cache_;
        }
        size_t within = pos_ % kPackageChunkSize;
        size_t n = std::min(output.size(), bytes.size() - within);
        std::memcpy(output.data(), bytes.data() + within, n);
        output = output.subspan(n);
        pos_ += n;
        *read += n;
      }
    });
  }

private:
  std::shared_ptr<State> state_;
  const Entry *entry_;
  bool force_;
  uint64_t pos_ = 0;
  size_t cached_ = SIZE_MAX;
  Bytes cache_;
};
class PackageFileSource final : public FileSource {
public:
  explicit PackageFileSource(std::shared_ptr<State> s) : state_(std::move(s)) {}
  std::string_view DebugName() const noexcept override { return "package"; }
  int DefaultMountPriority() const noexcept override {
    return kPackageMountPriority;
  }
  const Entry &Find(std::string_view path) {
    std::string normalized;
    Check(NormalizePath(path, true, &normalized));
    auto it = state_->entries.find(normalized);
    if (it == state_->entries.end())
      Fail("entry not found: " + normalized, FileStatusCode::kNotFound);
    return it->second;
  }
  FileStatus Stat(std::string_view path, FileInfo *out) override {
    return Guard([&] {
      if (!out)
        Fail("null output", FileStatusCode::kIoError);
      *out = Find(path).info;
    });
  }
  FileStatus List(std::string_view path, std::vector<FileInfo> *out) override {
    return Guard([&] {
      if (!out)
        Fail("null output", FileStatusCode::kIoError);
      const auto &e = Find(path);
      if (e.info.kind != FileKind::kDirectory)
        Fail("not a directory", FileStatusCode::kNotADirectory);
      std::string prefix = e.info.path.empty() ? "" : e.info.path + "/";
      std::vector<FileInfo> result;
      for (auto it = state_->entries.lower_bound(prefix);
          it != state_->entries.end(); ++it) {
        auto &name = it->first;
        if (!name.starts_with(prefix))
          break;
        if (name.size() > prefix.size() &&
            name.find('/', prefix.size()) == std::string::npos)
          result.push_back(it->second.info);
      }
      *out = std::move(result);
    });
  }
  FileStatus OpenFile(std::string_view path, const ReadOptions &options,
      std::unique_ptr<FileStream> *out) override {
    return Guard([&] {
      if (!out)
        Fail("null output", FileStatusCode::kIoError);
      const auto &e = Find(path);
      if (e.info.kind == FileKind::kDirectory)
        Fail("is a directory", FileStatusCode::kIsDirectory);
      if (options.max_size && e.info.size > options.max_size)
        Fail("file exceeds read budget", FileStatusCode::kBudgetExceeded);
      bool force = options.cache_policy == CachePolicy::kBypassCache;
      state_->CheckFile(e, force);
      *out = std::make_unique<PackageStream>(state_, &e, force);
    });
  }
  FileStatus MapFile(std::string_view path, const ReadOptions &options,
      MappedFile *out) override {
    return Guard([&] {
      if (!out)
        Fail("null output", FileStatusCode::kIoError);
      std::unique_ptr<FileStream> stream;
      Check(OpenFile(path, options, &stream));
      const auto &e = Find(path);
      if (options.prefer_native_mapping && state_->mapping && e.stored &&
          e.info.size) {
        for (const auto &c : e.chunks)
          state_->StoredView(
              c, options.cache_policy == CachePolicy::kBypassCache);
        *out = MappedFile::FromNativeMapping(
            state_->mapping->bytes().subspan(
                static_cast<size_t>(e.begin), static_cast<size_t>(e.info.size)),
            std::static_pointer_cast<const void>(state_->mapping), e.info);
        return;
      }
      if (!options.allow_owned_fallback)
        Fail("asset requires owned mapping", FileStatusCode::kUnsupported);
      if (stream->info().size > SIZE_MAX)
        Fail("file exceeds address space", FileStatusCode::kBudgetExceeded);
      Bytes b(static_cast<size_t>(stream->info().size));
      ReadExact(*stream, b);
      *out = MappedFile::FromOwnedBuffer(std::move(b), stream->info());
    });
  }

private:
  std::shared_ptr<State> state_;
};
void AddDirectories(State &s) {
  if (s.entries.size() > kEntryLimit)
    Fail("too many entries", FileStatusCode::kBudgetExceeded);
  uint64_t directory_bytes = 0;
  auto add = [&](const std::string &dir) {
    auto it = s.entries.find(dir);
    if (it != s.entries.end()) {
      if (it->second.info.kind != FileKind::kDirectory)
        Fail("file/directory path conflict");
      return;
    }
    if (s.entries.size() == kEntryLimit ||
        dir.size() > kIndexLimit - directory_bytes)
      Fail(
          "directory metadata exceeds budget", FileStatusCode::kBudgetExceeded);
    directory_bytes += dir.size();
    s.entries.emplace(dir, Entry{FileInfo{dir, FileKind::kDirectory}, {}});
  };
  add("");
  // Map insertion preserves iterators; parents sort before their children.
  for (const auto &[path, e] : s.entries) {
    if (e.info.kind == FileKind::kDirectory)
      continue;
    for (size_t p = path.find('/'); p != std::string::npos;
        p = path.find('/', p + 1))
      add(path.substr(0, p));
  }
}
FileStatus ReadPackageState(std::shared_ptr<State> s,
    const PackageOpenOptions &options, std::shared_ptr<FileSource> *out) {
  return Guard([&] {
    if (!out)
      Fail("null output", FileStatusCode::kIoError);
    uint64_t size = s->Size();
    if (size < kHeader + kFooter || size > INT64_MAX)
      Fail("invalid package size");
    auto header = s->ReadAt(0, kHeader);
    Cursor h{header};
    h.Expect("LUNAPACK");
    if (h.Get(4) != 2 || h.Get(4) != kPackageChunkSize)
      Fail("unsupported package version or chunk size");
    if (h.Get(8) || h.Get(4))
      Fail("nonzero reserved header fields");
    uint32_t header_crc = Crc(std::span(header).first(28));
    if (h.Get(4) != header_crc)
      Fail("header checksum mismatch");
    auto footer = s->ReadAt(size - kFooter, kFooter);
    Cursor f{footer};
    f.Expect("LUNAEND2");
    uint64_t index_offset = f.Get(8), index_size = f.Get(8),
             total_size = f.Get(8);
    uint32_t count = f.U32(), index_crc = f.U32();
    if (f.U32() != header_crc || f.U32() != Crc(std::span(footer).first(44)))
      Fail("footer checksum mismatch");
    if (total_size != size || index_offset < kHeader ||
        index_offset > size - kFooter ||
        index_size != size - kFooter - index_offset)
      Fail("invalid package/index lengths or trailing data");
    if (index_size > kIndexLimit || index_size > options.max_index_size)
      Fail("index exceeds budget", FileStatusCode::kBudgetExceeded);
    s->index = s->ReadAt(index_offset, static_cast<size_t>(index_size));
    const auto &index = s->index;
    if (Crc(index) != index_crc)
      Fail("index checksum mismatch");
    Cursor r{index};
    uint64_t next = kHeader;
    std::string previous;
    if (count >= kEntryLimit || count > index_size / 17)
      Fail("invalid file count");
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t len = r.U32(), chunks = r.U32();
      uint64_t raw = r.Get(8);
      if (!len || len > 4096 || raw > INT64_MAX ||
          chunks != raw / kPackageChunkSize + (raw % kPackageChunkSize != 0))
        Fail("invalid entry dimensions");
      auto name = r.Take(len);
      std::string path(
          reinterpret_cast<const char *>(name.data()), name.size());
      if (!ValidPath(path) || (!previous.empty() && path <= previous))
        Fail("invalid, duplicate or unsorted path");
      previous = path;
      if (chunks > (r.b.size() - r.pos) / 32)
        Fail("truncated chunk index");
      Entry e{FileInfo{path, FileKind::kFile, raw}, {}};
      e.begin = next;
      e.index_table = r.pos;
      e.slot = s->slots++;
      for (uint32_t j = 0; j < chunks; ++j) {
        Chunk c{};
        c.offset = r.Get(8);
        c.stored = r.U32();
        c.raw = r.U32();
        c.codec = r.U32();
        c.raw_crc = r.U32();
        c.stored_crc = r.U32();
        c.slot = s->slots++;
        if (r.U32())
          Fail("nonzero reserved chunk field");
        uint64_t expected = std::min<uint64_t>(
            kPackageChunkSize, raw - uint64_t(j) * kPackageChunkSize);
        if (c.offset != next || c.raw != expected || !c.stored ||
            c.stored > c.raw || c.codec > 1 ||
            (c.codec == 0 &&
                (c.stored != c.raw || c.raw_crc != c.stored_crc)) ||
            (c.codec == 1 && c.stored >= c.raw))
          Fail("invalid chunk dimensions or codec");
        if (next > index_offset || c.stored > index_offset - next)
          Fail("chunk outside data region");
        next += c.stored;
        e.stored = e.stored && c.codec == 0;
        e.chunks.push_back(c);
      }
      e.table_offset = next;
      uint64_t tail = uint64_t(chunks) * 32 + kTrailer;
      if (next > index_offset || tail > index_offset - next)
        Fail("file trailer outside data region");
      next += tail;
      e.end = next;
      s->entries.emplace(path, std::move(e));
    }
    if (r.pos != index.size() || next != index_offset)
      Fail("unreferenced package bytes");
    AddDirectories(*s);
    s->verified = std::make_unique<std::atomic<bool>[]>(s->slots);
    *out = std::make_shared<PackageFileSource>(std::move(s));
  });
}
} // namespace

FileStatus OpenPackageFileSource(std::unique_ptr<FileStream> stream,
    const PackageOpenOptions &options, std::shared_ptr<FileSource> *out) {
  return Guard([&] {
    if (!stream || !out)
      Fail("null argument", FileStatusCode::kIoError);
    auto s = std::make_shared<State>();
    s->stream = std::move(stream);
    Check(ReadPackageState(std::move(s), options, out));
  });
}
FileStatus OpenPackageFileSource(FileSource &source, std::string_view path,
    const PackageOpenOptions &options, std::shared_ptr<FileSource> *out) {
  return Guard([&] {
    if (!out)
      Fail("null output", FileStatusCode::kIoError);
    if (options.prefer_native_mapping) {
      ReadOptions read;
      read.allow_owned_fallback = false;
      MappedFile mapping;
      auto status = source.MapFile(path, read, &mapping);
      if (status && mapping.kind() == MappingKind::kNativeMapping) {
        auto s = std::make_shared<State>();
        s->mapping = std::make_shared<MappedFile>(std::move(mapping));
        Check(ReadPackageState(std::move(s), options, out));
        return;
      }
      if (!status && status.code() != FileStatusCode::kUnsupported &&
          status.code() != FileStatusCode::kIoError &&
          status.code() != FileStatusCode::kBudgetExceeded)
        Check(status);
    }
    std::unique_ptr<FileStream> stream;
    Check(source.OpenFile(path, {}, &stream));
    Check(OpenPackageFileSource(std::move(stream), options, out));
  });
}

FileStatus OpenPackageFileSource(std::string package_file,
    const PackageOpenOptions &options, std::shared_ptr<FileSource> *out) {
  return Guard([&] {
    if (!out)
      Fail("null output", FileStatusCode::kIoError);
#ifdef _WIN32
    std::replace(package_file.begin(), package_file.end(), '\\', '/');
#endif
    auto slash = package_file.find_last_of('/');
    auto parent = slash == std::string::npos
        ? std::string(".")
        : package_file.substr(0, slash + 1);
    auto name = slash == std::string::npos ? package_file
                                           : package_file.substr(slash + 1);
    std::shared_ptr<FileSource> source;
    Check(OpenNativeFileSource(parent, {}, &source));
    Check(OpenPackageFileSource(*source, name, options, out));
  });
}

FileStatus WritePackage(
    std::ostream &output, FileSource &source, std::vector<std::string> paths) {
  return Guard([&] {
    std::sort(paths.begin(), paths.end());
    State names;
    for (auto &p : paths) {
      if (!ValidPath(p) || !names.entries.emplace(p, Entry{}).second)
        Fail("invalid or duplicate input path: " + p,
            FileStatusCode::kInvalidPath);
    }
    AddDirectories(names);
    if (paths.size() > UINT32_MAX)
      Fail("too many files", FileStatusCode::kBudgetExceeded);
    uint64_t offset = 0;
    auto write = [&](const Bytes &b) {
      if (b.size() > uint64_t(INT64_MAX) - offset)
        Fail("package too large", FileStatusCode::kBudgetExceeded);
      if (!b.empty())
        output.write(reinterpret_cast<const char *>(b.data()),
            static_cast<std::streamsize>(b.size()));
      if (!output)
        Fail("output write failed", FileStatusCode::kIoError);
      offset += b.size();
    };
    Bytes header;
    Magic(header, "LUNAPACK");
    Put(header, 2, 4);
    Put(header, kPackageChunkSize, 4);
    Put(header, 0, 8);
    Put(header, 0, 4);
    auto header_crc = Crc(header);
    Put(header, header_crc, 4);
    write(header);
    Bytes index;
    for (const auto &path : paths) {
      std::unique_ptr<FileStream> stream;
      Check(source.OpenFile(path, {}, &stream));
      uint64_t size = stream->info().size,
               count =
                   size / kPackageChunkSize + (size % kPackageChunkSize != 0);
      uint64_t metadata = 16 + path.size() + count * 32;
      if (size > INT64_MAX || count > UINT32_MAX ||
          metadata > kIndexLimit - index.size())
        Fail("index exceeds format limit", FileStatusCode::kBudgetExceeded);
      // Probe compression without retaining a whole asset. Require at least
      // 1% savings; tiny gains should not sacrifice a directly mappable file.
      uint64_t packed_size = 0;
      Bytes raw(kPackageChunkSize),
          packed(LZ4_compressBound(kPackageChunkSize));
      for (uint64_t j = 0; j < count; ++j) {
        size_t n = static_cast<size_t>(std::min<uint64_t>(
            kPackageChunkSize, size - j * kPackageChunkSize));
        ReadExact(*stream, std::span(raw).first(n));
        int result =
            LZ4_compress_default(reinterpret_cast<const char *>(raw.data()),
                reinterpret_cast<char *>(packed.data()), static_cast<int>(n),
                static_cast<int>(packed.size()));
        packed_size += result > 0 ? std::min<uint64_t>(result, n) : n;
      }
      bool compress =
          size && size - packed_size >= size / 100 + (size % 100 != 0);
      Check(stream->Seek(0, std::ios::beg));
      Put(index, path.size(), 4);
      Put(index, count, 4);
      Put(index, size, 8);
      Magic(index, path);
      Bytes table;
      uint64_t begin = offset;
      for (uint64_t j = 0; j < count; ++j) {
        raw.resize(static_cast<size_t>(std::min<uint64_t>(
            kPackageChunkSize, size - j * kPackageChunkSize)));
        ReadExact(*stream, raw);
        int n = compress
            ? LZ4_compress_default(reinterpret_cast<const char *>(raw.data()),
                  reinterpret_cast<char *>(packed.data()),
                  static_cast<int>(raw.size()), static_cast<int>(packed.size()))
            : 0;
        bool encoded = n > 0 && static_cast<size_t>(n) < raw.size();
        Bytes payload =
            encoded ? Bytes(packed.begin(), packed.begin() + n) : raw;
        auto raw_crc = Crc(raw), stored_crc = encoded ? Crc(payload) : raw_crc;
        Put(table, offset, 8);
        Put(table, payload.size(), 4);
        Put(table, raw.size(), 4);
        Put(table, encoded ? 1 : 0, 4);
        Put(table, raw_crc, 4);
        Put(table, stored_crc, 4);
        Put(table, 0, 4);
        write(payload);
      }
      write(table);
      index.insert(index.end(), table.begin(), table.end());
      Bytes trailer;
      Magic(trailer, "LUNAFIL2");
      Put(trailer, size, 8);
      Put(trailer, offset + kTrailer - begin, 8);
      Put(trailer, count, 4);
      Put(trailer, Crc(trailer), 4);
      write(trailer);
      std::array<std::byte, 1> extra;
      size_t n = 0;
      Check(stream->Read(extra, &n));
      if (n)
        Fail("input grew while packaging");
    }
    uint64_t index_offset = offset;
    write(index);
    Bytes footer;
    Magic(footer, "LUNAEND2");
    Put(footer, index_offset, 8);
    Put(footer, index.size(), 8);
    Put(footer, offset + kFooter, 8);
    Put(footer, paths.size(), 4);
    Put(footer, Crc(index), 4);
    Put(footer, header_crc, 4);
    Put(footer, Crc(footer), 4);
    write(footer);
  });
}
FileStatus VerifyPackage(FileSource &source) {
  return Guard([&] {
    std::vector<std::string> dirs{""};
    Bytes buffer(kPackageChunkSize);
    while (!dirs.empty()) {
      auto dir = std::move(dirs.back());
      dirs.pop_back();
      std::vector<FileInfo> files;
      Check(source.List(dir, &files));
      for (const auto &f : files) {
        if (f.kind == FileKind::kDirectory) {
          dirs.push_back(f.path);
          continue;
        }
        std::unique_ptr<FileStream> stream;
        ReadOptions options;
        options.cache_policy = CachePolicy::kBypassCache;
        Check(source.OpenFile(f.path, options, &stream));
        uint64_t left = f.size;
        while (left) {
          size_t n = std::min<uint64_t>(left, buffer.size());
          ReadExact(*stream, std::span(buffer).first(n));
          left -= n;
        }
      }
    }
  });
}
} // namespace luna::file
