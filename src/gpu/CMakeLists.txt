add_library(rex-gpu STATIC)
add_library(rex::gpu ALIAS rex-gpu)
set_target_properties(rex-gpu PROPERTIES EXPORT_NAME gpu POSITION_INDEPENDENT_CODE ON)

if(WIN32)
    target_sources(rex-gpu PRIVATE surface_win.cpp)
elseif(APPLE)
    target_sources(rex-gpu PRIVATE surface_mac.cpp)
else()
    target_sources(rex-gpu PRIVATE surface_gnulinux.cpp)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(X11_XCB REQUIRED x11-xcb)
    pkg_check_modules(WAYLAND REQUIRED wayland-client)
    target_include_directories(rex-gpu PUBLIC ${X11_XCB_INCLUDE_DIRS} ${WAYLAND_INCLUDE_DIRS})
    target_link_libraries(rex-gpu PUBLIC ${X11_XCB_LIBRARIES} ${WAYLAND_LIBRARIES})
endif()

target_include_directories(rex-gpu PUBLIC
    $<BUILD_INTERFACE:${REXGLUE_ROOT}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(rex-gpu PUBLIC rex-core)
target_include_directories(rex-gpu PRIVATE $<TARGET_PROPERTY:SDL3::Headers,INTERFACE_INCLUDE_DIRECTORIES>)
