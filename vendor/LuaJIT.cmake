project(luajit C ASM)

set(LUAJIT_DIR ${CMAKE_CURRENT_LIST_DIR}/luajit)
set(LJ_DIR ${LUAJIT_DIR}/src)

if(NOT WIN32)
  include(GNUInstallDirs)
endif()

set(LUAJIT_BUILD_CLI ON CACHE BOOL "Build the command line interface")
set(LUAJIT_BUILD_SINGLE_SOURCE OFF CACHE BOOL "Build luajit with a single source unit")
set(LUAJIT_DISABLE_GC64 OFF CACHE BOOL "Disable GC64 mode for 64-bit targets")
set(LUA_MULTILIB "lib" CACHE PATH "Multilib")
set(LUAJIT_DISABLE_FFI OFF CACHE BOOL "Disable the FFI library")
set(LUAJIT_DISABLE_JIT OFF CACHE BOOL "Disable the JIT compiler")
set(LUAJIT_ENABLE_LUA52COMPAT ON CACHE BOOL "Enable Lua5.2 compatibility")
set(LUAJIT_NUMMODE 0 CACHE STRING
"The numerical mode to use. Possible values:
  0 - Default
  1 - Single
  2 - Dual
")

include(CheckTypeSize)
include(CheckCCompilerFlag)

if("${CMAKE_C_COMPILER_ID}" STREQUAL "GNU")
  include(Findunwind)
  check_unwind_tables(HAS_UNWIND_TABLES)
  if(HAS_UNWIND_TABLES)
    message(STATUS "Unwind support is available")
    set(LUAJIT_NO_UNWIND OFF)
  else()
    message(STATUS "Unwind support not available")
    set(LUAJIT_NO_UNWIND IGNORE)
  endif()
elseif(WIN32)
  set(LUAJIT_NO_UNWIND IGNORE)
else()
  check_c_compiler_flag(-funwind-tables HAVE_FLAG)
  if(HAVE_FLAG)
    set(LUAJIT_NO_UNWIND OFF)
    set(UNWIND_LIBRARY unwind)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -funwind-tables")
  else()
    set(LUAJIT_NO_UNWIND IGNORE)
  endif()
endif()
message(STATUS "LUAJIT_NO_UNWIND is set to ${LUAJIT_NO_UNWIND}")

if(CMAKE_CROSSCOMPILING)
  if(${CMAKE_HOST_SYSTEM_PROCESSOR} MATCHES 64)
    set(HOST_64 TRUE)
  else()
    set(HOST_64 FALSE)
  endif()
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(TARGET_64 TRUE)
  else()
    set(TARGET_64 FALSE)
  endif()
  message(STATUS "HOST_64 is ${HOST_64}")
  message(STATUS "TARGET_64 is ${TARGET_64}")
endif()
if(ANDROID)
  set(CROSS_C_FLAGS -target ${CMAKE_C_COMPILER_TARGET})
endif()

# Get preprocessor defines from lj_arch.h
if(NOT MSVC)
  set(LJ_CFLAGS -U_FORTIFY_SOURCE -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE)

  message(STATUS "CMAKE_C_COMPILER is ${CMAKE_C_COMPILER}")
  message(STATUS "CROSS_C_FLAGS is ${CROSS_C_FLAGS}")
  message(STATUS "CMAKE_C_FLAGS is ${CMAKE_C_FLAGS}")
  message(STATUS "LJ_CFLAGS is ${LJ_CFLAGS}")
  message(STATUS "Target: -E -dM lj_arch.h")

  execute_process(
    COMMAND ${CMAKE_C_COMPILER} ${CROSS_C_FLAGS}
      ${LJ_CFLAGS} -E -dM lj_arch.h
      WORKING_DIRECTORY ${LJ_DIR}
      OUTPUT_VARIABLE TARGET_TESTARCH
      RESULT_VARIABLE result
      ERROR_VARIABLE error_output
  )
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "Failed to preprocess lj_arch.h: ${error_output}")
  endif()
  message(DEBUG "Preprocessor output of lj_arch.h:\n${TARGET_TESTARCH}")

  if(LUA_TARGET_SHARED)
    add_definitions(-fPIC)
  endif()
endif()

set(HOST_CFLAGS)     # Build the buildvm for host platform
set(TARGET_ARCH)     # x86, x64, arm, arm64, ppc, mips, mips64, loongarch64

string(FIND "${TARGET_TESTARCH}" "LJ_TARGET_X64 1" HAVE_FLAG)
if(NOT HAVE_FLAG EQUAL -1)
  set(TARGET_ARCH "x64")
else()
  string(FIND "${TARGET_TESTARCH}" "LJ_TARGET_X86 1" HAVE_FLAG)
  if(NOT HAVE_FLAG EQUAL -1)
    set(TARGET_ARCH "x86")
  else()
    string(FIND "${TARGET_TESTARCH}" "LJ_TARGET_ARM 1" HAVE_FLAG)
    if(NOT HAVE_FLAG EQUAL -1)
      set(TARGET_ARCH "arm")
    else()
      string(FIND "${TARGET_TESTARCH}" "LJ_TARGET_ARM64 1" HAVE_FLAG)
      if(NOT HAVE_FLAG EQUAL -1)
        set(TARGET_ARCH "arm64")

        string(FIND "${TARGET_TESTARCH}" "__AARCH64EB__" HAVE_FLAG)
        if(NOT HAVE_FLAG EQUAL -1)
          list(APPEND HOST_CFLAGS -D__AARCH64EB__=1)
        endif()
      else()
        string(FIND "${TARGET_TESTARCH}" "LJ_TARGET_PPC 1" HAVE_FLAG)
        if(NOT HAVE_FLAG EQUAL -1)
          set(TARGET_ARCH "ppc")

          string(FIND "${TARGET_TESTARCH}" "LJ_LE 1" HAVE_FLAG)
          if(NOT HAVE_FLAG EQUAL -1)
            list(APPEND HOST_CFLAGS -DLJ_ARCH_ENDIAN=LUAJIT_LE)
          else()
            list(APPEND HOST_CFLAGS -DLJ_ARCH_ENDIAN=LUAJIT_BE)
          endif()
        else()
          string(FIND "${TARGET_TESTARCH}" "LJ_TARGET_MIPS 1" HAVE_FLAG)
          if(NOT HAVE_FLAG EQUAL -1)

            string(FIND "${TARGET_TESTARCH}" "MIPSEL" HAVE_FLAG)
            if(NOT HAVE_FLAG EQUAL -1)
              list(APPEND HOST_CFLAGS -D__MIPSEL__=1)
            endif()

            string(FIND "${TARGET_TESTARCH}" "LJ_TARGET_MIPS64 1" HAVE_FLAG)
            if(NOT HAVE_FLAG EQUAL -1)
              set(TARGET_ARCH "mips64")
            else()
              set(TARGET_ARCH "mips")
            endif()
          else()
            string(FIND "${TARGET_TESTARCH}" "LJ_TARGET_LOONGARCH64 1" HAVE_FLAG)
            if(NOT HAVE_FLAG EQUAL -1)
              set(TARGET_ARCH "loongarch64")
              list(APPEND HOST_CFLAGS -DLJ_ARCH_ENDIAN=LUAJIT_LE)
            elseif(NOT MSVC)
              message(FATAL_ERROR "Unsupported target architecture")
            endif()
          endif()
        endif()
      endif()
    endif()
  endif()
endif()

if(MSVC)
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "(AMD64|x86_64|X64)")
      set(TARGET_ARCH "x64")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "(ARM64|AArch64)")
      set(TARGET_ARCH "arm64")
    else()
      message(FATAL_ERROR "Unsupported 64-bit processor: ${CMAKE_SYSTEM_PROCESSOR}")
    endif()
  else()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "(i386|i686|x86|X86|AMD64)")
      set(TARGET_ARCH "x86")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "(ARM|armv7)")
      set(TARGET_ARCH "arm")
    else()
      message(FATAL_ERROR "Unsupported 32-bit processor: ${CMAKE_SYSTEM_PROCESSOR}")
    endif()
  endif()

  message(STATUS "Detected target architecture: ${TARGET_ARCH}")
endif()

string(FIND "${TARGET_TESTARCH}" "LJ_TARGET_PS3 1" HAVE_FLAG)
if(NOT HAVE_FLAG EQUAL -1)
  set(LJ_TARGET_SYS "PS3")
  list(APPEND HOST_CFLAGS -D__CELLOS_LV2__)
  list(APPEND HOST_CFLAGS -DLUAJIT_USE_SYSMALLOC)
  set(LIBPTHREAD_LIBRARIES pthread)
endif()

set(LJ_ENABLE_LARGEFILE ON)
if(ANDROID AND (CMAKE_SYSTEM_VERSION LESS 21))
  set(LJ_ENABLE_LARGEFILE OFF)
elseif(MSVC)
  set(LJ_ENABLE_LARGEFILE OFF)
endif()

if(NOT LJ_ENABLE_LARGEFILE)
  set(LJ_CFLAGS)
else()
  set(LJ_CFLAGS -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE)
endif()

# -fno-stack-protector
check_c_compiler_flag(-fno-stack-protector HAVE_FLAG)
if(HAVE_FLAG)
  list(APPEND LJ_CFLAGS -fno-stack-protector)
endif()

# DASM_FLAGS
set(DASM_FLAGS)

string(FIND "${TARGET_TESTARCH}" "LJ_LE 1" HAVE_FLAG)
if(MSVC OR NOT HAVE_FLAG EQUAL -1)
  set(DASM_FLAGS ${DASM_FLAGS} -D ENDIAN_LE)
else()
  set(DASM_FLAGS ${DASM_FLAGS} -D ENDIAN_BE)
endif()

string(FIND "${TARGET_TESTARCH}" "LJ_ARCH_BITS 64" HAVE_FLAG)
if(NOT HAVE_FLAG EQUAL -1)
  set(DASM_FLAGS ${DASM_FLAGS} -D P64)
elseif(TARGET_ARCH MATCHES "64")
  set(DASM_FLAGS ${DASM_FLAGS} -D P64)
endif()

string(FIND "${TARGET_TESTARCH}" "LJ_HASJIT 1" HAVE_FLAG)
if(MSVC OR NOT HAVE_FLAG EQUAL -1)
  set(DASM_FLAGS ${DASM_FLAGS} -D JIT)
endif()

string(FIND "${TARGET_TESTARCH}" "LJ_HASFFI 1" HAVE_FLAG)
if(MSVC OR NOT HAVE_FLAG EQUAL -1)
  set(DASM_FLAGS ${DASM_FLAGS} -D FFI)
endif()

string(FIND "${TARGET_TESTARCH}" "LJ_DUALNUM 1" HAVE_FLAG)
if(NOT HAVE_FLAG EQUAL -1)
  set(DASM_FLAGS ${DASM_FLAGS} -D DUALNUM)
endif()

string(FIND "${TARGET_TESTARCH}" "LJ_ARCH_HASFPU 1" HAVE_FLAG)
if(NOT HAVE_FLAG EQUAL -1)
  set(DASM_FLAGS ${DASM_FLAGS} -D FPU)
  list(APPEND HOST_CFLAGS -DLJ_ARCH_HASFPU=1)
else()
  list(APPEND HOST_CFLAGS -DLJ_ARCH_HASFPU=0)
endif()

string(FIND "${TARGET_TESTARCH}" "LJ_ABI_SOFTFP 1" HAVE_FLAG)
if(NOT HAVE_FLAG EQUAL -1)
  list(APPEND HOST_CFLAGS -DLJ_ABI_SOFTFP=1)
elseif(NOT MSVC)
  set(DASM_FLAGS ${DASM_FLAGS} -D HFABI)
  list(APPEND HOST_CFLAGS -DLJ_ABI_SOFTFP=0)
endif()

string(FIND "${TARGET_TESTARCH}" "LJ_NO_UNWIND 1" HAVE_FLAG)
if(NOT HAVE_FLAG EQUAL -1)
  set(DASM_FLAGS ${DASM_FLAGS} -D NO_UNWIND)
  list(APPEND HOST_CFLAGS -DLUAJIT_NO_UNWIND)
endif()

string(FIND "${TARGET_TESTARCH}" "LJ_ABI_PAUTH 1" HAVE_FLAG)
if(NOT HAVE_FLAG EQUAL -1)
  set(DASM_FLAGS ${DASM_FLAGS} -D PAUTH)
  list(APPEND HOST_CFLAGS -DLJ_ABI_PAUTH=1)
endif()

string(FIND "${TARGET_TESTARCH}" "LJ_ABI_BRANCH_TRACK 1" HAVE_FLAG)
if(NOT HAVE_FLAG EQUAL -1)
  set(DASM_FLAGS ${DASM_FLAGS} -D BRANCH_TRACK)
  list(APPEND HOST_CFLAGS -DLJ_ABI_BRANCH_TRACK=1)
endif()

string(FIND "${TARGET_TESTARCH}" "LJ_ABI_SHADOW_STACK 1" HAVE_FLAG)
if(NOT HAVE_FLAG EQUAL -1)
  set(DASM_FLAGS ${DASM_FLAGS} -D SHADOW_STACK)
  list(APPEND HOST_CFLAGS -DLJ_ABI_SHADOW_STACK=1)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL Windows)
  set(DASM_FLAGS ${DASM_FLAGS} -D WIN)
endif()

if(TARGET_ARCH STREQUAL x64)
  string(FIND "${TARGET_TESTARCH}" "LJ_FR2 1" HAVE_FLAG)
  if(NOT MSVC AND HAVE_FLAG EQUAL -1)
    set(TARGET_ARCH "x86")
  endif()
else()
  if(TARGET_ARCH STREQUAL arm)
    if(CMAKE_SYSTEM_NAME STREQUAL iOS)
      set(DASM_FLAGS ${DASM_FLAGS} -D IOS)
    endif()
  else()
    string(FIND "${TARGET_TESTARCH}" "LJ_TARGET_MIPSR6" HAVE_FLAG)
    if(NOT HAVE_FLAG EQUAL -1)
      set(DASM_FLAGS ${DASM_FLAGS} -D MIPSR6)
    endif()
    if(TARGET_ARCH STREQUAL ppc)
      string(FIND "${TARGET_TESTARCH}" "LJ_ARCH_SQRT 1" HAVE_FLAG)
      if(NOT HAVE_FLAG EQUAL -1)
        set(DASM_FLAGS ${DASM_FLAGS} -D SQRT)
      endif()
      string(FIND "${TARGET_TESTARCH}" "LJ_ARCH_ROUND 1" HAVE_FLAG)
      if(NOT HAVE_FLAG EQUAL -1)
        set(DASM_FLAGS ${DASM_FLAGS} -D ROUND)
      endif()
      string(FIND "${TARGET_TESTARCH}" "LJ_ARCH_PPC32ON64 1" HAVE_FLAG)
      if(NOT HAVE_FLAG EQUAL -1)
        set(DASM_FLAGS ${DASM_FLAGS} -D GPR64)
      endif()
      if(LJ_TARGET_SYS STREQUAL PS3)
        set(DASM_FLAGS ${DASM_FLAGS} -D PPE -D TOC)
      endif()
    endif()
  endif()
endif()

set(DASM_FLAGS ${DASM_FLAGS} -D VER=)
list(APPEND HOST_CFLAGS -DLUAJIT_TARGET=LUAJIT_ARCH_${TARGET_ARCH})

set(LJ_PREFIX "")

set(LJ_DEFINITIONS)
if("${LUAJIT_NO_UNWIND}" STREQUAL "ON")
  list(APPEND LJ_DEFINITIONS LUAJIT_NO_UNWIND)
elseif("${LUAJIT_NO_UNWIND}" STREQUAL "OFF")
  list(APPEND LJ_DEFINITIONS LUAJIT_UNWIND_EXTERNAL)
  list(APPEND HOST_CFLAGS -DLUAJIT_UNWIND_EXTERNAL)
endif()

if(ANDROID)
  list(APPEND LJ_DEFINITIONS LJ_NO_SYSTEM=1)
endif()

set(LJ_NUMMODE_SINGLE 0) # Single-number mode only.
set(LJ_NUMMODE_SINGLE_DUAL 1) # Default to single-number mode.
set(LJ_NUMMODE_DUAL 2) # Dual-number mode only.
set(LJ_NUMMODE_DUAL_SINGLE 3) # Default to dual-number mode.

set(LJ_ARCH_NUMMODE ${LJ_NUMMODE_DUAL})
if(LJ_HAS_FPU)
  set(LJ_ARCH_NUMMODE ${LJ_NUMMODE_DUAL_SINGLE})
endif()

if(("${TARGET_ARCH}" STREQUAL "x86") OR
    ("${TARGET_ARCH}" STREQUAL "x64"))
  set(LJ_ARCH_NUMMODE ${LJ_NUMMODE_SINGLE_DUAL})
endif()

if(("${TARGET_ARCH}" STREQUAL "arm") OR
    ("${TARGET_ARCH}" STREQUAL "arm64") OR
    ("${TARGET_ARCH}" STREQUAL "mips") OR
    ("${TARGET_ARCH}" STREQUAL "mips64"))
  set(LJ_ARCH_NUMMODE ${LJ_NUMMODE_DUAL})
endif()

# Enable or disable the dual-number mode for the VM.
if(((LJ_ARCH_NUMMODE EQUAL LJ_NUMMODE_SINGLE) AND (LUAJIT_NUMMODE EQUAL 2)) OR
    ((LJ_ARCH_NUMMODE EQUAL LJ_NUMMODE_DUAL) AND (LUAJIT_NUMMODE EQUAL 1)))
  message(FATAL_ERROR "No support for this number mode on this architecture")
endif()
if(
   (LJ_ARCH_NUMMODE EQUAL LJ_NUMMODE_DUAL) OR
   ((LJ_ARCH_NUMMODE EQUAL LJ_NUMMODE_DUAL_SINGLE) AND NOT
    (LUAJIT_NUMMODE EQUAL 1)) OR
   ((LJ_ARCH_NUMMODE EQUAL LJ_NUMMODE_SINGLE_DUAL) AND
    (LUAJIT_NUMMODE EQUAL 2))
  )
  set(LJ_DUALNUM 1)
else()
  set(LJ_DUALNUM 0)
endif()

set(BUILDVM_ARCH_H ${CMAKE_BINARY_DIR}/buildvm_arch.h)
set(DASM_PATH ${LUAJIT_DIR}/dynasm/dynasm.lua)

set(TARGET_OS_FLAGS "")
if(${CMAKE_SYSTEM_NAME} STREQUAL Android)
  set(TARGET_OS_FLAGS ${TARGET_OS_FLAGS}
    -DLUAJIT_OS=LUAJIT_OS_LINUX)
elseif(${CMAKE_SYSTEM_NAME} STREQUAL Windows)
  set(TARGET_OS_FLAGS ${TARGET_OS_FLAGS}
    -DLUAJIT_OS=LUAJIT_OS_WINDOWS)
elseif(${CMAKE_SYSTEM_NAME} MATCHES "Linux")
  set(TARGET_OS_FLAGS ${TARGET_OS_FLAGS}
    -DLUAJIT_OS=LUAJIT_OS_LINUX)
elseif(${CMAKE_SYSTEM_NAME} STREQUAL Haiku)
  set(TARGET_OS_FLAGS ${TARGET_OS_FLAGS}
    -DLUAJIT_OS=LUAJIT_OS_POSIX)
elseif(${CMAKE_SYSTEM_NAME} MATCHES "(Open|Free|Net)BSD")
  set(TARGET_OS_FLAGS ${TARGET_OS_FLAGS}
    -DLUAJIT_OS=LUAJIT_OS_BSD)
else()
  set(TARGET_OS_FLAGS ${TARGET_OS_FLAGS}
    -DLUAJIT_OS=LUAJIT_OS_OTHER)
endif()

if(LUAJIT_DISABLE_GC64)
  list(APPEND LJ_DEFINITIONS LUAJIT_DISABLE_GC64)
  list(APPEND HOST_CFLAGS -DLUAJIT_DISABLE_GC64)
endif()

list(APPEND HOST_CFLAGS ${TARGET_OS_FLAGS})

if(LUAJIT_DISABLE_FFI)
  list(APPEND LJ_DEFINITIONS LUAJIT_DISABLE_FFI)
  list(APPEND HOST_CFLAGS -DLUAJIT_DISABLE_FFI)
endif()
if(LUAJIT_DISABLE_JIT)
  list(APPEND LJ_DEFINITIONS LUAJIT_DISABLE_JIT)
  list(APPEND HOST_CFLAGS -DLUAJIT_DISABLE_JIT)
endif()

if(("${LUAJIT_NUMMODE}" STREQUAL "1") OR
    ("${LUAJIT_NUMMODE}" STREQUAL "2"))
  list(APPEND LJ_DEFINITIONS LUAJIT_NUMMODE=${LUAJIT_NUMMODE})
  list(APPEND HOST_CFLAGS -DLUAJIT_NUMMODE=${LUAJIT_NUMMODE})
endif()

if(LUAJIT_ENABLE_GDBJIT)
  list(APPEND LJ_DEFINITIONS LUAJIT_ENABLE_GDBJIT)
  list(APPEND HOST_CFLAGS -DLUAJIT_ENABLE_GDBJIT)
endif()

if(LUAJIT_ENABLE_LUA52COMPAT)
  list(APPEND LJ_DEFINITIONS LUAJIT_ENABLE_LUA52COMPAT)
endif()

if(MINGW)
  list(APPEND HOST_CFLAGS -malign-double)
endif()

set(VM_DASC_PATH ${LJ_DIR}/vm_${TARGET_ARCH}.dasc)

message(STATUS "DASM_FLAGS: ${DASM_FLAGS}")
message(STATUS "HOST_CFLAGS: ${HOST_CFLAGS}")

# Build the minilua for host platform
set(MINILUA minilua)
if(HOST_WINE)
  set(MINILUA minilua.exe)
endif()

list(JOIN HOST_CFLAGS " " MINILUA_CFLAGS)
if(NOT CMAKE_CROSSCOMPILING)
  add_subdirectory(minilua)
  set(MINILUA_PATH $<TARGET_FILE:minilua>)
else()
  make_directory(${CMAKE_BINARY_DIR}/minilua)
  ExternalProject_Add(minilua
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/minilua
    BINARY_DIR ${CMAKE_BINARY_DIR}/minilua
    CMAKE_ARGS
      -DCMAKE_BUILD_TYPE=$<IF:$<BOOL:${CMAKE_BUILD_TYPE}>,${CMAKE_BUILD_TYPE},Release>
    INSTALL_COMMAND ""
  )
  set(MINILUA_PATH "${CMAKE_BINARY_DIR}/minilua/${LJ_PREFIX}${MINILUA}")
endif()

# Generate luajit.h
set(GIT_FORMAT %ct)
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
  set(GIT_FORMAT %%ct)
endif()

execute_process(
  COMMAND git --version
  RESULT_VARIABLE GIT_EXISTENCE
  OUTPUT_VARIABLE GIT_VERSION
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

execute_process(
  COMMAND git rev-parse --is-inside-work-tree
  RESULT_VARIABLE GIT_IN_REPOSITORY
  OUTPUT_VARIABLE GIT_IS_IN_REPOSITORY
  OUTPUT_STRIP_TRAILING_WHITESPACE
  WORKING_DIRECTORY ${LUAJIT_DIR}
)

if((GIT_EXISTENCE EQUAL 0) AND (GIT_IN_REPOSITORY EQUAL 0))
  message(STATUS "Using Git: ${GIT_VERSION}")
  add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/luajit_relver.txt
    COMMAND git -c log.showSignature=false show -s --format=${GIT_FORMAT}
      > ${CMAKE_BINARY_DIR}/luajit_relver.txt
    WORKING_DIRECTORY ${LUAJIT_DIR}
  )
else()
  string(TIMESTAMP current_epoch "%s")
  message(STATUS "Using current epoch: ${current_epoch}")
  add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/luajit_relver.txt
    COMMAND echo "${current_epoch}"
      > ${CMAKE_BINARY_DIR}/luajit_relver.txt
    WORKING_DIRECTORY ${LUAJIT_DIR}
   )
endif()

add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/luajit.h
  COMMAND ${MINILUA_PATH} ${LUAJIT_DIR}/src/host/genversion.lua
  ARGS ${LUAJIT_DIR}/src/luajit_rolling.h
       ${CMAKE_BINARY_DIR}/luajit_relver.txt
       ${CMAKE_BINARY_DIR}/luajit.h
  DEPENDS ${LUAJIT_DIR}/src/luajit_rolling.h
  DEPENDS ${CMAKE_BINARY_DIR}/luajit_relver.txt
)

# Generate buildvm_arch.h
add_custom_command(OUTPUT ${BUILDVM_ARCH_H}
  COMMAND ${MINILUA_PATH} ${DASM_PATH} ${DASM_FLAGS}
          -o ${BUILDVM_ARCH_H} ${VM_DASC_PATH}
  DEPENDS minilua ${DASM_PATH} ${CMAKE_BINARY_DIR}/luajit.h)
add_custom_target(buildvm_arch_h ALL
  DEPENDS ${BUILDVM_ARCH_H}
)

# Build the buildvm for host platform
if(LUAJIT_ENABLE_LUA52COMPAT)
  set(BUILDVM_CFLAGS -DLUAJIT_ENABLE_LUA52COMPAT)
endif()
set(BUILDVM_COMPILER_FLAGS_PATH
  "${CMAKE_BINARY_DIR}/buildvm_flags.config")
file(WRITE ${BUILDVM_COMPILER_FLAGS_PATH} "${BUILDVM_CFLAGS} ${HOST_CFLAGS}")

set(BUILDVM buildvm)
if(HOST_WINE)
  set(BUILDVM buildvm.exe)
endif()

if(NOT CMAKE_CROSSCOMPILING)
  set(BUILDVM_COMPILER_FLAGS "${BUILDVM_CFLAGS} ${HOST_CFLAGS}")
  add_subdirectory(buildvm)
  set(BUILDVM_PATH $<TARGET_FILE:buildvm>)
  add_dependencies(buildvm buildvm_arch_h)
else()
  set(BUILDVM_PATH "${CMAKE_BINARY_DIR}/buildvm/${LJ_PREFIX}${BUILDVM}")
  make_directory(${CMAKE_BINARY_DIR}/buildvm)
  ExternalProject_Add(buildvm
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/buildvm
    BINARY_DIR ${CMAKE_BINARY_DIR}/buildvm
    CMAKE_ARGS
      -DCMAKE_BUILD_TYPE=$<IF:$<BOOL:${CMAKE_BUILD_TYPE}>,${CMAKE_BUILD_TYPE},Release>
      -DLUAJIT_DIR=${LUAJIT_DIR}
      -DEXTRA_COMPILER_FLAGS_FILE=${BUILDVM_COMPILER_FLAGS_PATH}
    DEPENDS buildvm_arch_h
    INSTALL_COMMAND ""
  )
endif()

set(LJVM_MODE elfasm)
if(WIN32)
  set(LJVM_MODE peobj)
endif()

set(LJ_VM_NAME lj_vm.S)
if("${LJVM_MODE}" STREQUAL "peobj")
  set(LJ_VM_NAME lj_vm.obj)
endif()

set(LJ_VM_S_PATH ${CMAKE_BINARY_DIR}/${LJ_VM_NAME})
add_custom_command(OUTPUT ${LJ_VM_S_PATH}
  COMMAND ${BUILDVM_PATH} -m ${LJVM_MODE} -o ${LJ_VM_S_PATH}
  DEPENDS buildvm
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/)

make_directory(${CMAKE_BINARY_DIR}/jit)
set(LJ_LIBDEF_PATH ${CMAKE_BINARY_DIR}/lj_libdef.h)
set(LJ_RECDEF_PATH ${CMAKE_BINARY_DIR}/lj_recdef.h)
set(LJ_FFDEF_PATH ${CMAKE_BINARY_DIR}/lj_ffdef.h)
set(LJ_BCDEF_PATH ${CMAKE_BINARY_DIR}/lj_bcdef.h)
set(LJ_VMDEF_PATH ${CMAKE_BINARY_DIR}/jit/vmdef.lua)

set(LJ_LIB_SOURCES
  ${LJ_DIR}/lib_base.c ${LJ_DIR}/lib_math.c ${LJ_DIR}/lib_bit.c
  ${LJ_DIR}/lib_string.c ${LJ_DIR}/lib_table.c ${LJ_DIR}/lib_io.c
  ${LJ_DIR}/lib_os.c ${LJ_DIR}/lib_package.c ${LJ_DIR}/lib_debug.c
  ${LJ_DIR}/lib_jit.c ${LJ_DIR}/lib_ffi.c ${LJ_DIR}/lib_buffer.c)
add_custom_command(
  OUTPUT ${LJ_LIBDEF_PATH} ${LJ_VMDEF_PATH} ${LJ_RECDEF_PATH} ${LJ_FFDEF_PATH}
  OUTPUT ${LJ_BCDEF_PATH}
  COMMAND
    ${BUILDVM_PATH} -m libdef -o ${LJ_LIBDEF_PATH} ${LJ_LIB_SOURCES}
  COMMAND
    ${BUILDVM_PATH} -m recdef -o ${LJ_RECDEF_PATH} ${LJ_LIB_SOURCES}
  COMMAND
    ${BUILDVM_PATH} -m ffdef -o ${LJ_FFDEF_PATH} ${LJ_LIB_SOURCES}
  COMMAND
    ${BUILDVM_PATH} -m bcdef -o ${LJ_BCDEF_PATH} ${LJ_LIB_SOURCES}
  COMMAND
    ${BUILDVM_PATH} -m vmdef -o ${LJ_VMDEF_PATH} ${LJ_LIB_SOURCES}
  DEPENDS buildvm ${LJ_LIB_SOURCE}
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/)

add_custom_target(lj_gen_headers ALL
  DEPENDS ${LJ_LIBDEF_PATH} ${LJ_RECDEF_PATH} ${LJ_VMDEF_PATH}
  DEPENDS ${LJ_FFDEF_PATH} ${LJ_BCDEF_PATH}
)

set(LJ_FOLDDEF_PATH ${CMAKE_BINARY_DIR}/lj_folddef.h)

set(LJ_FOLDDEF_SOURCE ${LJ_DIR}/lj_opt_fold.c)
add_custom_command(
  OUTPUT ${LJ_FOLDDEF_PATH}
  COMMAND
    ${BUILDVM_PATH} -m folddef -o ${LJ_FOLDDEF_PATH} ${LJ_FOLDDEF_SOURCE}
  DEPENDS ${BUILDVM_PATH}
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/)

add_custom_target(lj_gen_folddef ALL
  DEPENDS ${LJ_FOLDDEF_PATH}
)

file(GLOB_RECURSE SRC_LJCORE    "${LJ_DIR}/lj_*.c")
file(GLOB_RECURSE SRC_LIBCORE   "${LJ_DIR}/lib_*.c")

if(LUAJIT_BUILD_SINGLE_SOURCE)
  set(luajit_sources ${LJ_DIR}/ljamalg.c ${LJ_VM_S_PATH})
else()
  set(luajit_sources ${SRC_LIBCORE} ${SRC_LJCORE} ${LJ_VM_S_PATH})
endif()

if(WIN32)
  list(APPEND LJ_DEFINITIONS _CRT_SECURE_NO_WARNINGS)

  if(BUILD_SHARED_LIBS)
    list(APPEND LJ_DEFINITIONS LUA_BUILD_AS_DLL)
  endif()
endif()

# Build the luajit static library
add_library(libluajit ${luajit_sources})
if(MSVC)
  set_target_properties(libluajit PROPERTIES OUTPUT_NAME libluajit)
else()
  set_target_properties(libluajit PROPERTIES OUTPUT_NAME luajit)
endif()
if(LJ_CFLAGS)
  message(STATUS "LJ_CFLAGS: ${LJ_CFLAGS}")
  list(JOIN LJ_CFLAGS " " SLJ_CFLAGS)
  set_target_properties(libluajit PROPERTIES COMPILE_FLAGS "${SLJ_CFLAGS}")
endif()

add_dependencies(libluajit
  buildvm_arch_h
  buildvm
  lj_gen_headers
  lj_gen_folddef)
target_include_directories(libluajit PRIVATE
  ${CMAKE_BINARY_DIR}
  ${CMAKE_SOURCE_DIR})
target_include_directories(libluajit PUBLIC ${CMAKE_BINARY_DIR} ${LJ_DIR})

if(${CMAKE_SYSTEM_NAME} STREQUAL Linux)
  find_library(LIBM_LIBRARIES NAMES m)
  find_library(LIBDL_LIBRARIES NAMES dl)
endif()

if(LIBM_LIBRARIES)
  target_link_libraries(libluajit ${LIBM_LIBRARIES})
endif()

if(LIBDL_LIBRARIES)
  target_link_libraries(libluajit ${LIBDL_LIBRARIES})
endif()

if(UNWIND_LIBRARY)
  target_link_libraries(libluajit ${UNWIND_LIBRARY})
endif()

list(APPEND LJ_DEFINITIONS LUA_MULTILIB="${LUA_MULTILIB}")

message(STATUS "LJ_DEFINITIONS: ${LJ_DEFINITIONS}")

target_compile_definitions(libluajit PRIVATE ${LJ_DEFINITIONS})

if(CMAKE_C_COMPILER_ID MATCHES "Clang")
  # Any Clang
  # Since the assembler part does NOT maintain a frame pointer, it's pointless
  # to slow down the C part by not omitting it. Debugging, tracebacks and
  # unwinding are not affected -- the assembler part has frame unwind
  # information and GCC emits it where needed (x64) or with -g (see CCDEBUG).
  add_compile_options(-fomit-frame-pointer)
  if(CMAKE_C_COMPILER_ID MATCHES "^AppleClang$")
    # Apple Clang only
    add_compile_options(
      -faligned-allocation
      -fasm-blocks
    )
  endif()
endif()

if("${TARGET_ARCH}" STREQUAL "x86")
  if(CMAKE_COMPILER_IS_CLANG OR CMAKE_COMPILER_IS_GNUC)
    target_compile_options(libluajit PRIVATE
      -march=i686 -msse -msse2 -mfpmath=sse)
  endif()
  if(MSVC)
    target_compile_options(libluajit PRIVATE "/arch:SSE2")
  endif()
endif()

set(LJ_COMPILE_OPTIONS -U_FORTIFY_SOURCE)
if(NO_STACK_PROTECTOR_FLAG)
  set(LJ_COMPILE_OPTIONS ${LJ_COMPILE_OPTIONS} -fno-stack-protector)
endif()
if(IOS AND ("${TARGET_ARCH}" STREQUAL "arm64"))
  set(LJ_COMPILE_OPTIONS ${LJ_COMPILE_OPTIONS} -fno-omit-frame-pointer)
endif()

target_compile_options(libluajit PRIVATE ${LJ_COMPILE_OPTIONS})
if(MSVC)
  target_compile_options(libluajit PRIVATE
    "/D_CRT_STDIO_INLINE=__declspec(dllexport)__inline")
endif()

if("${TARGET_ARCH}" STREQUAL "Loongarch64")
  target_compile_options(libluajit PRIVATE "-fwrapv")
endif()

set(luajit_headers
  ${LJ_DIR}/lauxlib.h
  ${LJ_DIR}/lua.h
  ${LJ_DIR}/luaconf.h
  ${LJ_DIR}/lualib.h
  ${CMAKE_BINARY_DIR}/luajit.h)
install(FILES ${luajit_headers} DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/luajit)
install(TARGETS libluajit
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

# Build the luajit binary
if(LUAJIT_BUILD_CLI)
  add_executable(luajit ${LJ_DIR}/luajit.c)
  target_link_libraries(luajit libluajit)
  target_include_directories(luajit PRIVATE
    ${CMAKE_BINARY_DIR}
    ${LJ_DIR}
  )

  if(MINGW)
    target_link_libraries(luajit m)
  elseif(CMAKE_COMPILER_IS_CLANG OR CMAKE_COMPILER_IS_GNUC)
    target_link_libraries(luajit c m)
  endif()

  if(UNWIND_LIBRARY)
    target_link_libraries(luajit ${UNWIND_LIBRARY})
  endif()

  target_compile_definitions(luajit PRIVATE ${LJ_DEFINITIONS})
  file(COPY ${LJ_DIR}/jit DESTINATION ${CMAKE_BINARY_DIR})

  install(TARGETS luajit DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()

add_library(luajit-header INTERFACE)
target_include_directories(luajit-header INTERFACE ${LJ_DIR})

add_library(luajit::lib ALIAS libluajit)
add_library(luajit::header ALIAS luajit-header)
if(LUAJIT_BUILD_CLI)
  add_executable(luajit::lua ALIAS luajit)
endif()
