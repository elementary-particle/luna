#include "native_file_source.h"
#include "native_path_boundaries.h"

#if defined(__linux__)

#include <array>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

void Require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void Require(const luna::file::FileStatus &status, const std::string &message) {
  if (!status) {
    throw std::runtime_error(message + ": " + status.message());
  }
}

std::string MakeTempDir() {
  char pattern[] = "/tmp/luna-file-vfs-XXXXXX";
  char *dir = mkdtemp(pattern);
  Require(dir != nullptr, "mkdtemp");
  return std::string(dir);
}

void WriteText(const std::string &path, const char *text) {
  const auto bytes = std::span<const std::byte>(
      reinterpret_cast<const std::byte *>(text), std::strlen(text));
  Require(luna::file::NativeWriteFile(path, bytes), "write temp file");
}

void CheckNativeMapAndStream() {
  const std::string root = MakeTempDir();
  WriteText(root + "/alpha.txt", "alpha");

  std::shared_ptr<luna::file::FileSource> source;
  Require(luna::file::OpenNativeFileSource(root, {}, &source),
      "open native source");

  luna::file::MappedFile mapped;
  luna::file::ReadOptions map_options;
  map_options.allow_owned_fallback = false;
  Require(
      source->MapFile("alpha.txt", map_options, &mapped), "map native file");
  Require(mapped.kind() == luna::file::MappingKind::kNativeMapping,
      "native mapping kind");
  Require(mapped.size() == 5, "mapped size");

  luna::file::Vfs vfs;
  Require(vfs.Mount("/", source), "mount native source");
  Require(vfs.MapFile("alpha.txt", &mapped), "cache native mapping");
  luna::file::ReadOptions owned;
  owned.prefer_native_mapping = false;
  Require(vfs.MapFile("alpha.txt", owned, &mapped), "request owned mapping");
  Require(mapped.kind() == luna::file::MappingKind::kOwnedBuffer,
      "cache respects owned mapping request");
  Require(vfs.MapFile("alpha.txt", map_options, &mapped),
      "request native mapping without fallback");
  Require(mapped.kind() == luna::file::MappingKind::kNativeMapping,
      "cache respects native mapping request");

  std::unique_ptr<luna::file::FileStream> stream;
  Require(source->OpenFile("alpha.txt", {}, &stream), "open native stream");
  std::array<std::byte, 8> buffer;
  size_t bytes_read = 0;
  Require(stream->Read(buffer, &bytes_read), "read native stream");
  Require(bytes_read == 5, "stream byte count");
  Require(stream->Seek(1, std::ios_base::beg), "seek native stream");
  Require(stream->position() == 1, "stream position");
}

void CheckNativeErrorsAndDirectories() {
  const std::string root = MakeTempDir();
  Require(luna::file::NativeCreateDirectories(root + "/a/b/c"),
      "create directories");
  WriteText(root + "/a/b/c/data.txt", "data");

  std::shared_ptr<luna::file::FileSource> source;
  Require(luna::file::OpenNativeFileSource(root, {}, &source),
      "open native source");
  std::vector<luna::file::FileInfo> entries;
  Require(source->List("a/b/c", &entries), "list directory");
  Require(entries.size() == 1 && entries[0].path == "a/b/c/data.txt",
      "listed child path");

  luna::file::FileInfo missing;
  luna::file::FileStatus status = source->Stat("missing.txt", &missing);
  Require(!status && status.code() == luna::file::FileStatusCode::kNotFound,
      "missing maps to not found");
}

} // namespace

int main() {
  luna::test::CheckNativePathBoundaries();
  CheckNativeMapAndStream();
  CheckNativeErrorsAndDirectories();
  return 0;
}

#else
int main() { return 0; }
#endif
