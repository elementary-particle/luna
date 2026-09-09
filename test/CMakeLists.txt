# Test suites share a flat source layout and stable executable names.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
add_custom_target(luna_behavior_tests)
add_custom_target(luna_rendering_tests)

function(luna_register_test target suite)
  add_test(NAME ${target} COMMAND ${target})
  set_tests_properties(${target} PROPERTIES
    LABELS "${suite}" TIMEOUT 60
    WORKING_DIRECTORY "${PROJECT_BINARY_DIR}")
  add_dependencies(luna_${suite}_tests ${target})
  # Several existing tests use assert for both checks and Lua calls.
  if(MSVC)
    target_compile_options(${target} PRIVATE /UNDEBUG)
  else()
    target_compile_options(${target} PRIVATE -UNDEBUG)
  endif()
endfunction()

include(behavior.cmake)
if(LUNA_BUILD_RENDERING_TESTS)
  if(LUNA_BACKEND_SKIA AND LUNA_BACKEND_BLEND2D)
    include(rendering.cmake)
  else()
    message(STATUS "Rendering tests require both Skia and Blend2D; skipping")
  endif()
endif()

# Exercise the real engine with SDL's dummy drivers on Linux.
if(LUNA_BUILD_APP AND LUNA_BUILD_RENDERING_TESTS AND LUNA_BACKEND_BLEND2D
    AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_test(NAME luna_engine_test COMMAND ${CMAKE_COMMAND}
    -DLUNA_APP=$<TARGET_FILE:luna>
    -DLUNA_FONT=${CMAKE_CURRENT_SOURCE_DIR}/assets/ABeeZee-Regular.ttf
    -P ${CMAKE_CURRENT_SOURCE_DIR}/engine.cmake)
  set_tests_properties(luna_engine_test PROPERTIES LABELS rendering TIMEOUT 60
    WORKING_DIRECTORY "${PROJECT_BINARY_DIR}")
  add_dependencies(luna_rendering_tests luna)
endif()
