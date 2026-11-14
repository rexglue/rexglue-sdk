set(CMAKE_FOLDER "tests/modules")

# Shared library prefix is none on Windows and lib elsewhere.
if (WIN32)
  set(REX_TEST_MODULE_PREFIX "")
else ()
  set(REX_TEST_MODULE_PREFIX "lib")
endif ()

# FOREIGN modules link neither the host allocator nor the SDK, only its include directories.
function (rex_add_test_module name)
  cmake_parse_arguments(ARG "FOREIGN" "" "SOURCES;LIBS;DEFS" ${ARGN})
  add_library(${name} SHARED ${ARG_SOURCES})
  target_compile_definitions(${name} PRIVATE REXGLUE_BUILD_CONFIG="$<CONFIG>" ${ARG_DEFS})
  target_link_libraries(${name} PRIVATE ${ARG_LIBS})
  if (ARG_FOREIGN)
    target_include_directories(
      ${name} PRIVATE $<TARGET_PROPERTY:rexbase,INTERFACE_INCLUDE_DIRECTORIES>
    )
  else ()
    rexglue_link_host_allocator(${name})
  endif ()
  set_target_properties(
    ${name}
    PROPERTIES CXX_VISIBILITY_PRESET hidden
               VISIBILITY_INLINES_HIDDEN ON
               PREFIX "${REX_TEST_MODULE_PREFIX}"
               RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexkernel>
               LIBRARY_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexkernel>
  )
  rex_config_postfix(${name})
endfunction ()

foreach (variant a b)
  rex_add_test_module(
    rexmod-test-recompiled-${variant}
    SOURCES
    recompiled.cpp
    LIBS
    simde
    rexbase
    DEFS
    REX_TEST_IMAGE_BASE=0x8E000000
    $<IF:$<STREQUAL:${variant},a>,REX_TEST_RESULT=0xA1,REX_TEST_RESULT=0xB2>
  )
endforeach ()

foreach (role exporter importer)
  set(_exporter "$<STREQUAL:${role},exporter>")
  rex_add_test_module(
    rexmod-test-cross-${role}
    SOURCES
    crossmodule.cpp
    LIBS
    simde
    rexkernel
    DEFS
    $<IF:${_exporter},REX_TEST_EXPORTER=1,REX_TEST_IMPORTER=1>
    $<IF:${_exporter},REX_TEST_IMAGE_BASE=0x8E000000,REX_TEST_IMAGE_BASE=0x8C000000>
  )
endforeach ()

rex_add_test_module(rexmod-test-nomodule FOREIGN SOURCES foreign.cpp)

rex_add_test_module(rexmod-test-alloc SOURCES alloc/alloc_module.cpp LIBS rexbase)

rex_add_test_module(rexmodule-test-good SOURCES good_module.cpp LIBS rexbase)
rex_add_test_module(rexmodule-test-dyninit SOURCES dyninit_module.cpp LIBS rexbase)

# One source in four variants that differ in what they report. otherbuild reports another SDK build,
# unknownbuild reports none, needsnewer requires a version nobody serves and othermodule serves
# another version of IModule.
set(_rex_variant_sources versioned_module.cpp)
rex_add_test_module(
  rexmodule-test-otherbuild
  SOURCES
  ${_rex_variant_sources}
  LIBS
  rexbase
  DEFS
  REX_TEST_BUILD="0.0.0-other"
)
rex_add_test_module(
  rexmodule-test-unknownbuild
  SOURCES
  ${_rex_variant_sources}
  LIBS
  rexbase
  DEFS
  REX_TEST_NO_BUILD=1
)
rex_add_test_module(
  rexmodule-test-needsnewer
  SOURCES
  ${_rex_variant_sources}
  LIBS
  rexbase
  DEFS
  REX_TEST_REQUIRES_VERSION=2
)
rex_add_test_module(
  rexmodule-test-othermodule
  SOURCES
  ${_rex_variant_sources}
  LIBS
  rexbase
  DEFS
  REX_TEST_MODULE_VERSION=1
)

rexglue_audit_provider_init(rexmodule-test-good)
rexglue_audit_provider_init(rexmodule-test-dyninit EXPECT_FAIL)
