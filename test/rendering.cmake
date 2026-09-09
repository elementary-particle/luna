add_executable(
  luna_render_parity
  render_parity.cpp
  ${PROJECT_SOURCE_DIR}/src/log.cpp
  ${PROJECT_SOURCE_DIR}/src/skia/canvas.cpp
  ${PROJECT_SOURCE_DIR}/src/skia/enums.cpp
  ${PROJECT_SOURCE_DIR}/src/skia/font_manager.cpp
  ${PROJECT_SOURCE_DIR}/src/skia/path.cpp
  ${PROJECT_SOURCE_DIR}/src/skia/shader.cpp
  ${PROJECT_SOURCE_DIR}/src/skia/text.cpp
  ${PROJECT_SOURCE_DIR}/src/blend2d/canvas.cpp
  ${PROJECT_SOURCE_DIR}/src/blend2d/enums.cpp
  ${PROJECT_SOURCE_DIR}/src/blend2d/font_manager.cpp
  ${PROJECT_SOURCE_DIR}/src/blend2d/path.cpp
  ${PROJECT_SOURCE_DIR}/src/blend2d/region.cpp
  ${PROJECT_SOURCE_DIR}/src/blend2d/shader.cpp
  ${PROJECT_SOURCE_DIR}/src/blend2d/text.cpp
  ${CMAKE_BINARY_DIR}/lua_helper.cpp
)

target_include_directories(luna_render_parity PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/vendor/tracy)
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
  luna_register_test(luna_render_parity rendering)
  set_tests_properties(luna_render_parity PROPERTIES TIMEOUT 600)
endif()

add_executable(
  luna_paragraph_metrics_test
  paragraph_metrics.cpp
  ${PROJECT_SOURCE_DIR}/src/log.cpp
  ${PROJECT_SOURCE_DIR}/src/file_vfs.cpp
  ${PROJECT_SOURCE_DIR}/src/package_file_source.cpp
  ${LUNA_NATIVE_FILE_SOURCE}
  ${PROJECT_SOURCE_DIR}/src/skia/canvas.cpp
  ${PROJECT_SOURCE_DIR}/src/skia/enums.cpp
  ${PROJECT_SOURCE_DIR}/src/skia/font_manager.cpp
  ${PROJECT_SOURCE_DIR}/src/skia/path.cpp
  ${PROJECT_SOURCE_DIR}/src/skia/shader.cpp
  ${PROJECT_SOURCE_DIR}/src/skia/text.cpp
  ${PROJECT_SOURCE_DIR}/src/blend2d/canvas.cpp
  ${PROJECT_SOURCE_DIR}/src/blend2d/enums.cpp
  ${PROJECT_SOURCE_DIR}/src/blend2d/font_manager.cpp
  ${PROJECT_SOURCE_DIR}/src/blend2d/path.cpp
  ${PROJECT_SOURCE_DIR}/src/blend2d/region.cpp
  ${PROJECT_SOURCE_DIR}/src/blend2d/shader.cpp
  ${PROJECT_SOURCE_DIR}/src/blend2d/text.cpp
)

target_include_directories(
  luna_paragraph_metrics_test PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/vendor/tracy)
target_compile_definitions(
  luna_paragraph_metrics_test PRIVATE
  LUNA_HAVE_BACKEND_SKIA=1
  LUNA_HAVE_BACKEND_BLEND2D=1
  LUNA_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
target_link_libraries(
  luna_paragraph_metrics_test PRIVATE
  lz4::lz4
  fmt::fmt
  SDL3::SDL3
  blend2d::blend2d
  unofficial::skia::skia
  unofficial::skia::modules::skparagraph
  unofficial::skia::modules::skunicode_icu
  luajit::lib
)

luna_register_test(luna_paragraph_metrics_test rendering)

# Keep the manual fuzz tool available even when it is not registered in CTest.
add_dependencies(luna_rendering_tests luna_render_parity)
