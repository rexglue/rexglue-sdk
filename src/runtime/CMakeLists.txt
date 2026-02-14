# rexruntime: Runtime execution layer
# CPU execution, XEX/module loading, thread state, export resolution
# Namespace: rex::runtime

add_library(rexruntime STATIC
    # Guest types (thread-local storage for guest interop)
    guest/types.cpp

    # Module loading
    elf_module.cpp
    entry_table.cpp
    export_resolver.cpp
    lzx.cpp
    mmio_handler.cpp
    map_parser.cpp
    module.cpp
    processor.cpp
    raw_module.cpp
    # test_module.cpp  # TODO: Implement soon
    thread.cpp
    thread_state.cpp
    xex_module.cpp
)
add_library(rex::runtime ALIAS rexruntime)

target_include_directories(rexruntime PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_include_directories(rexruntime PRIVATE
    ${CMAKE_SOURCE_DIR}/thirdparty
)

target_link_libraries(rexruntime
    PUBLIC
        rexcore
        rexfilesystem
    PRIVATE
        mspack
        simde
)

# Propagate graphics backend selection from root CMakeLists.txt
if(REXGLUE_USE_D3D12)
    target_compile_definitions(rexruntime PUBLIC REX_HAS_D3D12=1)
endif()
if(REXGLUE_USE_VULKAN)
    target_compile_definitions(rexruntime PUBLIC REX_HAS_VULKAN=1)
endif()

