if(WIN32)
    add_library(rexgpu STATIC surface_win.cpp)
    set_target_properties(rexgpu PROPERTIES POSITION_INDEPENDENT_CODE ON)
    set(REX_GPU_SCOPE PUBLIC)
else()
    add_library(rexgpu INTERFACE)
    set(REX_GPU_SCOPE INTERFACE)
endif()
add_library(rex::gpu ALIAS rexgpu)
set_target_properties(rexgpu PROPERTIES EXPORT_NAME gpu)

if(NOT WIN32 AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(X11_XCB REQUIRED x11-xcb)
    pkg_check_modules(WAYLAND REQUIRED wayland-client)
    target_include_directories(rexgpu ${REX_GPU_SCOPE} ${X11_XCB_INCLUDE_DIRS} ${WAYLAND_INCLUDE_DIRS})
    target_link_libraries(rexgpu ${REX_GPU_SCOPE} ${X11_XCB_LIBRARIES} ${WAYLAND_LIBRARIES})
endif()

target_include_directories(rexgpu ${REX_GPU_SCOPE}
    $<BUILD_INTERFACE:${REXGLUE_ROOT}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(rexgpu ${REX_GPU_SCOPE} rexcore)
