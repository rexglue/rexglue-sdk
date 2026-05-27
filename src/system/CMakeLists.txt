set(REXSYSTEM_SOURCES
    runtime.cpp
    kernel_state.cpp
    xmemory.cpp
    kernel_module.cpp
    user_module.cpp
    xthread.cpp
    xevent.cpp
    xfile.cpp
    xmutant.cpp
    xsemaphore.cpp
    xtimer.cpp
    xiocompletion.cpp
    xnotifylistener.cpp
    xenumerator.cpp
    xsocket.cpp
    xsymboliclink.cpp
    xobject.cpp
    xmodule.cpp
    guest_path.cpp

    # Utilities
    util/object_table.cpp
    util/native_list.cpp
    util/xdbf_utils.cpp
    util/gameinfo_utils.cpp

    # XAM services
    xam/app_manager.cpp
    xam/content_manager.cpp
    xam/user_profile.cpp

    ppc_types.cpp
    elf_module.cpp
    entry_table.cpp
    export_resolver.cpp
    lzx.cpp
    mmio_handler.cpp
    map_parser.cpp
    module.cpp
    function_dispatcher.cpp
    thread.cpp
    thread_state.cpp
    xex_module.cpp
)

# Compile system sources once. Kernel sources are appended in src/kernel/CMakeLists.txt.
add_library(rexruntime_objects OBJECT ${REXSYSTEM_SOURCES})
target_include_directories(rexruntime_objects PRIVATE
    $<BUILD_INTERFACE:${REXGLUE_ROOT}/include>
    ${REXGLUE_ROOT}/thirdparty
)
target_link_libraries(rexruntime_objects PRIVATE
    simde
    fmt::fmt
    spdlog::spdlog
    tomlplusplus
    SDL3::SDL3
    aes128
    mspack
)

if(REXGLUE_ENABLE_TRACY)
    target_link_libraries(rexruntime_objects PRIVATE TracyClient)
    target_compile_definitions(rexruntime_objects PRIVATE
        $<$<NOT:$<CONFIG:Release>>:REXGLUE_ENABLE_PROFILING>)
endif()

if(REXGLUE_ENABLE_PERF_COUNTERS)
    target_compile_definitions(rexruntime_objects PRIVATE
        $<$<NOT:$<CONFIG:Release>>:REXGLUE_ENABLE_PERF_COUNTERS>)
endif()

if(WIN32)
    target_link_libraries(rexruntime_objects PRIVATE ws2_32)
    target_compile_definitions(rexruntime_objects PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

if(REXGLUE_USE_D3D12)
    target_link_libraries(rexruntime_objects PRIVATE dxgi dxguid dxc-headers)
    target_compile_definitions(rexruntime_objects PRIVATE REX_HAS_D3D12=1)
endif()

if(REXGLUE_USE_VULKAN)
    target_link_libraries(rexruntime_objects PRIVATE
        volk::volk
        GPUOpen::VulkanMemoryAllocator
        SPIRV-Tools-static
    )
    target_compile_definitions(rexruntime_objects PRIVATE REX_HAS_VULKAN=1)
endif()

# Shared variant: the public face for host applications
add_library(rexruntime SHARED)
target_link_libraries(rexruntime PRIVATE rexruntime_objects)
add_library(rex::runtime ALIAS rexruntime)
add_library(rex::system  ALIAS rexruntime)
add_library(rex::kernel  ALIAS rexruntime)

set_target_properties(rexruntime PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS TRUE)

target_include_directories(rexruntime PUBLIC
    $<BUILD_INTERFACE:${REXGLUE_ROOT}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_include_directories(rexruntime PRIVATE
    ${REXGLUE_ROOT}/thirdparty
)

target_link_libraries(rexruntime
    PUBLIC
        simde
        fmt::fmt
        spdlog::spdlog
        tomlplusplus
        SDL3::SDL3
    PRIVATE
        aes128
        mspack
)

if(REXGLUE_ENABLE_TRACY)
    target_link_libraries(rexruntime PUBLIC TracyClient)
    target_compile_definitions(rexruntime PUBLIC
        $<$<NOT:$<CONFIG:Release>>:REXGLUE_ENABLE_PROFILING>)
endif()

if(REXGLUE_ENABLE_PERF_COUNTERS)
    target_compile_definitions(rexruntime PUBLIC
        $<$<NOT:$<CONFIG:Release>>:REXGLUE_ENABLE_PERF_COUNTERS>)
endif()

if(WIN32)
    target_link_libraries(rexruntime PRIVATE ws2_32)
    target_compile_definitions(rexruntime PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

if(REXGLUE_USE_D3D12)
    target_link_libraries(rexruntime PUBLIC dxgi dxguid dxc-headers)
    target_compile_definitions(rexruntime PUBLIC REX_HAS_D3D12=1)
endif()

if(REXGLUE_USE_VULKAN)
    target_link_libraries(rexruntime PUBLIC
        volk::volk
        GPUOpen::VulkanMemoryAllocator
        SPIRV-Tools-static
    )
    target_compile_definitions(rexruntime PUBLIC REX_HAS_VULKAN=1)
endif()

# Static variant: used only by rexcodegen/rexglue
add_library(rexruntime_static STATIC)
target_link_libraries(rexruntime_static PRIVATE
    rexruntime_objects
    simde
    fmt::fmt
    spdlog::spdlog
    tomlplusplus
    SDL3::SDL3
    aes128
    mspack
)

if(REXGLUE_ENABLE_TRACY)
    target_link_libraries(rexruntime_static PRIVATE TracyClient)
endif()

if(WIN32)
    target_link_libraries(rexruntime_static PRIVATE ws2_32)
endif()

if(REXGLUE_USE_D3D12)
    target_link_libraries(rexruntime_static PRIVATE dxgi dxguid dxc-headers)
endif()

if(REXGLUE_USE_VULKAN)
    target_link_libraries(rexruntime_static PRIVATE
        volk::volk
        GPUOpen::VulkanMemoryAllocator
        SPIRV-Tools-static
    )
endif()
