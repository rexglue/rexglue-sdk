# Fails when a recompiled code header reaches the kernel or UI through its includes.
# Run with cmake -DREXGLUE_ROOT=<root> -P ppc_header_isolation.cmake

cmake_minimum_required(VERSION 3.25)

set(_include_dir "${REXGLUE_ROOT}/include")
file(
  GLOB _ppc_headers
  RELATIVE "${_include_dir}"
  "${_include_dir}/rex/ppc/*.h"
)
set(_roots ${_ppc_headers})
set(_forbidden "^rex/(kernel|ui)/")
set(_failures "")

foreach (_root IN LISTS _roots)
  set(_queue "${_root}")
  set(_seen "${_root}")
  while (_queue)
    list(POP_FRONT _queue _header)
    if (NOT EXISTS "${_include_dir}/${_header}")
      continue()
    endif ()
    file(STRINGS "${_include_dir}/${_header}" _lines REGEX "^[ \t]*#[ \t]*include[ \t]*<rex/[^>]+>")
    foreach (_line IN LISTS _lines)
      string(REGEX REPLACE "^[ \t]*#[ \t]*include[ \t]*<([^>]+)>.*$" "\\1" _included "${_line}")
      if (_included IN_LIST _seen)
        continue()
      endif ()
      list(APPEND _seen "${_included}")
      list(APPEND _queue "${_included}")
      if (_included MATCHES "${_forbidden}")
        list(APPEND _failures "${_root} reaches ${_included} through ${_header}")
      endif ()
    endforeach ()
  endwhile ()
endforeach ()

if (_failures)
  list(JOIN _failures "\n  " _message)
  message(FATAL_ERROR "Recompiled code headers include the kernel or UI:\n  ${_message}")
endif ()
