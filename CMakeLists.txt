cmake_minimum_required(VERSION 3.20)

project(Luna LANGUAGES C CXX)

# Enable C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(LUNA_ENABLE_SANITIZERS
  "Enable address and undefined behavior sanitizers for supported debug builds"
  OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")
add_subdirectory(vendor)

# Add the executable
add_executable(
  luna
  src/main.cpp
  src/engine.cpp
  src/factory.cpp
  src/font_manager.cpp
  src/log.cpp
  src/mixer.cpp
  src/canvas.cpp
  src/canvas_paragraph.cpp
  src/canvas_path.cpp
  src/canvas_shader.cpp
  src/renderer.cpp
  src/vfs.cpp
  src/vma.cpp
  vendor/tracy/TracyClient.cpp
)

set_target_properties(luna PROPERTIES OUTPUT_NAME "luna")

target_compile_definitions(luna PRIVATE TRACY_ENABLE)
target_include_directories(luna PRIVATE vendor/tracy)

if(LUNA_ENABLE_SANITIZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    message(FATAL_ERROR
      "LUNA_ENABLE_SANITIZERS requires a Clang- or GCC-compatible C++ compiler")
  endif()

  target_compile_options(
    luna PRIVATE
    $<$<CONFIG:Debug>:-fsanitize=address,undefined>
    $<$<CONFIG:Debug>:-fno-omit-frame-pointer>
  )
  target_link_options(
    luna PRIVATE
    $<$<CONFIG:Debug>:-fsanitize=address,undefined>
  )
endif()

find_package(fmt CONFIG REQUIRED)
find_package(VulkanHeaders CONFIG REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(SDL3 CONFIG REQUIRED)
find_package(SDL3_mixer CONFIG REQUIRED)
find_package(unofficial-skia CONFIG REQUIRED)

# Link libraries
target_link_libraries(
  luna PRIVATE
  Vulkan::Headers
  GPUOpen::VulkanMemoryAllocator
  fmt::fmt
  SDL3::SDL3
  SDL3_mixer::SDL3_mixer
  unofficial::skia::skia
  unofficial::skia::modules::skparagraph
  unofficial::skia::modules::skunicode_icu
  luajit::lib
)
