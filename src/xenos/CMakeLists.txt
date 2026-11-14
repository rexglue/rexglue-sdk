# rexxenos
add_library(
  rexxenos STATIC
  format.cpp
  register.cpp
  register_file.cpp
  ucode.cpp
  texture_info.cpp
  texture_info_format.cpp
  texture_extent.cpp
  texture_layout.cpp
  texture_conversion.cpp
)
add_library(rex::xenos ALIAS rexxenos)
rex_target_headers(rexxenos DIRS xenos)
set_target_properties(
  rexxenos
  PROPERTIES EXPORT_NAME xenos
             CXX_VISIBILITY_PRESET hidden
             VISIBILITY_INLINES_HIDDEN ON
             POSITION_INDEPENDENT_CODE ON
)
target_include_directories(
  rexxenos PRIVATE ${PROJECT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/thirdparty
)
rex_target_pch(rexxenos)
target_link_libraries(
  rexxenos
  PUBLIC rexbase
  PRIVATE $<BUILD_INTERFACE:xxHash::xxhash>
)
