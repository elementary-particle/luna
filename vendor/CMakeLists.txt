cmake_minimum_required(VERSION 3.20)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}")
include(LuaJIT)

if(LUNA_BACKEND_BLEND2D)
  if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/asmjit/CMakeLists.txt")
    message(FATAL_ERROR
      "LUNA_BACKEND_BLEND2D requires vendor/asmjit as a git submodule")
  endif()
  if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/blend2d/CMakeLists.txt")
    message(FATAL_ERROR
      "LUNA_BACKEND_BLEND2D requires vendor/blend2d as a git submodule")
  endif()

  set(ASMJIT_DIR "${CMAKE_CURRENT_LIST_DIR}/asmjit" CACHE PATH
      "Location of 'asmjit'" FORCE)
  set(BLEND2D_NO_STDCXX OFF CACHE BOOL
      "Use the shared C++/unwind runtime for Blend2D" FORCE)
  add_subdirectory(blend2d EXCLUDE_FROM_ALL)
endif()
