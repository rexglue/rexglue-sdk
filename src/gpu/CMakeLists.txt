add_library(rexgpu SHARED graphics_module.cpp graphics_system.cpp present_thread.cpp)
set(REX_GPU_SCOPE PUBLIC)
add_library(rex::gpu ALIAS rexgpu)
rex_target_headers(rexgpu DIRS gpu)
rexglue_link_host_allocator(rexgpu)
set_target_properties(
  rexgpu
  PROPERTIES EXPORT_NAME gpu
             CXX_VISIBILITY_PRESET hidden
             VISIBILITY_INLINES_HIDDEN ON
)
target_compile_definitions(rexgpu PRIVATE REX_BUILDING_GPU)
rex_target_pch(rexgpu)
target_include_directories(
  rexgpu PRIVATE ${PROJECT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/thirdparty
)
rexglue_add_version_resource(rexgpu DESCRIPTION "ReXGlue graphics")

if (NOT WIN32 AND NOT APPLE)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(X11_XCB REQUIRED x11-xcb)
  pkg_check_modules(WAYLAND REQUIRED wayland-client)
  target_include_directories(
    rexgpu ${REX_GPU_SCOPE} ${X11_XCB_INCLUDE_DIRS} ${WAYLAND_INCLUDE_DIRS}
  )
  target_link_libraries(rexgpu ${REX_GPU_SCOPE} ${X11_XCB_LIBRARIES} ${WAYLAND_LIBRARIES})
endif ()

target_include_directories(
  rexgpu ${REX_GPU_SCOPE} $<BUILD_INTERFACE:${REXGLUE_ROOT}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(rexgpu ${REX_GPU_SCOPE} rexbase)
