cmake_minimum_required(VERSION 3.20)

project(minilua C)

if(${CMAKE_SYSTEM_NAME} STREQUAL Linux)
  find_library(LIBM_LIBRARIES NAMES m)
endif()

add_executable(minilua ${LUAJIT_DIR}/src/host/minilua.c)

if(MSVC)
  target_compile_options(minilua PRIVATE -D_CRT_SECURE_NO_WARNINGS)
endif()

if(LIBM_LIBRARIES)
  target_link_libraries(minilua ${LIBM_LIBRARIES})
endif()

if(MINILUA_CFLAGS)
  string(REPLACE ";" " " SMINILUA_CFLAGS "${MINILUA_CFLAGS}")
  set_target_properties(minilua PROPERTIES
    COMPILE_FLAGS "${SMINILUA_CFLAGS}")
endif()
