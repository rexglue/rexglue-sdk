add_executable(filesystem_tests
    file_system_test.cpp
    host_file_test.cpp
    open_node_test.cpp
    overwrite_test.cpp
    providers_test.cpp
    rename_test.cpp
    wildcard_test.cpp
    ${PROJECT_SOURCE_DIR}/src/filesystem/wildcard.cpp
)

if(WIN32)
    target_sources(filesystem_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/filesystem/host_file_win.cpp)
elseif(UNIX)
    target_sources(filesystem_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/filesystem/host_file_posix.cpp)
endif()

target_compile_definitions(filesystem_tests PRIVATE
    REXGLUE_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures/filesystem"
)

target_include_directories(filesystem_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/tests
)

target_link_libraries(filesystem_tests PRIVATE
    rexcore
    Catch2::Catch2WithMain
)

add_dependencies(filesystem_tests rexfilesystem)

set_target_properties(filesystem_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexcore>
)
if(UNIX AND NOT APPLE)
    set_target_properties(filesystem_tests PROPERTIES
        INSTALL_RPATH "$ORIGIN"
        BUILD_WITH_INSTALL_RPATH ON
    )
endif()

include(Catch)
catch_discover_tests(filesystem_tests
    TEST_PREFIX "filesystem."
    PROPERTIES
        LABELS "filesystem"
        TIMEOUT 60
)
