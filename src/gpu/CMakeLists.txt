if(WIN32)
    add_library(rex-gpu STATIC surface_win.cpp)
    set_target_properties(rex-gpu PROPERTIES POSITION_INDEPENDENT_CODE ON)
    set(REX_GPU_SCOPE PUBLIC)
else()
    add_library(rex-gpu INTERFACE)
    set(REX_GPU_SCOPE INTERFACE)
endif()
add_library(rex::gpu ALIAS rex-gpu)
set_target_properties(rex-gpu PROPERTIES EXPORT_NAME gpu)

if(NOT WIN32 AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(X11_XCB REQUIRED x11-xcb)
    pkg_check_modules(WAYLAND REQUIRED wayland-client)
    target_include_directories(rex-gpu ${REX_GPU_SCOPE} ${X11_XCB_INCLUDE_DIRS} ${WAYLAND_INCLUDE_DIRS})
    target_link_libraries(rex-gpu ${REX_GPU_SCOPE} ${X11_XCB_LIBRARIES} ${WAYLAND_LIBRARIES})
endif()

target_include_directories(rex-gpu ${REX_GPU_SCOPE}
    $<BUILD_INTERFACE:${REXGLUE_ROOT}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(rex-gpu ${REX_GPU_SCOPE} rex-core)
