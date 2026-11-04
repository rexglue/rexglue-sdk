add_library(
  rexfilesystem-objects OBJECT
  container.cpp
  container_provider.cpp
  file_system.cpp
  gdfx.cpp
  host_provider.cpp
  lzx.cpp
  node.cpp
  node_file.cpp
  null_provider.cpp
  provider.cpp
  stfs.cpp
  svod.cpp
  wildcard.cpp
  xdbf.cpp
  xex.cpp
)
if (WIN32)
  target_sources(rexfilesystem-objects PRIVATE host_file_win.cpp)
elseif (UNIX)
  target_sources(rexfilesystem-objects PRIVATE host_file_posix.cpp)
endif ()
set_target_properties(
  rexfilesystem-objects
  PROPERTIES CXX_VISIBILITY_PRESET hidden
             VISIBILITY_INLINES_HIDDEN ON
             POSITION_INDEPENDENT_CODE ON
)
target_include_directories(
  rexfilesystem-objects PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/thirdparty
)
rex_target_pch(rexfilesystem-objects)
target_link_libraries(
  rexfilesystem-objects
  PUBLIC rexbase
  PRIVATE excrypt mspack
)

add_library(rexfilesystem SHARED module_entry.cpp $<TARGET_OBJECTS:rexfilesystem-objects>)
add_library(rex::filesystem ALIAS rexfilesystem)
rex_target_headers(rexfilesystem DIRS filesystem)
rexglue_link_host_allocator(rexfilesystem)
set_target_properties(
  rexfilesystem
  PROPERTIES EXPORT_NAME filesystem
             CXX_VISIBILITY_PRESET hidden
             VISIBILITY_INLINES_HIDDEN ON
)
target_include_directories(
  rexfilesystem PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/thirdparty
)
target_link_libraries(
  rexfilesystem
  PUBLIC rexbase
  PRIVATE excrypt mspack
)
rexglue_add_version_resource(rexfilesystem DESCRIPTION "ReXGlue file system")
