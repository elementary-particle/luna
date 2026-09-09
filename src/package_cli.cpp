#include "native_file_source.h"
#include "package_file_source.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace luna::file;
namespace {
void Require(FileStatus s) {
  if (!s)
    throw std::runtime_error(
        s.operation() + ": " + s.path() + ": " + s.message());
}
std::string Utf8(const fs::path &p) {
  auto s = p.u8string();
  return {s.begin(), s.end()};
}
void Collect(
    FileSource &s, const std::string &dir, std::vector<std::string> &paths) {
  std::vector<FileInfo> entries;
  Require(s.List(dir, &entries));
  for (const auto &e : entries) {
    if (e.kind == FileKind::kDirectory)
      Collect(s, e.path, paths);
    else
      paths.push_back(e.path);
  }
}
int Run(const std::vector<std::string> &args) {
  if (args.size() == 3 && args[1] == "verify") {
    std::shared_ptr<FileSource> source;
    Require(OpenPackageFileSource(args[2], {}, &source));
    Require(VerifyPackage(*source));
    std::cout << "Package verified\n";
    return 0;
  }
  if (args.size() != 4 || args[1] != "create") {
    std::cerr << "Usage: luna_pack create INPUT_DIRECTORY OUTPUT.luna\n"
                 "       luna_pack verify ARCHIVE.luna\n";
    return 2;
  }
  auto root =
      fs::canonical(fs::path(std::u8string(args[2].begin(), args[2].end())));
  auto dest = fs::weakly_canonical(
      fs::absolute(fs::path(std::u8string(args[3].begin(), args[3].end()))));
  auto relative = dest.lexically_relative(root);
  if (!relative.empty() && *relative.begin() != "..")
    throw std::runtime_error("output must be outside input directory");
  if (fs::exists(dest))
    throw std::runtime_error("output already exists");
  std::shared_ptr<FileSource> source;
  Require(OpenNativeFileSource(Utf8(root), {}, &source));
  std::vector<std::string> paths;
  Collect(*source, "", paths);
  // An exclusively created sibling directory owns the temporary file.
  fs::path temp;
  std::random_device random;
  for (int i = 0; i < 100; ++i) {
    auto candidate =
        dest.parent_path() / (".luna-pack-" + std::to_string(random()));
    if (fs::create_directory(candidate)) {
      temp = candidate;
      break;
    }
  }
  if (temp.empty())
    throw std::runtime_error("cannot create temporary output");
  struct Cleanup {
    fs::path p;
    ~Cleanup() {
      std::error_code ec;
      fs::remove_all(p, ec);
    }
  } cleanup{temp};
  auto archive = temp / "archive";
  std::ofstream output(archive, std::ios::binary);
  Require(WritePackage(output, *source, std::move(paths)));
  output.close();
  if (!output)
    throw std::runtime_error("output close failed");
  std::shared_ptr<FileSource> check;
  Require(OpenPackageFileSource(Utf8(archive), {}, &check));
  Require(VerifyPackage(*check));
  check.reset();
  // Same-filesystem hard link publishes without overwriting an existing file.
  fs::create_hard_link(archive, dest);
  std::cout << "Created " << args[3] << '\n';
  return 0;
}
} // namespace
#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
#else
int main(int argc, char **argv) {
#endif
  try {
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i)
      args.push_back(Utf8(fs::path(argv[i])));
    return Run(args);
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
