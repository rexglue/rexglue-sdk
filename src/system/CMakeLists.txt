# rexsystem: System emulation layer (merged with former rexruntime)
# Namespace: rex::system, rex::runtime

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

    # Utilities
    util/object_table.cpp
    util/native_list.cpp
    util/xdbf_utils.cpp
    util/gameinfo_utils.cpp

    # XAM services
    xam/app_manager.cpp
    xam/content_manager.cpp
    xam/user_profile.cpp

    # Formerly rexruntime: module loading, CPU execution, thread state
    ppc_types.cpp
    elf_module.cpp
    entry_table.cpp
    export_resolver.cpp
    lzx.cpp
    mmio_handler.cpp
    map_parser.cpp
    module.cpp
    processor.cpp
    thread.cpp
    thread_state.cpp
    xex_module.cpp
)

add_library(rexsystem STATIC ${REXSYSTEM_SOURCES})
add_library(rex::system ALIAS rexsystem)

target_include_directories(rexsystem PUBLIC
    $<BUILD_INTERFACE:${REXGLUE_ROOT}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_include_directories(rexsystem PRIVATE
    ${REXGLUE_ROOT}/thirdparty
)

target_link_libraries(rexsystem
    PUBLIC
        rexcore
        rexfilesystem
        simde
    PRIVATE
        aes128
        mspack
)

if(WIN32)
    target_link_libraries(rexsystem PRIVATE ws2_32)
endif()

# Propagate graphics backend selection
if(REXGLUE_USE_D3D12)
    target_compile_definitions(rexsystem PUBLIC REX_HAS_D3D12=1)
endif()
if(REXGLUE_USE_VULKAN)
    target_compile_definitions(rexsystem PUBLIC REX_HAS_VULKAN=1)
endif()
