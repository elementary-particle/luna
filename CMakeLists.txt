cmake_minimum_required(VERSION 3.20)

project(Luna LANGUAGES C CXX)

# Enable C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")
add_subdirectory(vendor)

# Add the executable
add_executable(
  luna
  src/main.cpp
  src/engine.cpp
  src/factory.cpp
  src/mixer.cpp
  src/renderer.cpp
  src/vfs.cpp
)

set_target_properties(luna PROPERTIES OUTPUT_NAME "luna")

find_package(fmt CONFIG REQUIRED)
find_package(SDL3 CONFIG REQUIRED)
find_package(SDL3_image CONFIG REQUIRED)
find_package(SDL3_ttf CONFIG REQUIRED)
find_package(SDL3_mixer CONFIG REQUIRED)

# Link libraries
target_link_libraries(
  luna PRIVATE
  fmt::fmt
  SDL3::SDL3
  SDL3_image::SDL3_image
  SDL3_ttf::SDL3_ttf
  SDL3_mixer::SDL3_mixer
  luajit::lib
)
