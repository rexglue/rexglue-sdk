set_target_properties(rexcore PROPERTIES EXPORT_NAME core)
set_target_properties(rexfilesystem PROPERTIES EXPORT_NAME filesystem)
set_target_properties(rexui PROPERTIES EXPORT_NAME ui)
set_target_properties(rexruntime PROPERTIES EXPORT_NAME runtime)
set_target_properties(rexcodegen PROPERTIES EXPORT_NAME codegen)
set_target_properties(rexgpu PROPERTIES EXPORT_NAME gpu)

set(REXGLUE_INSTALL_TARGETS
    rexcore
    reximgui
    rexruntime
    rexsdk
    rexgpu
    rexui
    rexui-sdl3
    rexapu-sdl3
    rexhid
    rexhid-sdl3
    rexgpu-xenos
    rexgpu-xenos-vulkan
    rexfilesystem
    renderdoc
    simde
    tomlplusplus
    excrypt
    mspack
    o1heap
    disasm
    xxhash
    libavcodec
    libavutil
)
if (WIN32)
  list(APPEND REXGLUE_INSTALL_TARGETS rexhid-xinput)
endif ()

list(
  APPEND
  REXGLUE_INSTALL_TARGETS
  SPIRV
  glslang
  MachineIndependent
  GenericCodeGen
  OSDependent
  OGLCompiler # glslang
  spirv-tools-headers
)

if (REXGLUE_GPU_D3D12)
  list(APPEND REXGLUE_INSTALL_TARGETS rexgpu-xenos-d3d12 dxc-headers)
endif ()

if (REXGLUE_ENABLE_TRACY)
  list(APPEND REXGLUE_INSTALL_TARGETS TracyClient)
endif ()
