add_library(
  rexfilesystem SHARED
  container_provider.cpp
  file_system.cpp
  host_provider.cpp
  module.cpp
  node.cpp
  node_file.cpp
  null_provider.cpp
  provider.cpp
  wildcard.cpp
)
add_library(rex::filesystem ALIAS rexfilesystem)

if (WIN32)
  target_sources(rexfilesystem PRIVATE host_file_win.cpp)
elseif (UNIX)
  target_sources(rexfilesystem PRIVATE host_file_posix.cpp)
endif ()

set_target_properties(
  rexfilesystem PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON
)
rexglue_add_version_resource(rexfilesystem DESCRIPTION "ReXGlue filesystem")

target_include_directories(rexfilesystem PRIVATE ${PROJECT_SOURCE_DIR}/src)

target_link_libraries(rexfilesystem PRIVATE rexcore)
