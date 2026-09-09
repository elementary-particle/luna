#ifndef LUNA_TEST_TEMP_DIRECTORY_H
#define LUNA_TEST_TEMP_DIRECTORY_H

#include <filesystem>
#include <random>
#include <stdexcept>

namespace luna::test {

class TempDirectory {
public:
  TempDirectory() {
    std::random_device random;
    for (int attempt = 0; attempt < 100; ++attempt) {
      path_ = std::filesystem::temp_directory_path() /
          ("luna-test-" + std::to_string(random()));
      if (std::filesystem::create_directory(path_)) {
        return;
      }
    }
    throw std::runtime_error("failed to create temporary test directory");
  }
  ~TempDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

} // namespace luna::test

#endif
