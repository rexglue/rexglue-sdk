add_library(rexui STATIC null_user_interface.cpp)
add_library(rex::ui ALIAS rexui)
set_target_properties(rexui PROPERTIES EXPORT_NAME ui POSITION_INDEPENDENT_CODE ON)

target_include_directories(rexui PUBLIC
    $<BUILD_INTERFACE:${REXGLUE_ROOT}/include>
    $<BUILD_INTERFACE:$<TARGET_PROPERTY:reximgui,INTERFACE_INCLUDE_DIRECTORIES>>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_definitions(rexui PUBLIC "IMGUI_USER_CONFIG=\"rex/ui/imgui_config.h\"")

target_link_libraries(rexui PUBLIC rexcore rexgpu)
