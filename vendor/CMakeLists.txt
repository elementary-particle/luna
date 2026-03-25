cmake_minimum_required(VERSION 3.20)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}")
include(LuaJIT)

# For a cpu based alternative backend
# set(ASMJIT_DIR "${CMAKE_CURRENT_LIST_DIR}/asmjit")
# set(BLEND2D_STATIC ON)
# add_subdirectory(blend2d)
