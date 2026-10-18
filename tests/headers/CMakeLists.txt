file(
  GLOB_RECURSE header_paths
  RELATIVE "${REXGLUE_ROOT}/include"
  CONFIGURE_DEPENDS "${REXGLUE_ROOT}/include/rex/*.h"
)
if (NOT WIN32)
  list(FILTER header_paths EXCLUDE REGEX "_win\\.h$")
endif ()
if (NOT APPLE)
  list(FILTER header_paths EXCLUDE REGEX "_mac\\.h$")
endif ()
if (NOT (UNIX AND NOT APPLE))
  list(FILTER header_paths EXCLUDE REGEX "_gnulinux\\.h$")
endif ()

set(_header_units "")
foreach (header IN LISTS header_paths)
  set(_unit "${CMAKE_CURRENT_BINARY_DIR}/${header}.cpp")
  file(
    GENERATE
    OUTPUT "${_unit}"
    CONTENT "#include <${header}>\n"
  )
  list(APPEND _header_units "${_unit}")
endforeach ()

add_library(rex-header-check OBJECT ${_header_units})
target_link_libraries(
  rex-header-check
  PRIVATE rexcore
          rexruntime
          reximgui
          rexgpu
          rexgpu-xenos
          rexcodegen
          rexui
          rexhid-common
)
target_include_directories(
  rex-header-check PRIVATE $<TARGET_PROPERTY:rexfilesystem,INTERFACE_INCLUDE_DIRECTORIES>
)
target_compile_definitions(
  rex-header-check PRIVATE $<TARGET_PROPERTY:rexfilesystem,INTERFACE_COMPILE_DEFINITIONS>
)
