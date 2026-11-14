set(REXXBDM_SOURCES xbdm_bodies.cpp xbdm_module.cpp xbdm_stub.cpp)

list(TRANSFORM REXXBDM_SOURCES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")

add_library(rexxbdm-objects OBJECT ${REXXBDM_SOURCES})
add_library(rexxbdm SHARED module_entry.cpp $<TARGET_OBJECTS:rexxbdm-objects>)
add_library(rex::xbdm ALIAS rexxbdm)
rexglue_link_host_allocator(rexxbdm)
set_target_properties(rexxbdm PROPERTIES EXPORT_NAME xbdm)

foreach (xbdm_target rexxbdm-objects rexxbdm)
  set_target_properties(
    ${xbdm_target}
    PROPERTIES CXX_VISIBILITY_PRESET hidden
               VISIBILITY_INLINES_HIDDEN ON
               POSITION_INDEPENDENT_CODE ON
  )
  target_compile_definitions(${xbdm_target} PRIVATE REX_BUILDING_XBDM)
  target_include_directories(
    ${xbdm_target}
    PUBLIC $<BUILD_INTERFACE:${REXGLUE_ROOT}/include>
           $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>
           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  )
  target_include_directories(${xbdm_target} PRIVATE ${REXGLUE_ROOT}/src ${REXGLUE_ROOT}/thirdparty)
  target_link_libraries(${xbdm_target} PUBLIC rexbase rexppc simde)
endforeach ()
if (APPLE)
  target_link_options(rexxbdm PRIVATE -Wl,-undefined,error)
elseif (UNIX)
  target_link_options(rexxbdm PRIVATE -Wl,--no-undefined)
endif ()
rex_target_pch(rexxbdm-objects <rex/ppc/hook.h>)
rexglue_add_version_resource(rexxbdm DESCRIPTION "ReXGlue XBDM")
