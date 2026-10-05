add_executable(rex-sample-console console_app.cpp)
target_link_libraries(rex-sample-console PRIVATE rex-runtime)
set_target_properties(rex-sample-console PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rex-runtime>)

add_executable(rex-sample-title WIN32 title_app.cpp)
target_link_libraries(rex-sample-title PRIVATE rex-runtime)
set_target_properties(rex-sample-title PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rex-runtime>)
