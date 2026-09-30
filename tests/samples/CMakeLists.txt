add_executable(rex-sample-console console_app.cpp)
target_link_libraries(rex-sample-console PRIVATE rexruntime)
set_target_properties(rex-sample-console PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>)

add_executable(rex-sample-title WIN32 title_app.cpp)
target_link_libraries(rex-sample-title PRIVATE rexruntime)
set_target_properties(rex-sample-title PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>)
