add_executable(luna_svg_path_test svg_path.cpp)
target_include_directories(luna_svg_path_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
luna_register_test(luna_svg_path_test behavior)

add_executable(
  luna_file_vfs_test
  file_vfs.cpp
  ${PROJECT_SOURCE_DIR}/src/file_vfs.cpp
  ${PROJECT_SOURCE_DIR}/src/package_file_source.cpp
  ${LUNA_NATIVE_FILE_SOURCE}
)
target_include_directories(luna_file_vfs_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(
  luna_file_vfs_test PRIVATE LUNA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
luna_register_test(luna_file_vfs_test behavior)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(
    luna_native_file_source_linux_test
    native_file_source_linux.cpp
    ${PROJECT_SOURCE_DIR}/src/file_vfs.cpp
    ${PROJECT_SOURCE_DIR}/src/native_file_source_linux.cpp
  )
  target_include_directories(luna_native_file_source_linux_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
  luna_register_test(luna_native_file_source_linux_test behavior)
elseif(WIN32)
  add_executable(
    luna_native_file_source_windows_test
    native_file_source_windows.cpp
    ${PROJECT_SOURCE_DIR}/src/file_vfs.cpp
    ${PROJECT_SOURCE_DIR}/src/native_file_source_windows.cpp
  )
  target_include_directories(luna_native_file_source_windows_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
  luna_register_test(luna_native_file_source_windows_test behavior)
endif()

add_executable(
  luna_fs_test
  filesystem.cpp
  ${PROJECT_SOURCE_DIR}/src/vfs.cpp
  ${PROJECT_SOURCE_DIR}/src/file_vfs.cpp
  ${PROJECT_SOURCE_DIR}/src/package_file_source.cpp
  ${LUNA_NATIVE_FILE_SOURCE}
  ${PROJECT_SOURCE_DIR}/src/lua_json.cpp
)
target_include_directories(luna_fs_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(
  luna_fs_test PRIVATE LUNA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(
  luna_fs_test PRIVATE
  SDL3::SDL3
  luajit::lib
)
luna_register_test(luna_fs_test behavior)

add_executable(
  luna_input_system_test
  input_system.cpp
  ${PROJECT_SOURCE_DIR}/src/input_system.cpp
  ${PROJECT_SOURCE_DIR}/src/renderer_interface.cpp
  ${PROJECT_SOURCE_DIR}/src/log.cpp
)
target_include_directories(luna_input_system_test PRIVATE
  ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/vendor/tracy)
target_link_libraries(
  luna_input_system_test PRIVATE
  fmt::fmt
  SDL3::SDL3
  luajit::lib
)
luna_register_test(luna_input_system_test behavior)

target_link_libraries(luna_file_vfs_test PRIVATE lz4::lz4)
target_link_libraries(luna_fs_test PRIVATE lz4::lz4)
add_executable(luna_package_test package.cpp
  ${PROJECT_SOURCE_DIR}/src/package_file_source.cpp
  ${PROJECT_SOURCE_DIR}/src/file_vfs.cpp ${LUNA_NATIVE_FILE_SOURCE})
target_include_directories(luna_package_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(luna_package_test PRIVATE lz4::lz4)
luna_register_test(luna_package_test behavior)
