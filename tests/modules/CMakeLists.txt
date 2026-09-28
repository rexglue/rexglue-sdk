# Test modules for the module loader tests. They link nothing, only SDK headers.

foreach(name good missing badversion failinit dupecho)
    set(target rexmod-test-${name})
    add_library(${target} SHARED ${name}.cpp)
    target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_compile_definitions(${target} PRIVATE REXGLUE_BUILD_CONFIG="$<CONFIG>")
    set_target_properties(${target} PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>
        LIBRARY_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>
    )
    rex_config_postfix(${target})
    if(NOT WIN32)
        set_target_properties(${target} PROPERTIES PREFIX "lib")
    endif()
endforeach()
