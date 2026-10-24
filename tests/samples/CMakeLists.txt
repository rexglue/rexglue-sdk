add_executable(rex-sample-console console_app.cpp)
rexglue_link_host_allocator(rex-sample-console)
target_link_libraries(rex-sample-console PRIVATE rexruntime)
set_target_properties(
  rex-sample-console PROPERTIES RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>
)

add_executable(rex-sample-title WIN32 title_app.cpp)
rexglue_link_host_allocator(rex-sample-title)
target_link_libraries(rex-sample-title PRIVATE rexruntime)
set_target_properties(
  rex-sample-title PROPERTIES RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>
)

add_executable(rex-sample-app-main app_main.cpp)
rexglue_link_host_allocator(rex-sample-app-main)
target_link_libraries(rex-sample-app-main PRIVATE rexruntime)
set_target_properties(
  rex-sample-app-main PROPERTIES RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>
)
