# Test modules for the module loader tests. They link only the header only SDK targets.

foreach(name good missing badversion failinit dupecho)
    set(target rexmod-test-${name})
    add_library(${target} SHARED ${name}.cpp)
    target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_compile_definitions(${target} PRIVATE REXGLUE_BUILD_CONFIG="$<CONFIG>")
    target_link_libraries(${target} PRIVATE simde)
    set_target_properties(${target} PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rex-runtime>
        LIBRARY_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rex-runtime>
    )
    rex_config_postfix(${target})
    if(NOT WIN32)
        set_target_properties(${target} PROPERTIES PREFIX "lib")
    endif()
endforeach()

add_library(rexmod-test-null-gpu SHARED null_gpu/null_gpu_module.cpp)
target_include_directories(rexmod-test-null-gpu PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_definitions(rexmod-test-null-gpu PRIVATE REXGLUE_BUILD_CONFIG="$<CONFIG>")
target_link_libraries(rexmod-test-null-gpu PRIVATE simde)
set_target_properties(rexmod-test-null-gpu PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rex-runtime>
    LIBRARY_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rex-runtime>
)
rex_config_postfix(rexmod-test-null-gpu)
if(NOT WIN32)
    set_target_properties(rexmod-test-null-gpu PROPERTIES PREFIX "lib")
endif()

foreach(name apu hid)
    set(target rexmod-test-${name})
    add_library(${target} SHARED ${name}/${name}_module.cpp)
    target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_compile_definitions(${target} PRIVATE REXGLUE_BUILD_CONFIG="$<CONFIG>")
    target_link_libraries(${target} PRIVATE rex-core)
    set_target_properties(${target} PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rex-runtime>
        LIBRARY_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rex-runtime>
    )
    rex_config_postfix(${target})
    if(NOT WIN32)
        set_target_properties(${target} PROPERTIES PREFIX "lib")
    endif()
endforeach()

foreach(variant a b)
    set(target rexmod-test-recompiled-${variant})
    add_library(${target} SHARED recompiled.cpp)
    target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_compile_definitions(${target} PRIVATE
        REXGLUE_BUILD_CONFIG="$<CONFIG>"
        REX_TEST_IMAGE_BASE=0x8E000000
        $<IF:$<STREQUAL:${variant},a>,REX_TEST_RESULT=0xA1,REX_TEST_RESULT=0xB2>)
    target_link_libraries(${target} PRIVATE simde)
    set_target_properties(${target} PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rex-runtime>
        LIBRARY_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rex-runtime>
    )
    rex_config_postfix(${target})
    if(NOT WIN32)
        set_target_properties(${target} PROPERTIES PREFIX "lib")
    endif()
endforeach()
