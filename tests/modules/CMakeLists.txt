# Test modules for the module loader tests. Most link only the header only SDK targets, the apu
# and hid modules link rexcore and the cross modules link rexruntime.

# Prefix of a shared library target: none on Windows, lib elsewhere.
if (WIN32)
  set(REX_TEST_MODULE_PREFIX "")
else ()
  set(REX_TEST_MODULE_PREFIX "lib")
endif ()

# Adds a test module next to rexruntime. FOREIGN modules do not link the host allocator or the SDK,
# so they only get its include directories.
function (rex_add_test_module name)
  cmake_parse_arguments(ARG "FOREIGN" "" "SOURCES;LIBS;DEFS" ${ARGN})
  add_library(${name} SHARED ${ARG_SOURCES})
  target_compile_definitions(${name} PRIVATE REXGLUE_BUILD_CONFIG="$<CONFIG>" ${ARG_DEFS})
  target_link_libraries(${name} PRIVATE ${ARG_LIBS})
  if (ARG_FOREIGN)
    target_include_directories(
      ${name} PRIVATE $<TARGET_PROPERTY:rexcore,INTERFACE_INCLUDE_DIRECTORIES>
    )
  else ()
    rexglue_link_host_allocator(${name})
  endif ()
  set_target_properties(
    ${name}
    PROPERTIES CXX_VISIBILITY_PRESET hidden
               VISIBILITY_INLINES_HIDDEN ON
               PREFIX "${REX_TEST_MODULE_PREFIX}"
               RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>
               LIBRARY_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>
  )
  rex_config_postfix(${name})
endfunction ()

foreach (name good missing badversion failinit duplicate-echo)
  rex_add_test_module(rexmod-test-${name} SOURCES ${name}.cpp LIBS simde rexcore)
endforeach ()

rex_add_test_module(rexmod-test-null-gpu SOURCES null_gpu/null_gpu_module.cpp LIBS simde rexcore)

foreach (name apu hid)
  rex_add_test_module(rexmod-test-${name} SOURCES ${name}/${name}_module.cpp LIBS rexcore)
endforeach ()

foreach (variant a b)
  rex_add_test_module(
    rexmod-test-recompiled-${variant}
    SOURCES
    recompiled.cpp
    LIBS
    simde
    rexcore
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
    rexruntime
    DEFS
    $<IF:${_exporter},REX_TEST_EXPORTER=1,REX_TEST_IMPORTER=1>
    $<IF:${_exporter},REX_TEST_IMAGE_BASE=0x8E000000,REX_TEST_IMAGE_BASE=0x8C000000>
  )
endforeach ()

foreach (variant noidentity otheridentity)
  set(_defs "")
  if (variant STREQUAL "otheridentity")
    set(_defs REX_TEST_FOREIGN_IDENTITY="0.0.0-000000000000")
  endif ()
  rex_add_test_module(
    rexmod-test-${variant}
    FOREIGN
    SOURCES
    foreign.cpp
    LIBS
    simde
    DEFS
    ${_defs}
  )
endforeach ()

rex_add_test_module(rexmod-test-alloc SOURCES alloc/alloc_module.cpp LIBS rexcore)
