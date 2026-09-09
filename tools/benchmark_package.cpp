// Opt-in microbenchmark: compile with the same optimization for every source.
#include "native_file_source.h"
#include "package_file_source.h"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <random>
#include <stdexcept>
using namespace luna::file;
using Clock = std::chrono::steady_clock;
void Check(FileStatus s) {
  if (!s)
    throw std::runtime_error(s.message());
}
int main(int argc, char **argv) {
  if (argc != 4) {
    std::cerr << "ROOT ARCHIVE ASSET\n";
    return 2;
  }
  try {
    std::shared_ptr<FileSource> native, package;
    Check(OpenNativeFileSource(argv[1], {}, &native));
    auto mount = Clock::now();
    Check(OpenPackageFileSource(std::string(argv[2]), {}, &package));
    std::cout << "mount_ms "
              << std::chrono::duration<double, std::milli>(Clock::now() - mount)
                     .count()
              << '\n';
    uint64_t sink = 0;
    for (auto source : {native, package}) {
      FileInfo info;
      Check(source->Stat(argv[3], &info));
      if (info.size <= 4096)
        throw std::runtime_error("benchmark asset must exceed 4 KiB");
      std::unique_ptr<FileStream> stream;
      Check(source->OpenFile(argv[3], {}, &stream));
      std::vector<std::byte> bytes(4096);
      size_t got;
      auto start = Clock::now();
      Check(stream->Read(bytes, &got));
      sink += std::to_integer<unsigned>(bytes[0]);
      std::cout << source->DebugName() << " first_4k_us "
                << std::chrono::duration<double, std::micro>(
                       Clock::now() - start)
                       .count()
                << '\n';
      // Warm all integrity checks; OS page-cache warmth is not controlled here.
      MappedFile map;
      Check(source->MapFile(argv[3], {}, &map));
      start = Clock::now();
      std::mt19937 random(1729);
      for (int i = 0; i < 10000; ++i) {
        auto pos = random() % (info.size - 4096);
        Check(stream->Seek(pos, std::ios::beg));
        Check(stream->Read(bytes, &got));
        if (got != 4096)
          throw std::runtime_error("short read");
        sink += std::to_integer<unsigned>(bytes[i % 4096]);
      }
      std::cout << source->DebugName() << " warm_random_4k_us "
                << std::chrono::duration<double, std::micro>(
                       Clock::now() - start)
                       .count() /
              10000
                << '\n';
      start = Clock::now();
      for (int i = 0; i < 50; ++i) {
        Check(source->MapFile(argv[3], {}, &map));
        sink += std::to_integer<unsigned>(map.data()[i]);
      }
      std::cout << source->DebugName() << " warm_map_us "
                << std::chrono::duration<double, std::micro>(
                       Clock::now() - start)
                       .count() /
              50 << '\n';
    }
    std::cout << "sink " << sink << '\n';
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
