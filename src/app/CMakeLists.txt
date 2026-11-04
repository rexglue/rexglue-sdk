add_library(rexapp-objects OBJECT app.cpp command_line.cpp lifecycle.cpp title_app.cpp)
add_library(rexapp SHARED $<TARGET_OBJECTS:rexapp-objects>)
add_library(rex::app ALIAS rexapp)
rex_target_headers(rexapp DIRS app)
rexglue_link_host_allocator(rexapp)
set_target_properties(rexapp PROPERTIES EXPORT_NAME app)
foreach (app_target rexapp-objects rexapp)
  set_target_properties(
    ${app_target}
    PROPERTIES CXX_VISIBILITY_PRESET hidden
               VISIBILITY_INLINES_HIDDEN ON
               POSITION_INDEPENDENT_CODE ON
  )
  target_compile_definitions(${app_target} PRIVATE REX_BUILDING_APP)
  target_include_directories(${app_target} PRIVATE ${REXGLUE_ROOT}/src ${REXGLUE_ROOT}/thirdparty)
endforeach ()
rex_target_pch(rexapp-objects)
target_link_libraries(rexapp-objects PUBLIC rexbase rexkernel-objects)
target_link_libraries(rexapp PUBLIC rexbase rexkernel)
rexglue_add_version_resource(rexapp DESCRIPTION "ReXGlue app")
