set(CMAKE_FOLDER "tests")

add_subdirectory(modules)

# The provider modules hold no dynamic initializer
rexglue_audit_provider_init(rexfilesystem)
rexglue_audit_provider_init(rexfilesystem-objects)
rexglue_audit_provider_init(rexapu)
rexglue_audit_provider_init(rexhid)
rexglue_audit_provider_init(rexui)
rexglue_audit_provider_init(rexgpu)
rexglue_audit_provider_init(rexxam)
rexglue_audit_provider_init(rexxbdm)
rexglue_audit_provider_init(rexapu-sdl3)
rexglue_audit_provider_init(rexhid-sdl3)
rexglue_audit_provider_init(rexui-sdl3)
if (WIN32)
  rexglue_audit_provider_init(rexhid-xinput)
endif ()

# The shared Xenos code reaches a title only through the title context
add_test(NAME gpu_no_runtime_include
         COMMAND ${CMAKE_COMMAND} -DGPU_SOURCE_DIR=${PROJECT_SOURCE_DIR}/src/gpu -P
                 ${PROJECT_SOURCE_DIR}/cmake/check_gpu_includes.cmake
)

# Arguments rexglue_configure_target refuses fail configure and name the cause
function (add_configure_target_args_test name arguments expected)
  add_test(NAME ${name}
           COMMAND ${CMAKE_COMMAND} -DREXGLUE_ROOT=${PROJECT_SOURCE_DIR} "-DARGS=${arguments}" -P
                   ${CMAKE_CURRENT_SOURCE_DIR}/configure_target_args.cmake
  )
  set_tests_properties(${name} PROPERTIES PASS_REGULAR_EXPRESSION "${expected}")
endfunction ()

add_configure_target_args_test(
  configure_target_rejects_gpu_plugins "GPU_PLUGINS$<SEMICOLON>xenos"
  "'GPU_PLUGINS'[ \n]+is[ \n]+not[ \n]+accepted"
)
add_configure_target_args_test(
  configure_target_rejects_no_copy_with_exclude "NO_COPY$<SEMICOLON>EXCLUDE$<SEMICOLON>rexapu"
  "cannot[ \n]+be[ \n]+combined[ \n]+with[ \n]+EXCLUDE"
)
add_configure_target_args_test(
  configure_target_rejects_unknown_exclude_name "EXCLUDE$<SEMICOLON>rexnothing"
  "'rexnothing'[ \n]+is[ \n]+not[ \n]+copied.*Valid[ \n]+names:[ \n]+rexfilesystem"
)
add_configure_target_args_test(
  configure_target_rejects_empty_exclude "EXCLUDE"
  "EXCLUDE[ \n]+needs[ \n]+at[ \n]+least[ \n]+one[ \n]+name"
)

# Compile and run checks for the entry point macros
add_subdirectory(samples)

add_subdirectory(headers)

add_subdirectory(unit)

# Needs the PPC toolchain
if (REXGLUE_BUILD_PPC_TESTS)
  add_subdirectory(ppc)
endif ()

# Builds every test executable and test module. The folder is excluded from the default target, so
# build this target before ctest. Object libraries and targets with REXGLUE_BUILT_BY_TEST are left
# out, since a test builds them itself.
function (_rexglue_collect_test_targets folder out_var)
  get_property(
    _targets
    DIRECTORY "${folder}"
    PROPERTY BUILDSYSTEM_TARGETS
  )
  foreach (_target IN LISTS _targets)
    get_target_property(_type ${_target} TYPE)
    get_target_property(_built_by_test ${_target} REXGLUE_BUILT_BY_TEST)
    if (_type MATCHES "^(EXECUTABLE|SHARED_LIBRARY|MODULE_LIBRARY)$" AND NOT _built_by_test)
      list(APPEND _found ${_target})
    endif ()
  endforeach ()
  get_property(
    _folders
    DIRECTORY "${folder}"
    PROPERTY SUBDIRECTORIES
  )
  foreach (_folder IN LISTS _folders)
    _rexglue_collect_test_targets("${_folder}" _nested)
    list(APPEND _found ${_nested})
  endforeach ()
  set(${out_var}
      ${_found}
      PARENT_SCOPE
  )
endfunction ()

_rexglue_collect_test_targets("${CMAKE_CURRENT_SOURCE_DIR}" _rexglue_test_targets)
add_custom_target(rexglue_tests COMMENT "Building the tests")
add_dependencies(rexglue_tests ${_rexglue_test_targets})

# Builds every test program, then runs ctest for the configuration that was built
add_custom_target(
  rextest_check_all
  COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR} -C $<CONFIG> --output-on-failure
  USES_TERMINAL VERBATIM
  COMMENT "Running every test"
)
add_dependencies(rextest_check_all rexglue_tests)

set(REXTEST_BENCHMARK_FILTER
    "[benchmark]"
    CACHE STRING "Catch2 test spec that the rextest_benchmark_all target runs"
)
set(REXTEST_BENCHMARK_SAMPLES
    100
    CACHE STRING "Samples that the rextest_benchmark_all target takes for each benchmark"
)

# Runs every benchmark with full sampling and writes the CSV file the contributing page describes
add_custom_target(
  rextest_benchmark_all
  COMMAND
    ${CMAKE_COMMAND} -DUNIT_TESTS=$<TARGET_FILE:unit_tests> -DCONFIG=$<CONFIG>
    -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/rextest -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
    "-DFILTER=${REXTEST_BENCHMARK_FILTER}" -DSAMPLES=${REXTEST_BENCHMARK_SAMPLES}
    -DSDK_VERSION=${REXGLUE_FULL_VERSION}
    "-DCOMPILER=${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}" -P
    ${PROJECT_SOURCE_DIR}/cmake/rextest_benchmark.cmake
  USES_TERMINAL VERBATIM
  COMMENT "Running every benchmark"
)
add_dependencies(rextest_benchmark_all unit_tests)
