add_library(rexfilesystem OBJECT
    device.cpp
    entry.cpp
    virtual_file_system.cpp
    devices/disc_image_device.cpp
    devices/disc_image_entry.cpp
    devices/disc_image_file.cpp
    devices/host_path_device.cpp
    devices/host_path_entry.cpp
    devices/host_path_file.cpp
    devices/null_device.cpp
    devices/null_entry.cpp
    devices/null_file.cpp
    devices/stfs_container_device.cpp
    devices/stfs_container_entry.cpp
    devices/stfs_container_file.cpp
)
add_library(rex::filesystem ALIAS rexfilesystem)

rex_target_pch(rexfilesystem
    <mutex>
    <rex/math.h>
    <rex/string.h>
    <rex/memory.h>
    <rex/filesystem.h>
    <rex/thread/mutex.h>
    <rex/system/xtypes.h>
    <rex/system/xmemory.h>
)

target_link_libraries(rexfilesystem PUBLIC rexcore)
target_include_directories(rexfilesystem PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_include_directories(rexfilesystem PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/devices
    ${PROJECT_SOURCE_DIR}/src
)
