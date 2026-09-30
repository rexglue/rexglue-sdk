add_library(rexsdk INTERFACE)
target_link_libraries(rexsdk INTERFACE rexcore rexruntime)
set_target_properties(rexsdk PROPERTIES EXPORT_NAME sdk)
add_library(rex::sdk ALIAS rexsdk)
