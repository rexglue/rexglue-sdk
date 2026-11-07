set_target_properties(rexbase PROPERTIES EXPORT_NAME base)
set_target_properties(rexppc PROPERTIES EXPORT_NAME ppc)
set_target_properties(rexkernel PROPERTIES EXPORT_NAME kernel)
set_target_properties(rexxam PROPERTIES EXPORT_NAME xam)
set_target_properties(rexxbdm PROPERTIES EXPORT_NAME xbdm)
set_target_properties(rexcodegen PROPERTIES EXPORT_NAME codegen)
set_target_properties(rexgpu PROPERTIES EXPORT_NAME gpu)

set(REXGLUE_INSTALL_TARGETS
    rexbase
    rexppc
    rexxenos
    rexkernel
    rexxam
    rexxbdm
    rexapp
    rexsdk
    simde
    tlsf
)

set(REXGLUE_MODULE_TARGETS
    rexfilesystem
    rexapu
    rexhid
    rexui
    rexgpu
    rexui-sdl3
    rexapu-sdl3
    rexhid-sdl3
    rexgpu-xenos-null
    rexgpu-xenos-vulkan
)
if (WIN32)
  list(APPEND REXGLUE_MODULE_TARGETS rexhid-xinput)
endif ()

if (REXGLUE_GPU_D3D12)
  list(APPEND REXGLUE_MODULE_TARGETS rexgpu-xenos-d3d12)
endif ()

if (REXGLUE_ENABLE_TRACY)
  list(APPEND REXGLUE_INSTALL_TARGETS TracyClient)
endif ()
