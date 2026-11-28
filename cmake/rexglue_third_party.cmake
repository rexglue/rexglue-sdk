# Fails the configure when a folder or submodule in thirdparty has no row in the license table of
# docs/site/pages/appendix/third-party.md, so a new dependency cannot be added without its license.
function (rexglue_check_third_party_rows)
  set(page "${PROJECT_SOURCE_DIR}/docs/site/pages/appendix/third-party.md")
  set_property(
    DIRECTORY
    APPEND
    PROPERTY CMAKE_CONFIGURE_DEPENDS "${page}" "${PROJECT_SOURCE_DIR}/.gitmodules"
  )
  file(STRINGS "${page}" lines ENCODING UTF-8)
  set(rows "")
  set(in_table FALSE)
  foreach (line IN LISTS lines)
    if (line MATCHES "^## ")
      set(in_table FALSE)
      if (line MATCHES "^## Folders in `thirdparty`$")
        set(in_table TRUE)
      endif ()
    elseif (in_table AND line MATCHES "^\\| ([^ |]+) \\|")
      list(APPEND rows "${CMAKE_MATCH_1}")
    endif ()
  endforeach ()

  file(
    GLOB entries
    LIST_DIRECTORIES TRUE
    RELATIVE "${PROJECT_SOURCE_DIR}/thirdparty"
    "${PROJECT_SOURCE_DIR}/thirdparty/*"
  )
  set(folders "")
  foreach (entry IN LISTS entries)
    if (IS_DIRECTORY "${PROJECT_SOURCE_DIR}/thirdparty/${entry}")
      list(APPEND folders "${entry}")
    endif ()
  endforeach ()
  file(STRINGS "${PROJECT_SOURCE_DIR}/.gitmodules" module_lines REGEX "^[ \t]*path = thirdparty/")
  foreach (line IN LISTS module_lines)
    string(REGEX REPLACE "^[ \t]*path = thirdparty/" "" module "${line}")
    list(APPEND folders "${module}")
  endforeach ()
  list(REMOVE_DUPLICATES folders)

  set(missing "")
  foreach (folder IN LISTS folders)
    if (NOT folder IN_LIST rows)
      list(APPEND missing "  thirdparty/${folder}")
    endif ()
  endforeach ()
  if (missing)
    list(JOIN missing "\n" detail)
    message(FATAL_ERROR "third party: these folders have no row in ${page}\n${detail}")
  endif ()
endfunction ()

rexglue_check_third_party_rows()
