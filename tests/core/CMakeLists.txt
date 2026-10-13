add_executable(core_tests assert_test.cpp core_test.cpp container_test.cpp high_resolution_timer_test.cpp logger_test.cpp math_test.cpp propagate_const_test.cpp sleep_test.cpp socket_test.cpp socket_wait_test.cpp thread_callback_test.cpp thread_start_data_test.cpp thread_time_test.cpp timer_queue_test.cpp utf8_test.cpp xex_test.cpp
    ${PROJECT_SOURCE_DIR}/thirdparty/crypto/rijndael-alg-fst.c)

set_source_files_properties(${PROJECT_SOURCE_DIR}/thirdparty/crypto/rijndael-alg-fst.c
    PROPERTIES LANGUAGE CXX COMPILE_OPTIONS -w)

target_compile_definitions(core_tests PRIVATE
    REXGLUE_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures/filesystem"
)

target_link_libraries(core_tests PRIVATE
    rex-core
    Catch2::Catch2WithMain
)
if(WIN32)
    target_link_libraries(core_tests PRIVATE ws2_32)
endif()

target_include_directories(core_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests ${PROJECT_SOURCE_DIR}/thirdparty)

set_target_properties(core_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rex-core>
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
