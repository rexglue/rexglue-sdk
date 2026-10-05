set_target_properties(rex-core PROPERTIES EXPORT_NAME core)
set_target_properties(rex-filesystem PROPERTIES EXPORT_NAME filesystem)
set_target_properties(rex-ui PROPERTIES EXPORT_NAME ui)
set_target_properties(rex-input PROPERTIES EXPORT_NAME input)
set_target_properties(rex-audio PROPERTIES EXPORT_NAME audio)
set_target_properties(rex-runtime PROPERTIES EXPORT_NAME runtime)
set_target_properties(rex-codegen PROPERTIES EXPORT_NAME codegen)

set(REXGLUE_INSTALL_TARGETS
    rex-core
    rex-runtime
    rex-sdk
    rex-gpu-xenos
    rex-filesystem
    disruptorplus renderdoc simde tomlplusplus
    aes128 mspack o1heap disasm xxhash
    libavcodec libavutil
)

list(APPEND REXGLUE_INSTALL_TARGETS
    SPIRV glslang MachineIndependent GenericCodeGen OSDependent OGLCompiler  # glslang
    spirv-tools-headers
)

if(REXGLUE_GPU_D3D12)
    list(APPEND REXGLUE_INSTALL_TARGETS dxc-headers)
endif()

if(REXGLUE_ENABLE_TRACY)
    list(APPEND REXGLUE_INSTALL_TARGETS TracyClient)
endif()
