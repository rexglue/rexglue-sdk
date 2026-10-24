add_library(rexfilesystem-host-objects OBJECT wildcard.cpp)
if (WIN32)
  target_sources(rexfilesystem-host-objects PRIVATE host_file_win.cpp)
elseif (UNIX)
  target_sources(rexfilesystem-host-objects PRIVATE host_file_posix.cpp)
endif ()
set_target_properties(
  rexfilesystem-host-objects
  PROPERTIES CXX_VISIBILITY_PRESET hidden
             VISIBILITY_INLINES_HIDDEN ON
             POSITION_INDEPENDENT_CODE ON
)
target_include_directories(rexfilesystem-host-objects PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(rexfilesystem-host-objects PRIVATE rexcore)

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
  $<TARGET_OBJECTS:rexfilesystem-host-objects>
)
add_library(rex::filesystem ALIAS rexfilesystem)
rexglue_link_host_allocator(rexfilesystem)
set_target_properties(
  rexfilesystem PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON
)
rexglue_add_version_resource(rexfilesystem DESCRIPTION "ReXGlue filesystem")

target_include_directories(rexfilesystem PRIVATE ${PROJECT_SOURCE_DIR}/src)

target_link_libraries(rexfilesystem PRIVATE rexcore)
