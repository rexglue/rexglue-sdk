# rex-ui

# Core UI sources
set(REXUI_CORE_SOURCES
    console/model.cpp
    console/view.cpp
    console/window.cpp
    graphics_upload_buffer_pool.cpp
    graphics_util.cpp
    image_decode.cpp
    imgui_dialog.cpp
    imgui_drawer.cpp
    immediate_drawer.cpp
    keybinds.cpp
    menu_item.cpp
    overlay.cpp
    presenter.cpp
    renderdoc_api.cpp
    sdl_virtual_key.cpp
    title_app.cpp
    video_mode_util.cpp
    window.cpp
    window_sdl.cpp
    windowed_app_context.cpp
    windowed_app_context_sdl.cpp
)

# Optional overlay dialogs
set(REXUI_OVERLAY_SOURCES
    overlay/achievement_icon_cache.cpp
    overlay/achievement_toast.cpp
    overlay/achievements_overlay.cpp
    overlay/debug_overlay.cpp
    overlay/console_overlay.cpp
    overlay/console_commands.cpp
    overlay/settings_overlay.cpp
)

# Vulkan UI backend
set(REXUI_VULKAN_SOURCES
    vulkan/linked_type_descriptor_set_allocator.cpp
    vulkan/vulkan_moltenvk.cpp
    vulkan/single_layout_descriptor_set_pool.cpp
    vulkan/spirv_tools_context.cpp
    vulkan/ui_samplers.cpp
    vulkan/vulkan_device.cpp
    vulkan/vulkan_immediate_drawer.cpp
    vulkan/vulkan_instance.cpp
    vulkan/vulkan_mem_alloc.cpp
    vulkan/vulkan_presenter.cpp
    vulkan/vulkan_provider.cpp
    vulkan/vulkan_submission_tracker.cpp
    vulkan/vulkan_upload_buffer_pool.cpp
    vulkan/vulkan_util.cpp
)

# D3D12 UI backend
if(REXGLUE_GPU_D3D12)
    set(REXUI_D3D12_SOURCES
        d3d12/d3d12_cpu_descriptor_pool.cpp
        d3d12/d3d12_descriptor_heap_pool.cpp
        d3d12/d3d12_immediate_drawer.cpp
        d3d12/d3d12_presenter.cpp
        d3d12/d3d12_provider.cpp
        d3d12/d3d12_submission_tracker.cpp
        d3d12/d3d12_upload_buffer_pool.cpp
        d3d12/d3d12_util.cpp
    )
endif()

# Platform-specific sources (windowing itself is SDL3, platform-independent)
if(WIN32)
    set(REXUI_PLATFORM_SOURCES
        surface_win.cpp
    )
elseif(APPLE)
    set(REXUI_PLATFORM_SOURCES
        surface_mac.cpp
    )
else()
    set(REXUI_PLATFORM_SOURCES
        surface_gnulinux.cpp
    )
endif()

# Combine sources
set(REXUI_SOURCES
    ${REXUI_CORE_SOURCES}
    ${REXUI_OVERLAY_SOURCES}
    ${REXUI_PLATFORM_SOURCES}
)

list(APPEND REXUI_SOURCES ${REXUI_VULKAN_SOURCES})

if(REXGLUE_GPU_D3D12)
    list(APPEND REXUI_SOURCES ${REXUI_D3D12_SOURCES})
endif()

add_library(rex-ui OBJECT ${REXUI_SOURCES})
add_library(rex::ui ALIAS rex-ui)

rex_target_pch(rex-ui <rex/math.h>)

target_include_directories(rex-ui PUBLIC
    $<BUILD_INTERFACE:${REXGLUE_ROOT}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_include_directories(rex-ui PRIVATE
    ${PROJECT_BINARY_DIR}/include  # Generated macOS Vulkan path data
    ${REXGLUE_ROOT}/src  # For ui/shaders/bytecode includes
    ${REXGLUE_ROOT}/thirdparty/stb  # stb_image.h for icon decoding
)

target_link_libraries(rex-ui PUBLIC
    rex-core
    renderdoc
    imgui::imgui
    SDL3::SDL3
)
target_link_libraries(rex-ui PRIVATE utf8cpp)

target_link_libraries(rex-ui PUBLIC
    Vulkan::Headers
    GPUOpen::VulkanMemoryAllocator
    spirv-tools-headers
)

# Platform-specific dependencies
if(UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(X11_XCB REQUIRED x11-xcb)
    # Required, not optional: vulkan/api.h pulls in <wayland-client.h> on every
    # Linux build.
    pkg_check_modules(WAYLAND REQUIRED wayland-client)

    target_include_directories(rex-ui PUBLIC
        ${X11_XCB_INCLUDE_DIRS}
        ${WAYLAND_INCLUDE_DIRS}
    )
    target_link_libraries(rex-ui PUBLIC
        ${X11_XCB_LIBRARIES}
        ${WAYLAND_LIBRARIES}
    )
elseif(APPLE)
    target_link_libraries(rex-ui PRIVATE "-framework CoreFoundation")
endif()

if(WIN32)
    target_link_libraries(rex-ui PUBLIC dxgi dxguid)
endif()

# D3D12 backend dependencies
if(REXGLUE_GPU_D3D12)
    target_link_libraries(rex-ui PUBLIC dxc-headers)
endif()

