cmake_minimum_required(VERSION 3.20)

project(Luna LANGUAGES C CXX)

# Enable C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(LUNA_ENABLE_SANITIZERS
  "Enable address and undefined behavior sanitizers for supported debug builds"
  OFF)
option(LUNA_BACKEND_SKIA
  "Enable Skia - Vulkan render backend"
  ON)
option(LUNA_BACKEND_BLEND2D
  "Enable Blend2D - CPU render backend"
  ON)
option(LUNA_ENABLE_RENDER_PARITY_TEST
  "Add the render parity fuzz test to ctest"
  OFF)

if(NOT LUNA_BACKEND_SKIA AND NOT LUNA_BACKEND_BLEND2D)
  message(FATAL_ERROR "At least one renderer backend must be enabled")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")
add_subdirectory(vendor)
include(CTest)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS src/backend/helper.lua)
file(READ src/backend/helper.lua BACKEND_HELPER_LUA)
configure_file(src/backend/lua_helper.cpp.in ${CMAKE_BINARY_DIR}/lua_helper.cpp @ONLY)

# Add the executable
add_executable(
  luna
  src/main.cpp
  src/engine.cpp
  src/factory.cpp
  src/log.cpp
  src/mixer.cpp
  src/renderer_interface.cpp
  src/renderer_factory.cpp
  src/vfs.cpp
  vendor/tracy/TracyClient.cpp
  ${CMAKE_BINARY_DIR}/lua_helper.cpp
)

target_include_directories(luna PRIVATE src)

set_target_properties(luna PROPERTIES OUTPUT_NAME "luna")

target_compile_definitions(luna PRIVATE TRACY_ENABLE TRACY_VK_USE_SYMBOL_TABLE TRACY_ON_DEMAND)
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
find_package(SDL3 CONFIG REQUIRED)
find_package(SDL3_mixer CONFIG REQUIRED)

target_link_libraries(
  luna PRIVATE
  fmt::fmt
  SDL3::SDL3
  SDL3_mixer::SDL3_mixer
  luajit::lib
)

if(LUNA_BACKEND_SKIA)
  target_compile_definitions(luna PRIVATE LUNA_HAVE_BACKEND_SKIA=1)
  target_sources(
    luna PRIVATE
    src/skia/canvas.cpp
    src/skia/enums.cpp
    src/skia/font_manager.cpp
    src/skia/path.cpp
    src/skia/renderer.cpp
    src/skia/shader.cpp
    src/skia/text.cpp
    src/skia/vma.cpp
  )

  find_package(VulkanHeaders CONFIG REQUIRED)
  find_package(VulkanMemoryAllocator CONFIG REQUIRED)
  find_package(unofficial-skia CONFIG REQUIRED)

  target_link_libraries(
    luna PRIVATE
    Vulkan::Headers
    GPUOpen::VulkanMemoryAllocator
    unofficial::skia::skia
    unofficial::skia::modules::skparagraph
    unofficial::skia::modules::skunicode_icu
  )
endif()

if(LUNA_BACKEND_BLEND2D)
  target_compile_definitions(luna PRIVATE LUNA_HAVE_BACKEND_BLEND2D=1)
  target_sources(
    luna PRIVATE
    src/blend2d/canvas.cpp
    src/blend2d/enums.cpp
    src/blend2d/font_manager.cpp
    src/blend2d/path.cpp
    src/blend2d/region.cpp
    src/blend2d/renderer.cpp
    src/blend2d/shader.cpp
    src/blend2d/text.cpp
  )

  target_link_libraries(luna PRIVATE blend2d::blend2d)
endif()

if(LUNA_BACKEND_SKIA AND LUNA_BACKEND_BLEND2D)
  add_executable(
    luna_render_parity
    test/render_parity.cpp
    src/log.cpp
    src/skia/canvas.cpp
    src/skia/enums.cpp
    src/skia/font_manager.cpp
    src/skia/path.cpp
    src/skia/shader.cpp
    src/skia/text.cpp
    src/blend2d/canvas.cpp
    src/blend2d/enums.cpp
    src/blend2d/font_manager.cpp
    src/blend2d/path.cpp
    src/blend2d/region.cpp
    src/blend2d/shader.cpp
    src/blend2d/text.cpp
    ${CMAKE_BINARY_DIR}/lua_helper.cpp
  )

  target_include_directories(luna_render_parity PRIVATE src vendor/tracy)
  target_compile_definitions(
    luna_render_parity PRIVATE
    LUNA_HAVE_BACKEND_SKIA=1
    LUNA_HAVE_BACKEND_BLEND2D=1
  )
  target_link_libraries(
    luna_render_parity PRIVATE
    fmt::fmt
    SDL3::SDL3
    blend2d::blend2d
    unofficial::skia::skia
    unofficial::skia::modules::skparagraph
    unofficial::skia::modules::skunicode_icu
    luajit::lib
  )

  if(LUNA_ENABLE_RENDER_PARITY_TEST)
    add_test(NAME luna_render_parity COMMAND luna_render_parity)
  endif()

  add_executable(
    luna_paragraph_metrics_test
    test/paragraph_metrics.cpp
    src/log.cpp
    src/skia/canvas.cpp
    src/skia/enums.cpp
    src/skia/font_manager.cpp
    src/skia/path.cpp
    src/skia/shader.cpp
    src/skia/text.cpp
    src/blend2d/canvas.cpp
    src/blend2d/enums.cpp
    src/blend2d/font_manager.cpp
    src/blend2d/path.cpp
    src/blend2d/region.cpp
    src/blend2d/shader.cpp
    src/blend2d/text.cpp
  )

  target_include_directories(
    luna_paragraph_metrics_test PRIVATE src vendor/tracy)
  target_compile_definitions(
    luna_paragraph_metrics_test PRIVATE
    LUNA_HAVE_BACKEND_SKIA=1
    LUNA_HAVE_BACKEND_BLEND2D=1
    LUNA_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  )
  target_link_libraries(
    luna_paragraph_metrics_test PRIVATE
    fmt::fmt
    SDL3::SDL3
    blend2d::blend2d
    unofficial::skia::skia
    unofficial::skia::modules::skparagraph
    unofficial::skia::modules::skunicode_icu
    luajit::lib
  )

  add_test(NAME luna_paragraph_metrics_test COMMAND luna_paragraph_metrics_test)
endif()
