# Fails when raw guest memory access appears outside the memory implementation and what recompiled
# code calls. Run with cmake -DREXGLUE_ROOT=<root> -P.
function (rexglue_audit_raw_memory)
  set(raw_names
      TranslateVirtual
      TranslatePhysical
      TranslateRelative
      GetVirtualMembase
      GetPhysicalMembase
      HostToGuestVirtual
      HostPtrToGuest
      "GuestPtr[<(]"
  )
  list(JOIN raw_names "|" raw_pattern)
  set(allowed_files src/kernel/memory.cpp src/kernel/guest_memory.cpp include/rex/kernel/heap.h
                    include/rex/ppc/hook.h
  )
  set(allowed_folders include/rex/ppc/)

  file(
    GLOB_RECURSE files
    RELATIVE "${REXGLUE_ROOT}"
    "${REXGLUE_ROOT}/src/*.cpp" "${REXGLUE_ROOT}/src/*.h" "${REXGLUE_ROOT}/include/*.h"
  )

  set(violations "")
  set(site_count 0)
  foreach (file IN LISTS files)
    if (file IN_LIST allowed_files)
      continue()
    endif ()
    set(skip FALSE)
    foreach (folder IN LISTS allowed_folders)
      if (file MATCHES "^${folder}")
        set(skip TRUE)
      endif ()
    endforeach ()
    if (skip)
      continue()
    endif ()
    file(STRINGS "${REXGLUE_ROOT}/${file}" lines)
    set(line_number 0)
    foreach (line IN LISTS lines)
      math(EXPR line_number "${line_number} + 1")
      if (line MATCHES "${raw_pattern}")
        list(APPEND violations "${file}:${line_number}")
        math(EXPR site_count "${site_count} + 1")
      endif ()
    endforeach ()
  endforeach ()

  if (violations)
    list(JOIN violations "\n  " report)
    message(FATAL_ERROR "raw memory audit: ${site_count} raw guest memory sites\n  ${report}")
  endif ()
endfunction ()

rexglue_audit_raw_memory()
