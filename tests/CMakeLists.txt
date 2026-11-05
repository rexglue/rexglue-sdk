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
