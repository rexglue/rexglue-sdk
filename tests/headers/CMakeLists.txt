file(GLOB_RECURSE header_paths RELATIVE "${REXGLUE_ROOT}/include" CONFIGURE_DEPENDS
    "${REXGLUE_ROOT}/include/rex/*.h")
if(NOT WIN32)
    list(FILTER header_paths EXCLUDE REGEX "_win\\.h$")
endif()
if(NOT APPLE)
    list(FILTER header_paths EXCLUDE REGEX "_mac\\.h$")
endif()
if(NOT (UNIX AND NOT APPLE))
    list(FILTER header_paths EXCLUDE REGEX "_gnulinux\\.h$")
endif()

set(header_units "")
foreach(header IN LISTS header_paths)
    set(unit "${CMAKE_CURRENT_BINARY_DIR}/${header}.cpp")
    file(CONFIGURE OUTPUT "${unit}" CONTENT "#include <${header}>\n" @ONLY)
    list(APPEND header_units "${unit}")
endforeach()

add_library(rex-header-check OBJECT ${header_units})
target_link_libraries(rex-header-check PRIVATE
    rex-core rex-runtime rex-imgui rex-gpu rex-gpu-xenos rex-codegen rex-ui rex-hid-common)
target_include_directories(rex-header-check PRIVATE
    $<TARGET_PROPERTY:rex-filesystem,INTERFACE_INCLUDE_DIRECTORIES>)
target_compile_definitions(rex-header-check PRIVATE
    $<TARGET_PROPERTY:rex-filesystem,INTERFACE_COMPILE_DEFINITIONS>)
