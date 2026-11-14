# rexglue_set_folder_recursive(<directory> <folder> [OBJECT_LIBRARIES_ONLY])
# sets the FOLDER of every target that a directory and its subdirectories created, replacing a
# FOLDER the subproject chose itself. With OBJECT_LIBRARIES_ONLY it leaves the other targets alone.
function (rexglue_set_folder_recursive directory folder)
  cmake_parse_arguments(PARSE_ARGV 2 ARG "OBJECT_LIBRARIES_ONLY" "" "")
  get_property(
    targets
    DIRECTORY "${directory}"
    PROPERTY BUILDSYSTEM_TARGETS
  )
  foreach (target IN LISTS targets)
    get_property(
      type
      TARGET ${target}
      PROPERTY TYPE
    )
    if (NOT ARG_OBJECT_LIBRARIES_ONLY OR type STREQUAL "OBJECT_LIBRARY")
      set_property(TARGET ${target} PROPERTY FOLDER "${folder}")
    endif ()
  endforeach ()
  get_property(
    subdirectories
    DIRECTORY "${directory}"
    PROPERTY SUBDIRECTORIES
  )
  foreach (subdirectory IN LISTS subdirectories)
    if (ARG_OBJECT_LIBRARIES_ONLY)
      rexglue_set_folder_recursive("${subdirectory}" "${folder}" OBJECT_LIBRARIES_ONLY)
    else ()
      rexglue_set_folder_recursive("${subdirectory}" "${folder}")
    endif ()
  endforeach ()
endfunction ()

# rexglue_group_object_libraries() puts every OBJECT library of the SDK sources, the <lib>-objects
# targets below src, into the sdk/objects folder.
function (rexglue_group_object_libraries)
  set(src_directory "${REXGLUE_ROOT}/src")
  get_property(
    subdirectories
    DIRECTORY "${REXGLUE_ROOT}"
    PROPERTY SUBDIRECTORIES
  )
  foreach (subdirectory IN LISTS subdirectories)
    cmake_path(IS_PREFIX src_directory "${subdirectory}" NORMALIZE in_src)
    if (in_src)
      rexglue_set_folder_recursive("${subdirectory}" "sdk/objects" OBJECT_LIBRARIES_ONLY)
    endif ()
  endforeach ()
endfunction ()
