add_library(rex-sdk INTERFACE)
target_link_libraries(rex-sdk INTERFACE rex-core rex-runtime rex-imgui)
set_target_properties(rex-sdk PROPERTIES EXPORT_NAME sdk)
add_library(rex::sdk ALIAS rex-sdk)
