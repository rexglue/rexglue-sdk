add_library(rex-ui STATIC null_user_interface.cpp)
add_library(rex::ui ALIAS rex-ui)
set_target_properties(rex-ui PROPERTIES EXPORT_NAME ui POSITION_INDEPENDENT_CODE ON)

target_include_directories(rex-ui PUBLIC
    $<BUILD_INTERFACE:${REXGLUE_ROOT}/include>
    $<BUILD_INTERFACE:$<TARGET_PROPERTY:rex-imgui,INTERFACE_INCLUDE_DIRECTORIES>>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_definitions(rex-ui PUBLIC "IMGUI_USER_CONFIG=\"rex/ui/imgui_config.h\"")

target_link_libraries(rex-ui PUBLIC rex-core rex-gpu)
