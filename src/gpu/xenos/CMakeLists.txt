# rexgpu-xenos, the Xenos GPU backend module

set(REXGPU_XENOS_SOURCES
    plugin_main.cpp
    shared_cvars.cpp
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
    index_conversion.cpp
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
    vulkan/tessellation_glsl.cpp
    vulkan/texture_cache.cpp
)

# D3D12 backend
if (REXGLUE_GPU_D3D12)
  # DXBC third-party dependency
  set(REXGPU_XENOS_DXBC_THIRDPARTY_SOURCES ${REXGLUE_ROOT}/thirdparty/dxbc/DXBCChecksum.cpp)

  # DXBC shader translation (required for D3D12)
  set(REXGPU_XENOS_DXBC_SOURCES
      pipeline/shader/dxbc.cpp pipeline/shader/dxbc_translator.cpp
      pipeline/shader/dxbc_translator_alu.cpp pipeline/shader/dxbc_translator_fetch.cpp
      pipeline/shader/dxbc_translator_memexport.cpp pipeline/shader/dxbc_translator_om.cpp
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
endif ()

set(REXGPU_XENOS_HOST_SOURCES host/graphics_upload_buffer_pool.cpp host/fixed_point.cpp
                              host/immediate_drawer.cpp host/presenter.cpp host/renderdoc_api.cpp
)

set(REXGPU_XENOS_HOST_VULKAN_SOURCES
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

list(APPEND REXGPU_XENOS_SOURCES ${REXGPU_XENOS_HOST_SOURCES})

set(REXGPU_XENOS_VULKAN_ALL vulkan/plugin_main.cpp ${REXGPU_XENOS_VULKAN_SOURCES}
                            ${REXGPU_XENOS_SPIRV_SOURCES} ${REXGPU_XENOS_HOST_VULKAN_SOURCES}
)

if (REXGLUE_GPU_D3D12)
  set(REXGPU_XENOS_HOST_D3D12_SOURCES
      host/d3d12/d3d12_cpu_descriptor_pool.cpp
      host/d3d12/d3d12_descriptor_heap_pool.cpp
      host/d3d12/d3d12_immediate_drawer.cpp
      host/d3d12/d3d12_presenter.cpp
      host/d3d12/d3d12_provider.cpp
      host/d3d12/d3d12_submission_tracker.cpp
      host/d3d12/d3d12_upload_buffer_pool.cpp
      host/d3d12/d3d12_objects.cpp
  )
  set(REXGPU_XENOS_D3D12_ALL
      d3d12/plugin_main.cpp ${REXGPU_XENOS_DXBC_THIRDPARTY_SOURCES} ${REXGPU_XENOS_DXBC_SOURCES}
      ${REXGPU_XENOS_D3D12_SOURCES} ${REXGPU_XENOS_HOST_D3D12_SOURCES}
  )
endif ()

set(REXGPU_XENOS_INCLUDE_DIRS ${REXGLUE_ROOT}/src ${PROJECT_BINARY_DIR}/include
                              ${REXGLUE_ROOT}/thirdparty/renderdoc ${REXGLUE_ROOT}
)

set(REXGPU_XENOS_PCH_HEADERS
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

# Adds a Xenos backend library exported as rex::<export_name>.
function (rexgpu_xenos_library target export_name description)
  add_library(${target} SHARED ${ARGN})
  add_library(rex::${export_name} ALIAS ${target})
  set_target_properties(
    ${target}
    PROPERTIES EXPORT_NAME ${export_name}
               CXX_VISIBILITY_PRESET hidden
               VISIBILITY_INLINES_HIDDEN ON
  )
  rexglue_add_version_resource(${target} DESCRIPTION "${description}")
  target_include_directories(${target} PRIVATE ${REXGPU_XENOS_INCLUDE_DIRS})
  rex_target_pch(${target} ${REXGPU_XENOS_PCH_HEADERS})
  if (UNIX
      AND NOT APPLE
      AND NOT REXGLUE_ENABLE_TSAN
      AND NOT REXGLUE_ENABLE_SANITIZERS
  )
    target_link_options(${target} PRIVATE -Wl,--no-undefined)
  endif ()
endfunction ()

rexgpu_xenos_library(rexgpu-xenos gpu-xenos "ReXGlue Xenos GPU emulation" ${REXGPU_XENOS_SOURCES})
target_compile_definitions(rexgpu-xenos PRIVATE REX_BUILDING_GPU_XENOS)
target_link_libraries(rexgpu-xenos PRIVATE rexgpu rexruntime renderdoc xxHash::xxhash)
if (WIN32)
  target_link_libraries(rexgpu-xenos PRIVATE dxgi)
endif ()

rexgpu_xenos_library(
  rexgpu-xenos-vulkan gpu-xenos-vulkan "ReXGlue Xenos GPU emulation on Vulkan"
  ${REXGPU_XENOS_VULKAN_ALL}
)
target_link_libraries(
  rexgpu-xenos-vulkan
  PRIVATE rexgpu-xenos
          rexgpu
          rexruntime
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

if (REXGLUE_GPU_D3D12)
  rexgpu_xenos_library(
    rexgpu-xenos-d3d12 gpu-xenos-d3d12 "ReXGlue Xenos GPU emulation on Direct3D 12"
    ${REXGPU_XENOS_D3D12_ALL}
  )
  target_link_libraries(
    rexgpu-xenos-d3d12
    PRIVATE rexgpu-xenos
            rexgpu
            rexruntime
            renderdoc
            xxHash::xxhash
            d3d12
            dxgi
            dxguid
            dxc-headers
  )
  if (REXGPU_XENOS_DXBC_THIRDPARTY_SOURCES)
    set_source_files_properties(
      ${REXGPU_XENOS_DXBC_THIRDPARTY_SOURCES} PROPERTIES SKIP_PRECOMPILE_HEADERS ON COMPILE_OPTIONS
                                                                                    -w
    )
  endif ()
endif ()
