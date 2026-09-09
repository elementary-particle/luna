#ifndef LUNA_TEST_NATIVE_PATH_BOUNDARIES_H
#define LUNA_TEST_NATIVE_PATH_BOUNDARIES_H

#include "native_file_source.h"
#include "temp_directory.h"

#include <fstream>

namespace luna::test {

inline void CheckNativePathBoundaries() {
  TempDirectory temp;
  const auto root = temp.path() / "root";
  const auto outside = temp.path() / "outside";
  std::filesystem::create_directory(root);
  std::filesystem::create_directory(outside);
  std::ofstream(outside / "data.txt") << "outside";
  auto require = [](bool ok, const char *message) {
    if (!ok) {
      throw std::runtime_error(message);
    }
  };
  std::error_code ec;
  std::filesystem::create_directory_symlink(outside, root / "link", ec);
#if defined(_WIN32)
  if (ec == std::errc::permission_denied || ec.value() == 1314) {
    return; // Symlink creation requires Developer Mode or privileges.
  }
#endif
  require(!ec, "create directory symlink");
  std::filesystem::create_symlink(outside / "data.txt", root / "file-link", ec);
  require(!ec, "create file symlink");

  std::shared_ptr<file::FileSource> source;
  require(file::OpenNativeFileSource(root.string(), {}, &source).ok(),
      "open native root");
  file::FileInfo info;
  file::MappedFile mapped;
  std::unique_ptr<file::FileStream> stream;
  for (const char *path : {"link/data.txt", "file-link"}) {
    require(!source->Stat(path, &info), "stat rejects symlink");
    require(!source->MapFile(path, {}, &mapped), "map rejects symlink");
    require(!source->OpenFile(path, {}, &stream), "stream rejects symlink");
  }
  std::vector<file::FileInfo> entries;
  require(!source->List("link", &entries), "list rejects symlink");
  file::NativeSourceOptions options;
  options.follow_symlinks = true;
  require(file::OpenNativeFileSource(root.string(), options, &source).ok(),
      "open root with symlinks enabled");
  for (const char *path : {"link/data.txt", "file-link"}) {
    require(
        source->MapFile(path, {}, &mapped).ok(), "explicitly follow symlink");
    require(mapped.size() == 7, "read symlink target");
  }
}

} // namespace luna::test

#endif
