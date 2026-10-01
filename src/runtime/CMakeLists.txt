set(REXSYSTEM_SOURCES
    runtime.cpp
    error.cpp
    embedded_metadata.cpp
    achievement_manager.cpp
    xmemory.cpp
    kernel_module.cpp
    user_module.cpp
    xthread.cpp
    thread_registry.cpp
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
    module_registry.cpp
    notifier.cpp
    dispatcher.cpp
    process.cpp
    guest_path.cpp

    # Utilities
    object_table.cpp
    util/native_list.cpp
    util/xdbf_utils.cpp
    util/gameinfo_utils.cpp

    # XAM services
    xam/app_manager.cpp
    xam/content_manager.cpp
    xam/user_profile.cpp

    ppc_types.cpp
    elf_module.cpp
    lzx.cpp
    mmio_handler.cpp
    map_parser.cpp
    module.cpp
    function_dispatcher.cpp
    runtime_module.cpp
    thread_state.cpp
    xex_module.cpp
)

add_library(rexruntime SHARED ${REXSYSTEM_SOURCES})
add_library(rex::runtime ALIAS rexruntime)

set_target_properties(rexruntime PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS TRUE)
rexglue_add_version_resource(rexruntime DESCRIPTION "ReXGlue runtime")

rex_target_pch(rexruntime
    <mutex>
    <unordered_map>
    <rex/math.h>
    <rex/string.h>
    <rex/memory.h>
    <rex/filesystem.h>
    <rex/platform/thread.h>
    <rex/ppc/context.h>
    <rex/runtime.h>
)

target_include_directories(rexruntime PUBLIC
    $<BUILD_INTERFACE:${REXGLUE_ROOT}/include>
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>
    $<BUILD_INTERFACE:${REXGLUE_ROOT}/thirdparty/imgui>
    $<BUILD_INTERFACE:${REXGLUE_ROOT}/thirdparty/xxHash>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_include_directories(rexruntime PRIVATE
    ${REXGLUE_ROOT}/src
    ${REXGLUE_ROOT}/thirdparty
)

target_link_libraries(rexruntime
    PUBLIC
        rexcore
        simde
        fmt::fmt
        tomlplusplus
        SDL3::SDL3
    PRIVATE
        aes128
        mspack
        xxHash::xxhash
)

if(REXGLUE_ENABLE_TRACY)
    # TARGET_NAME is what tells install(EXPORT) this text is a target to
    # namespace. Without it the plain name survives into the installed config
    # and consumers link a bare TracyClient they have no search path for.
    target_link_libraries(rexruntime PUBLIC
        $<$<NOT:$<CONFIG:Release>>:$<TARGET_NAME:TracyClient>>)
    target_compile_definitions(rexruntime PUBLIC
        $<$<NOT:$<CONFIG:Release>>:REXGLUE_ENABLE_PROFILING>)
endif()

if(REXGLUE_ENABLE_PERF_COUNTERS)
    target_compile_definitions(rexruntime PUBLIC
        $<$<NOT:$<CONFIG:Release>>:REXGLUE_ENABLE_PERF_COUNTERS>)
endif()

if(WIN32)
    target_link_libraries(rexruntime PRIVATE ws2_32 winmm)
    target_link_libraries(rexruntime PUBLIC dxgi dxguid)
    target_compile_definitions(rexruntime PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

if(REXGLUE_USE_D3D12)
    target_link_libraries(rexruntime PUBLIC dxc-headers)
    target_compile_definitions(rexruntime PUBLIC REX_HAS_D3D12=1)
endif()
if(REXGLUE_USE_VULKAN)
    target_link_libraries(rexruntime PUBLIC
        Vulkan::Headers
        GPUOpen::VulkanMemoryAllocator
        spirv-tools-headers
    )
    target_compile_definitions(rexruntime PUBLIC REX_HAS_VULKAN=1)
endif()
