function(check_unwind_tables output_var)
  set(TEST_DIR ${CMAKE_CURRENT_BINARY_DIR}/unwind_check)
  file(MAKE_DIRECTORY ${TEST_DIR})

  set(TEST_SRC_FILE ${TEST_DIR}/test.c)
  file(WRITE ${TEST_SRC_FILE} "
      extern void b(void);
      int a(void) { b(); return 0; }
  ")

  set(OBJ_FILE ${TEST_DIR}/test.o)
  execute_process(
    COMMAND ${CMAKE_C_COMPILER} ${CMAKE_C_FLAGS} -c ${TEST_SRC_FILE} -o ${OBJ_FILE}
    WORKING_DIRECTORY ${TEST_DIR}
    RESULT_VARIABLE COMPILE_RESULT
    ERROR_QUIET
    OUTPUT_QUIET
  )

  if(COMPILE_RESULT EQUAL 0 AND EXISTS ${OBJ_FILE})
    find_program(READELF readelf)
    find_program(OBJDUMP objdump)

    if(READELF)
      execute_process(
        COMMAND ${READELF} -S ${OBJ_FILE}
        OUTPUT_VARIABLE SECTIONS
        ERROR_QUIET
      )
      if(SECTIONS MATCHES "\.eh_frame|\.eh_frame_hdr")
        set(${output_var} TRUE PARENT_SCOPE)
      else()
        set(${output_var} FALSE PARENT_SCOPE)
      endif()
    elseif(OBJDUMP)
      execute_process(
        COMMAND ${OBJDUMP} -h ${OBJ_FILE}
        OUTPUT_VARIABLE HEADERS
        ERROR_QUIET
      )
      if(HEADERS MATCHES "\.eh_frame")
        set(${output_var} TRUE PARENT_SCOPE)
      else()
        set(${output_var} FALSE PARENT_SCOPE)
      endif()
    endif()
  else()
    set(${output_var} FALSE PARENT_SCOPE)
  endif()

  file(REMOVE_RECURSE ${TEST_DIR})
endfunction()

if (HAVE_UNWIND_LIB AND HAVE_UNWIND_H)
  set(unwind_FOUND ON)
endif()
