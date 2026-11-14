# Decides how the 0xE0000000 view reaches physical memory from 0x1000. The memory mapping offset
# has to be a multiple of the MMU page size. A 4 KiB page host maps the offset. A host with larger
# pages starts the view at physical 0 and recompiled code adds 0x1000 to each access.
#
# Sets REX_PHYSICAL_OFFSET_IN_CODE_VALUE to 1 or 0 for the compile definition on rexbase.
#
# Windows maps the view through placeholders, which only need a page aligned offset. macOS on
# arm64 has 16 KiB pages, so it always adds the offset in code. Linux on aarch64 depends on the
# kernel page size, so it is the option REX_PHYSICAL_OFFSET_IN_CODE. Every other host maps.
set(_rex_offset_arm64 FALSE)
if (APPLE AND CMAKE_OSX_ARCHITECTURES)
  if ("arm64" IN_LIST CMAKE_OSX_ARCHITECTURES)
    set(_rex_offset_arm64 TRUE)
  endif ()
elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|ARM64|arm64)$")
  set(_rex_offset_arm64 TRUE)
endif ()

if (APPLE AND _rex_offset_arm64)
  set(REX_PHYSICAL_OFFSET_IN_CODE ON)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND _rex_offset_arm64)
  set(_rex_offset_default OFF)
  if (NOT CMAKE_CROSSCOMPILING)
    execute_process(
      COMMAND getconf PAGESIZE
      OUTPUT_VARIABLE _rex_host_page_size
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE _rex_page_size_result
    )
    if (_rex_page_size_result EQUAL 0 AND _rex_host_page_size GREATER 4096)
      set(_rex_offset_default ON)
    endif ()
  endif ()
  option(REX_PHYSICAL_OFFSET_IN_CODE
         "Add the 0x1000 offset of the 0xE0000000 view in code, for hosts with pages above 4 KiB"
         ${_rex_offset_default}
  )
else ()
  set(REX_PHYSICAL_OFFSET_IN_CODE OFF)
endif ()

if (REX_PHYSICAL_OFFSET_IN_CODE)
  set(REX_PHYSICAL_OFFSET_IN_CODE_VALUE 1)
else ()
  set(REX_PHYSICAL_OFFSET_IN_CODE_VALUE 0)
endif ()
