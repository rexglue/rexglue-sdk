set_target_properties(rexcore PROPERTIES EXPORT_NAME core)
set_target_properties(rexfilesystem PROPERTIES EXPORT_NAME filesystem)
set_target_properties(rexui PROPERTIES EXPORT_NAME ui)
set_target_properties(rexinput PROPERTIES EXPORT_NAME input)
set_target_properties(rexaudio PROPERTIES EXPORT_NAME audio)
set_target_properties(rexruntime PROPERTIES EXPORT_NAME runtime)
set_target_properties(rexcodegen PROPERTIES EXPORT_NAME codegen)

set(REXGLUE_INSTALL_TARGETS
    rexruntime
    rexgpu-xenos
    disruptorplus renderdoc simde tomlplusplus
    aes128 mspack o1heap disasm xxhash
    libavcodec libavutil
)

if(REXGLUE_USE_VULKAN)
    list(APPEND REXGLUE_INSTALL_TARGETS
        SPIRV glslang MachineIndependent GenericCodeGen OSDependent OGLCompiler  # glslang
        spirv-tools-headers
    )
endif()

if(REXGLUE_USE_D3D12)
    list(APPEND REXGLUE_INSTALL_TARGETS dxc-headers)
endif()

if(REXGLUE_ENABLE_TRACY)
    list(APPEND REXGLUE_INSTALL_TARGETS TracyClient)
endif()

set(REXGLUE_INSTALL_FIDELITYFX_TARGETS)
if(TARGET amd_fidelityfx_vk)
    list(APPEND REXGLUE_INSTALL_FIDELITYFX_TARGETS amd_fidelityfx_vk)
endif()

if(TARGET amd_fidelityfx_dx12)
    list(APPEND REXGLUE_INSTALL_FIDELITYFX_TARGETS amd_fidelityfx_dx12)
endif()
