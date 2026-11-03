set(CMAKE_FOLDER "tools")

add_library(
  rexglue-objects OBJECT ui/ui.cpp ui/progress.cpp command/init_command.cpp command/test_spec.cpp
                         command/project_scan.cpp command/test_recompiler.cpp
)
target_include_directories(
  rexglue-objects PRIVATE ${REXGLUE_ROOT}/src ${REXGLUE_ROOT}/thirdparty/disasm
)
target_include_directories(rexglue-objects SYSTEM PRIVATE ${REXGLUE_ROOT}/thirdparty/cli11/include)
target_link_libraries(rexglue-objects PRIVATE rexcodegen tomlplusplus)
rex_target_pch(rexglue-objects <CLI/CLI.hpp>)

add_executable(
  rexglue main.cpp command/codegen_command.cpp command/vfs_command.cpp command/list_command.cpp
          $<TARGET_OBJECTS:rexglue-objects>
)

rexglue_link_host_allocator(rexglue)
rex_config_postfix(rexglue)
rexglue_add_version_resource(rexglue DESCRIPTION "ReXGlue recompiler")

target_include_directories(rexglue PRIVATE ${REXGLUE_ROOT}/src ${REXGLUE_ROOT}/thirdparty/disasm)
target_include_directories(rexglue SYSTEM PRIVATE ${REXGLUE_ROOT}/thirdparty/cli11/include)

target_link_libraries(rexglue PRIVATE rexcodegen tomlplusplus)

rex_target_pch(rexglue <CLI/CLI.hpp>)

add_executable(rex::rexglue ALIAS rexglue)
add_dependencies(
  rexglue
  rexfilesystem
  rexapu
  rexhid
  rexui
  rexgpu
  rexxam
)
