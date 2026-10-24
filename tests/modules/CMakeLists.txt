# Test modules for the module loader tests. Most link only the header only SDK targets, the apu
# and hid modules link rexcore and the cross modules link rexruntime.

foreach (name good missing badversion failinit duplicate-echo)
  set(_target rexmod-test-${name})
  add_library(${_target} SHARED ${name}.cpp)
  target_include_directories(
    ${_target} PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_BINARY_DIR}/include
  )
  target_compile_definitions(${_target} PRIVATE REXGLUE_BUILD_CONFIG="$<CONFIG>")
  target_link_libraries(${_target} PRIVATE simde)
  set_target_properties(
    ${_target}
    PROPERTIES CXX_VISIBILITY_PRESET hidden
               VISIBILITY_INLINES_HIDDEN ON
               PREFIX ""
               RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>
               LIBRARY_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>
  )
  rex_config_postfix(${_target})
  if (NOT WIN32)
    set_target_properties(${_target} PROPERTIES PREFIX "lib")
  endif ()
endforeach ()

target_link_libraries(rexmod-test-good PRIVATE rexcore)

add_library(rexmod-test-null-gpu SHARED null_gpu/null_gpu_module.cpp)
target_include_directories(
  rexmod-test-null-gpu PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_BINARY_DIR}/include
)
target_compile_definitions(rexmod-test-null-gpu PRIVATE REXGLUE_BUILD_CONFIG="$<CONFIG>")
target_link_libraries(rexmod-test-null-gpu PRIVATE simde)
set_target_properties(
  rexmod-test-null-gpu
  PROPERTIES CXX_VISIBILITY_PRESET hidden
             VISIBILITY_INLINES_HIDDEN ON
             PREFIX ""
             RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>
             LIBRARY_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>
)
rex_config_postfix(rexmod-test-null-gpu)
if (NOT WIN32)
  set_target_properties(rexmod-test-null-gpu PROPERTIES PREFIX "lib")
endif ()

foreach (name apu hid)
  set(_target rexmod-test-${name})
  add_library(${_target} SHARED ${name}/${name}_module.cpp)
  target_include_directories(
    ${_target} PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_BINARY_DIR}/include
  )
  target_compile_definitions(${_target} PRIVATE REXGLUE_BUILD_CONFIG="$<CONFIG>")
  target_link_libraries(${_target} PRIVATE rexcore)
  set_target_properties(
    ${_target}
    PROPERTIES CXX_VISIBILITY_PRESET hidden
               VISIBILITY_INLINES_HIDDEN ON
               PREFIX ""
               RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>
               LIBRARY_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>
  )
  rex_config_postfix(${_target})
  if (NOT WIN32)
    set_target_properties(${_target} PROPERTIES PREFIX "lib")
  endif ()
endforeach ()

foreach (variant a b)
  set(_target rexmod-test-recompiled-${variant})
  add_library(${_target} SHARED recompiled.cpp)
  target_include_directories(
    ${_target} PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_BINARY_DIR}/include
  )
  target_compile_definitions(
    ${_target} PRIVATE REXGLUE_BUILD_CONFIG="$<CONFIG>" REX_TEST_IMAGE_BASE=0x8E000000
                       $<IF:$<STREQUAL:${variant},a>,REX_TEST_RESULT=0xA1,REX_TEST_RESULT=0xB2>
  )
  target_link_libraries(${_target} PRIVATE simde)
  set_target_properties(
    ${_target}
    PROPERTIES CXX_VISIBILITY_PRESET hidden
               VISIBILITY_INLINES_HIDDEN ON
               PREFIX ""
               RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>
               LIBRARY_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>
  )
  rex_config_postfix(${_target})
  if (NOT WIN32)
    set_target_properties(${_target} PROPERTIES PREFIX "lib")
  endif ()
endforeach ()

foreach (role exporter importer)
  set(_target rexmod-test-cross-${role})
  add_library(${_target} SHARED crossmodule.cpp)
  target_include_directories(
    ${_target} PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_BINARY_DIR}/include
  )
  set(_exporter "$<STREQUAL:${role},exporter>")
  target_compile_definitions(
    ${_target}
    PRIVATE REXGLUE_BUILD_CONFIG="$<CONFIG>"
            $<IF:${_exporter},REX_TEST_EXPORTER=1,REX_TEST_IMPORTER=1>
            $<IF:${_exporter},REX_TEST_IMAGE_BASE=0x8E000000,REX_TEST_IMAGE_BASE=0x8C000000>
  )
  target_link_libraries(${_target} PRIVATE simde rexruntime)
  set_target_properties(
    ${_target}
    PROPERTIES CXX_VISIBILITY_PRESET hidden
               VISIBILITY_INLINES_HIDDEN ON
               PREFIX ""
               RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>
               LIBRARY_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>
  )
  rex_config_postfix(${_target})
  if (NOT WIN32)
    set_target_properties(${_target} PROPERTIES PREFIX "lib")
  endif ()
endforeach ()

foreach (variant noidentity otheridentity)
  set(_target rexmod-test-${variant})
  add_library(${_target} SHARED foreign.cpp)
  target_include_directories(
    ${_target} PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_BINARY_DIR}/include
  )
  target_compile_definitions(${_target} PRIVATE REXGLUE_BUILD_CONFIG="$<CONFIG>")
  if (variant STREQUAL "otheridentity")
    target_compile_definitions(${_target} PRIVATE REX_TEST_FOREIGN_IDENTITY="0.0.0-000000000000")
  endif ()
  target_link_libraries(${_target} PRIVATE simde)
  set_target_properties(
    ${_target}
    PROPERTIES CXX_VISIBILITY_PRESET hidden
               VISIBILITY_INLINES_HIDDEN ON
               PREFIX ""
               RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>
               LIBRARY_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>
  )
  rex_config_postfix(${_target})
  if (NOT WIN32)
    set_target_properties(${_target} PROPERTIES PREFIX "lib")
  endif ()
endforeach ()
