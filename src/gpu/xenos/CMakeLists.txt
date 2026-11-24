# rexgpu-xenos-base, the shared Xenos code, and the null, Vulkan and D3D12 driver modules

set(REXGPU_XENOS_BASE_SOURCES
    pipeline/shader/shader.cpp
    sampler_info.cpp
    xenos_graphics_driver_base.cpp
    command_processor.cpp
    draw_extent_estimator.cpp
    draw_state.cpp
    index_conversion.cpp
    primitive_processor.cpp
    pipeline/render_target/cache.cpp
    shared_memory.cpp
    pipeline/texture/cache.cpp
    host/imgui_overlay.cpp
    host/surface.cpp
    host/xenos_presentation.cpp
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

# Vulkan driver
set(REXGPU_XENOS_VULKAN_SOURCES
    vulkan/vulkan_xenos_graphics_driver.cpp
    vulkan/vulkan_xenos_provider.cpp
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

# D3D12 driver
if (REXGLUE_GPU_D3D12)
  set(REXGPU_XENOS_DXBC_THIRDPARTY_SOURCES ${REXGLUE_ROOT}/thirdparty/dxbc/DXBCChecksum.cpp)

  set(REXGPU_XENOS_DXBC_SOURCES
      pipeline/shader/dxbc.cpp pipeline/shader/dxbc_translator.cpp
      pipeline/shader/dxbc_translator_alu.cpp pipeline/shader/dxbc_translator_fetch.cpp
      pipeline/shader/dxbc_translator_memexport.cpp pipeline/shader/dxbc_translator_om.cpp
  )

  set(REXGPU_XENOS_D3D12_SOURCES
      d3d12/d3d12_xenos_graphics_driver.cpp
      d3d12/d3d12_xenos_provider.cpp
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
    host/vulkan/ui_sampler.cpp
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

list(APPEND REXGPU_XENOS_BASE_SOURCES ${REXGPU_XENOS_HOST_SOURCES})

set(REXGPU_XENOS_VULKAN_ALL ${REXGPU_XENOS_VULKAN_SOURCES} ${REXGPU_XENOS_SPIRV_SOURCES}
                            ${REXGPU_XENOS_HOST_VULKAN_SOURCES}
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
  set(REXGPU_XENOS_D3D12_ALL ${REXGPU_XENOS_DXBC_THIRDPARTY_SOURCES} ${REXGPU_XENOS_DXBC_SOURCES}
                             ${REXGPU_XENOS_D3D12_SOURCES} ${REXGPU_XENOS_HOST_D3D12_SOURCES}
  )
endif ()

set(REXGPU_XENOS_INCLUDE_DIRS ${REXGLUE_ROOT}/src ${PROJECT_BINARY_DIR}/include
                              ${REXGLUE_ROOT}/thirdparty/renderdoc ${REXGLUE_ROOT}
)

set(REXGPU_XENOS_PCH_HEADERS
    <unordered_map>
    <rex/math.h>
    <rex/memory.h>
    <rex/xenos/fetch.h>
    <rex/xenos/format.h>
    <rex/xenos/packet.h>
    <rex/xenos/state.h>
    <rex/xenos/register.h>
    <rex/xenos/ucode.h>
    <gpu/xenos/pipeline/shader/shader.h>
)

# Adds a Xenos driver library exported as rex::<export_name>.
function (rexgpu_xenos_library target export_name description)
  add_library(${target} SHARED ${ARGN})
  rexglue_link_host_allocator(${target})
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

add_library(rexgpu-xenos-base STATIC ${REXGPU_XENOS_BASE_SOURCES})
add_library(rex::gpu-xenos-base ALIAS rexgpu-xenos-base)
set_target_properties(
  rexgpu-xenos-base
  PROPERTIES EXPORT_NAME gpu-xenos-base
             CXX_VISIBILITY_PRESET hidden
             VISIBILITY_INLINES_HIDDEN ON
             POSITION_INDEPENDENT_CODE ON
)
target_compile_definitions(rexgpu-xenos-base PRIVATE REX_BUILDING_GPU_XENOS)
target_include_directories(rexgpu-xenos-base PRIVATE ${REXGPU_XENOS_INCLUDE_DIRS})
rex_target_pch(rexgpu-xenos-base ${REXGPU_XENOS_PCH_HEADERS})
target_link_libraries(rexgpu-xenos-base PUBLIC rexbase rexxenos rexgpu renderdoc xxHash::xxhash)
if (WIN32)
  target_link_libraries(rexgpu-xenos-base PUBLIC dxgi)
endif ()

rexgpu_xenos_library(
  rexgpu-xenos-null gpu-xenos-null "ReXGlue null Xenos GPU" null/null_xenos_provider.cpp
  null/null_xenos_graphics_driver.cpp null/null_command_processor.cpp
)
target_compile_definitions(rexgpu-xenos-null PRIVATE REX_GPU_XENOS_STATIC)
target_link_libraries(rexgpu-xenos-null PRIVATE rexgpu-xenos-base)

if (REXGLUE_USE_VULKAN)
  rexgpu_xenos_library(
    rexgpu-xenos-vulkan gpu-xenos-vulkan "ReXGlue Xenos GPU emulation on Vulkan"
    ${REXGPU_XENOS_VULKAN_ALL}
  )
  target_compile_definitions(rexgpu-xenos-vulkan PRIVATE REX_GPU_XENOS_STATIC)
  target_link_libraries(
    rexgpu-xenos-vulkan
    PRIVATE rexgpu-xenos-base
            rexgpu
            renderdoc
            xxHash::xxhash
            glslang::SPIRV
            glslang
            MachineIndependent
            GenericCodeGen
            OSDependent
            OGLCompiler
            rex-vma
            Vulkan::Headers
  )
endif ()

if (REXGLUE_GPU_D3D12)
  rexgpu_xenos_library(
    rexgpu-xenos-d3d12 gpu-xenos-d3d12 "ReXGlue Xenos GPU emulation on Direct3D 12"
    ${REXGPU_XENOS_D3D12_ALL}
  )
  target_compile_definitions(rexgpu-xenos-d3d12 PRIVATE REX_GPU_XENOS_STATIC)
  target_link_libraries(
    rexgpu-xenos-d3d12
    PRIVATE rexgpu-xenos-base
            rexgpu
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

# Metal driver. The fork's src/graphics/CMakeLists.txt is the template: the same three dependencies (Metal Shader Converter's
# dylib, spirv-cross msl/core from the MoltenVK submodule, the DXC/DXIL conversion headers) and the same frameworks.
if (REXGLUE_USE_METAL)
  # The DXBC shader model (DXBCShader and its translations) is what the Metal backend's shader classes derive from.
  set(REXGPU_XENOS_METAL_SOURCES
      ${REXGLUE_ROOT}/thirdparty/dxbc/DXBCChecksum.cpp
      pipeline/shader/dxbc.cpp
      pipeline/shader/dxbc_translator.cpp
      pipeline/shader/dxbc_translator_alu.cpp
      pipeline/shader/dxbc_translator_fetch.cpp
      pipeline/shader/dxbc_translator_memexport.cpp
      pipeline/shader/dxbc_translator_om.cpp
      metal/dxbc_to_dxil_converter.cc
      metal/ir_runtime_impl.mm
      metal/metal_command_processor.mm
      metal/metal_dxil_store.mm
      metal/metal_encode_stream.mm
      metal/metal_load_shader_pipelines.mm
      metal/metal_pipeline_cache.mm
      metal/metal_pipeline_storage.mm
      metal/metal_primitive_processor.cc
      metal/metal_render_target_cache.mm
      metal/metal_resolve_shader_pipelines.mm
      metal/metal_shader.mm
      metal/metal_shader_converter.cc
      metal/metal_shared_memory.mm
      metal/metal_shared_texture_cache.mm
      metal/metal_texture_cache.mm
      metal/metal_upload_arena.mm
      metal/metal_zpd_visibility_pool.mm
      metal/metal_draw_util.cc
      metal/host_tessellator.cc
      metal/dxbc_geometry_shader.cc
      metal/resolve_writeback_log.cc
      ${REXGLUE_ROOT}/thirdparty/d3d11-tessellator/tessellator.cpp
      metal/metal_xenos_graphics_driver.cpp
  )

  rexgpu_xenos_library(
    rexgpu-xenos-metal gpu-xenos-metal "ReXGlue Xenos GPU emulation on Metal"
    ${REXGPU_XENOS_METAL_SOURCES}
  )
  target_compile_definitions(rexgpu-xenos-metal PRIVATE REX_GPU_XENOS_STATIC)
  # The ported sources are written against the bridges in this header (SAL annotations for the DXC headers, the XE_* spellings,
  # the setting macros). Every per-unit check compiled with it force-included, so the target does the same.
  target_compile_options(
    rexgpu-xenos-metal PRIVATE -include
                               ${REXGLUE_ROOT}/include/rex/graphics/metal/metal_upstream_compat.h
                               $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
                               # The ported sources are the fork's code, not ours to restyle; these are diagnostics, not defects.
                               -Wno-error=shadow -Wno-error=int-in-bool-context
                               -Wno-error=unused-variable
                               -Wno-error=unused-but-set-variable
                               -Wno-error=unused-parameter
                               -Wno-error=undefined-bool-conversion
                               -Wno-error=unused-private-field
                               -Wno-error=missing-field-initializers
                               -Wno-error=overloaded-virtual
                               -Wno-error=unguarded-availability-new
                               -Wno-error=macro-redefined
                               -Wno-error=sign-compare
                               -Wno-error=unused-function
                               -Wno-error=logical-not-parentheses
                               -Wno-error=enum-compare
                               -Wno-error=deprecated-anon-enum-enum-conversion
  )

  # The ported backend owns the public headers it carries: the whole graphics folder plus the metal-specific ones,
  # including the forwarders and the bridge headers. Without this the header-set audit fails the configure.
  rex_target_headers(rexgpu-xenos-metal DIRS graphics)


  # The Metal shader path needs two external toolchains, and neither can be vendored:
  #   - dxilconv, the DXBC -> DXIL component of Microsoft's DirectXShaderCompiler, built as its
  #     own target (libdxilconv. It is a nested LLVM build, not a package);
  #   - Apple's Metal Shader Converter, which converts DXIL to Metal IR.
  # Point the build at them with these cache variables; the in-tree locations are only defaults
  # for a checkout that already carries them.
  set(REXGLUE_DXILCONV_LIBRARY_DIR "" CACHE PATH
      "Directory containing libdxilconv (the DirectXShaderCompiler dxilconv target's output)")
  set(REXGLUE_DXC_INCLUDE_DIR "" CACHE PATH
      "DirectXShaderCompiler include directory (DxbcConverter.h and Support/)")
  set(REXGLUE_METAL_SHADER_CONVERTER_DIR "" CACHE PATH
      "Apple Metal Shader Converter root (containing lib/ and include/)")

  find_library(DXILCONV_LIBRARY dxilconv
               HINTS "${REXGLUE_DXILCONV_LIBRARY_DIR}"
                     "${REXGLUE_ROOT}/thirdparty/DirectXShaderCompiler/build_dxilconv_macos/lib")
  if (NOT DXILCONV_LIBRARY)
    message(FATAL_ERROR
            "rexglue: libdxilconv not found. Build DirectXShaderCompiler's dxilconv target and set "
            "REXGLUE_DXILCONV_LIBRARY_DIR to the directory containing it.")
  endif ()

  find_library(METAL_IRCONVERTER_LIBRARY metalirconverter
               HINTS "${REXGLUE_METAL_SHADER_CONVERTER_DIR}/lib"
                     "${REXGLUE_ROOT}/thirdparty/metal-shader-converter/lib")
  if (NOT METAL_IRCONVERTER_LIBRARY)
    message(FATAL_ERROR
            "rexglue: libmetalirconverter not found. Install Apple's Metal Shader Converter and set "
            "REXGLUE_METAL_SHADER_CONVERTER_DIR to its root.")
  endif ()

  if (REXGLUE_DXC_INCLUDE_DIR)
    # DxbcConverter.h sits in the root; WinAdapter.h in dxc/Support.
    set(REXGLUE_DXC_INCLUDE_DIRS
        "${REXGLUE_ROOT}/thirdparty/dxc/include"
        "${REXGLUE_DXC_INCLUDE_DIR}"
        "${REXGLUE_DXC_INCLUDE_DIR}/dxc"
        "${REXGLUE_DXC_INCLUDE_DIR}/dxc/Support")
  else ()
    set(REXGLUE_DXC_INCLUDE_DIRS
        "${REXGLUE_ROOT}/thirdparty/dxc/include"
        "${REXGLUE_ROOT}/thirdparty/DirectXShaderCompiler/include"
        "${REXGLUE_ROOT}/thirdparty/DirectXShaderCompiler/include/dxc"
        "${REXGLUE_ROOT}/thirdparty/DirectXShaderCompiler/include/dxc/Support")
  endif ()

  target_include_directories(
    rexgpu-xenos-metal
    PRIVATE ${REXGLUE_ROOT}
            ${REXGLUE_DXC_INCLUDE_DIRS}
            "${REXGLUE_METAL_SHADER_CONVERTER_DIR}/include"
            "${REXGLUE_ROOT}/thirdparty/metal-shader-converter/include"
  )

  # INTERIM LAYERING DEBT: the ported backend calls MetalProvider::Create and its device picker directly, so this target has to
  # see rexui. The fix is to route both through the driver's provider hook and drop this line; until then the arrow points
  # GPU -> UI, which is backwards.
  target_link_libraries(
    rexgpu-xenos-metal
    PRIVATE rexgpu-xenos-base
            rexui
            rexgpu
            renderdoc
            xxHash::xxhash
            fmt::fmt
            "-framework Metal"
            "-framework Foundation"
            "-framework QuartzCore"
  )
  if (DXILCONV_LIBRARY)
    target_link_libraries(rexgpu-xenos-metal PRIVATE "${DXILCONV_LIBRARY}")
  endif ()
  if (METAL_IRCONVERTER_LIBRARY)
    target_link_libraries(rexgpu-xenos-metal PRIVATE "${METAL_IRCONVERTER_LIBRARY}")
  endif ()
  if (TARGET spirv-cross-msl)
    target_link_libraries(rexgpu-xenos-metal PRIVATE spirv-cross-msl spirv-cross-core)
  endif ()
endif ()
