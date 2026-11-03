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

# The removed staging arguments fail configure and name the argument
add_test(NAME configure_target_rejects_gpu_plugins
         COMMAND ${CMAKE_COMMAND} -DREXGLUE_ROOT=${PROJECT_SOURCE_DIR} -P
                 ${CMAKE_CURRENT_SOURCE_DIR}/configure_target_args.cmake
)
set_tests_properties(
  configure_target_rejects_gpu_plugins PROPERTIES PASS_REGULAR_EXPRESSION
                                                  "'GPU_PLUGINS' is no longer accepted"
)

# Compile and run checks for the entry point macros
add_subdirectory(samples)

add_subdirectory(headers)

add_subdirectory(unit)

# Needs the PPC toolchain
if (REXGLUE_BUILD_PPC_TESTS)
  add_subdirectory(ppc)
endif ()
