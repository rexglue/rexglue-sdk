# rex-gpu-xenos, the Xenos GPU backend module

set(REXGPU_XENOS_SOURCES
    plugin_main.cpp

    # Core GPU types
    pipeline/shader/shader.cpp
    sampler_info.cpp

    # GPU system
    graphics_system.cpp
    command_processor.cpp
    null/null_command_processor.cpp
    null/null_graphics_system.cpp
    draw_extent_estimator.cpp
    draw_state.cpp
    primitive_processor.cpp
    pipeline/render_target/cache.cpp
    shared_memory.cpp
    pipeline/texture/cache.cpp
    pipeline/shader/interpreter.cpp
    pipeline/shader/translator.cpp
    pipeline/shader/translator_disasm.cpp
)

# SPIR-V shader translation
set(REXGPU_XENOS_SPIRV_SOURCES
    pipeline/shader/spirv_builder.cpp
    pipeline/shader/spirv.cpp
    pipeline/shader/spirv_translator.cpp
    pipeline/shader/spirv_translator_alu.cpp
    pipeline/shader/spirv_translator_fetch.cpp
    pipeline/shader/spirv_translator_memexport.cpp
    pipeline/shader/spirv_translator_rb.cpp
)

# Vulkan backend
set(REXGPU_XENOS_VULKAN_SOURCES
    vulkan/graphics_system.cpp
    vulkan/deferred_command_buffer.cpp
    vulkan/command_processor.cpp
    vulkan/pipeline_cache.cpp
    vulkan/primitive_processor.cpp
    vulkan/render_target_cache.cpp
    vulkan/shader.cpp
    vulkan/shared_memory.cpp
    vulkan/texture_cache.cpp
)

# D3D12 backend
if(REXGLUE_GPU_D3D12)
    # DXBC third-party dependency
    set(REXGPU_XENOS_DXBC_THIRDPARTY_SOURCES
        ${REXGLUE_ROOT}/thirdparty/dxbc/DXBCChecksum.cpp
    )

    # DXBC shader translation (required for D3D12)
    set(REXGPU_XENOS_DXBC_SOURCES
        pipeline/shader/dxbc.cpp
        pipeline/shader/dxbc_translator.cpp
        pipeline/shader/dxbc_translator_alu.cpp
        pipeline/shader/dxbc_translator_fetch.cpp
        pipeline/shader/dxbc_translator_memexport.cpp
        pipeline/shader/dxbc_translator_om.cpp
    )

    # D3D12 GPU backend
    set(REXGPU_XENOS_D3D12_SOURCES
        d3d12/graphics_system.cpp
        d3d12/command_processor.cpp
        d3d12/primitive_processor.cpp
        d3d12/render_target_cache.cpp
        d3d12/shader.cpp
        d3d12/shared_memory.cpp
        d3d12/texture_cache.cpp
        d3d12/deferred_command_list.cpp
        d3d12/pipeline_cache.cpp
    )
endif()

set(REXGPU_XENOS_HOST_SOURCES
    host/graphics_upload_buffer_pool.cpp
    host/fixed_point.cpp
    host/immediate_drawer.cpp
    host/presenter.cpp
    host/renderdoc_api.cpp
    host/vulkan/linked_type_descriptor_set_allocator.cpp
    host/vulkan/vulkan_moltenvk.cpp
    host/vulkan/single_layout_descriptor_set_pool.cpp
    host/vulkan/ui_samplers.cpp
    host/vulkan/vulkan_device.cpp
    host/vulkan/vulkan_immediate_drawer.cpp
    host/vulkan/vulkan_instance.cpp
    host/vulkan/vulkan_mem_alloc.cpp
    host/vulkan/vulkan_presenter.cpp
    host/vulkan/vulkan_provider.cpp
    host/vulkan/vulkan_submission_tracker.cpp
    host/vulkan/vulkan_upload_buffer_pool.cpp
    host/vulkan/vulkan_objects.cpp
)
if(REXGLUE_GPU_D3D12)
    list(APPEND REXGPU_XENOS_HOST_SOURCES
        host/d3d12/d3d12_cpu_descriptor_pool.cpp
        host/d3d12/d3d12_descriptor_heap_pool.cpp
        host/d3d12/d3d12_immediate_drawer.cpp
        host/d3d12/d3d12_presenter.cpp
        host/d3d12/d3d12_provider.cpp
        host/d3d12/d3d12_submission_tracker.cpp
        host/d3d12/d3d12_upload_buffer_pool.cpp
        host/d3d12/d3d12_objects.cpp)
endif()

list(APPEND REXGPU_XENOS_SOURCES
    ${REXGPU_XENOS_SPIRV_SOURCES}
    ${REXGPU_XENOS_VULKAN_SOURCES}
    ${REXGPU_XENOS_HOST_SOURCES})

if(REXGLUE_GPU_D3D12)
    list(APPEND REXGPU_XENOS_SOURCES
        ${REXGPU_XENOS_DXBC_THIRDPARTY_SOURCES}
        ${REXGPU_XENOS_DXBC_SOURCES}
        ${REXGPU_XENOS_D3D12_SOURCES})
endif()

add_library(rex-gpu-xenos SHARED ${REXGPU_XENOS_SOURCES})
add_library(rex::gpu-xenos ALIAS rex-gpu-xenos)

set_target_properties(rex-gpu-xenos PROPERTIES
    EXPORT_NAME gpu-xenos
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
rexglue_add_version_resource(rex-gpu-xenos DESCRIPTION "ReXGlue Xenos GPU emulation")

target_include_directories(rex-gpu-xenos PRIVATE
    ${REXGLUE_ROOT}/src
    ${PROJECT_BINARY_DIR}/include
    ${REXGLUE_ROOT}/thirdparty/renderdoc
    ${REXGLUE_ROOT}
)

rex_target_pch(rex-gpu-xenos
    <mutex>
    <unordered_map>
    <rex/math.h>
    <rex/memory.h>
    <rex/runtime/xmemory.h>
    <rex/xenos/xenos.h>
    <rex/xenos/registers.h>
    <rex/xenos/ucode.h>
    <gpu/xenos/pipeline/shader/shader.h>
)

if(REXGPU_XENOS_DXBC_THIRDPARTY_SOURCES)
    set_source_files_properties(${REXGPU_XENOS_DXBC_THIRDPARTY_SOURCES}
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON COMPILE_OPTIONS -w)
endif()

target_link_libraries(rex-gpu-xenos PRIVATE
    rex-gpu
    rex-runtime
    renderdoc
    xxHash::xxhash
    Vulkan::Headers
    GPUOpen::VulkanMemoryAllocator
    spirv-tools-headers
    glslang::SPIRV
    glslang
    MachineIndependent
    GenericCodeGen
    OSDependent
    OGLCompiler
)

if(REXGLUE_GPU_D3D12)
    target_link_libraries(rex-gpu-xenos PRIVATE
        d3d12
        dxgi
        dxguid
        dxc-headers
    )
endif()
