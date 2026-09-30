add_executable(core_tests core_test.cpp)

target_link_libraries(core_tests PRIVATE
    rexcore
    Catch2::Catch2WithMain
)

set_target_properties(core_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexcore>
)
if(UNIX AND NOT APPLE)
    set_target_properties(core_tests PROPERTIES
        INSTALL_RPATH "$ORIGIN"
        BUILD_WITH_INSTALL_RPATH ON
    )
endif()

include(Catch)
catch_discover_tests(core_tests
    TEST_PREFIX "core."
    PROPERTIES
        LABELS "core"
        TIMEOUT 60
)
