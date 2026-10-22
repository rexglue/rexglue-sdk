set(ENV{REX_MIDASM_GENERATED_DIR} "${GENERATED_DIR}")
execute_process(
  COMMAND "${UNIT_TESTS}" "The generated declarations and calls of source hooks*"
  WORKING_DIRECTORY "${UNIT_TESTS_DIR}"
  RESULT_VARIABLE _written
)
if (NOT _written EQUAL 0)
  message(FATAL_ERROR "the generated hook files were not written (${_written})")
endif ()
execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${BUILD_DIR}" --config "${CONFIG}" --target
          midasm-generated-hooks RESULT_VARIABLE _built
)
if (NOT _built EQUAL 0)
  message(FATAL_ERROR "the generated hooks did not compile against their bodies (${_built})")
endif ()
execute_process(
  COMMAND "${HOOKS}"
  RESULT_VARIABLE _ran
  OUTPUT_VARIABLE _output
)
if (NOT _ran EQUAL 0)
  message(FATAL_ERROR "the generated hooks ran wrong (${_ran}): ${_output}")
endif ()
