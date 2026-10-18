# ReXGlue

add_executable(
  rexglue
  main.cpp
  ui/ui.cpp
  ui/progress.cpp
  commands/codegen_command.cpp
  commands/init_command.cpp
  commands/vfs_command.cpp
  commands/list_command.cpp
  commands/test_recompiler.cpp
  commands/test_spec.cpp
  commands/project_scan.cpp
)

rex_config_postfix(rexglue)
rexglue_add_version_resource(rexglue DESCRIPTION "ReXGlue recompiler")

target_include_directories(rexglue PRIVATE ${REXGLUE_ROOT}/src ${REXGLUE_ROOT}/thirdparty/disasm)
target_include_directories(rexglue SYSTEM PRIVATE ${REXGLUE_ROOT}/thirdparty/cli11/include)

target_link_libraries(rexglue PRIVATE rexcodegen)

rex_target_pch(rexglue <CLI/CLI.hpp>)

add_dependencies(rexglue rexfilesystem)

add_executable(rex::rexglue ALIAS rexglue)
