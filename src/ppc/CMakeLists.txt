add_library(
  rexppc SHARED
  detour_check.cpp
  dispatch.cpp
  embedded_metadata.cpp
  kernel_hook.cpp
  mmio.cpp
  ppc_detour_frame.cpp
  ppc_stack.cpp
  recompiled_code.cpp
  vec128.cpp
)
if (WIN32)
  target_sources(rexppc PRIVATE seh_win.cpp)
else ()
  target_sources(rexppc PRIVATE seh_posix.cpp)
endif ()
add_library(rex::ppc ALIAS rexppc)
rex_target_headers(rexppc DIRS ppc)
rexglue_link_host_allocator(rexppc)
set_target_properties(rexppc PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
target_compile_definitions(rexppc PRIVATE REX_BUILDING_PPC)
rex_target_pch(rexppc)
target_include_directories(
  rexppc PRIVATE ${PROJECT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/thirdparty
)
target_link_libraries(rexppc PUBLIC rexbase)
rexglue_add_version_resource(rexppc DESCRIPTION "ReXGlue PPC runtime")
