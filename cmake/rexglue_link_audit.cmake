set(REXGLUE_RUNTIME_FOLDED_LIBS "")
set(REXGLUE_FOLDED_LIB_USERS rexui-sdl3 unit_tests)
set(REXGLUE_ABOVE_CORE_TARGETS
    rexkernel
    rexxam
    rexxbdm
    rexapp
    rexppc
    rexxenos
    rexfilesystem
    rexapu
    rexhid
    rexui
    rexui-sdl3
    rexgpu
    rexgpu-xenos-null
    rexgpu-xenos-vulkan
    rexgpu-xenos-d3d12
    rexcodegen
    rexapu-sdl3
    rexhid-sdl3
    rexhid-xinput
)

# Collects every build system target under a directory, subdirectories included.
function (_rexglue_collect_targets dir out_var)
  get_property(
    targets
    DIRECTORY "${dir}"
    PROPERTY BUILDSYSTEM_TARGETS
  )
  get_property(
    subdirs
    DIRECTORY "${dir}"
    PROPERTY SUBDIRECTORIES
  )
  foreach (subdir IN LISTS subdirs)
    _rexglue_collect_targets("${subdir}" sub_targets)
    list(APPEND targets ${sub_targets})
  endforeach ()
  set(${out_var}
      "${targets}"
      PARENT_SCOPE
  )
endfunction ()

# Resolves an alias to the target it names.
function (_rexglue_resolve_alias name out_var)
  if (TARGET "${name}")
    get_target_property(aliased "${name}" ALIASED_TARGET)
    if (aliased)
      set(${out_var}
          "${aliased}"
          PARENT_SCOPE
      )
      return()
    endif ()
  endif ()
  set(${out_var}
      "${name}"
      PARENT_SCOPE
  )
endfunction ()

# Rejects a target that links rexkernel and an OBJECT library carrying rexbase objects.
function (rexglue_audit_core_linkage)
  _rexglue_collect_targets("${CMAKE_CURRENT_SOURCE_DIR}" all_targets)

  set(violations "")
  foreach (target_name IN LISTS all_targets)
    if (target_name STREQUAL "rexkernel" OR target_name IN_LIST REXGLUE_FOLDED_LIB_USERS)
      continue()
    endif ()

    get_target_property(type ${target_name} TYPE)
    if (NOT type MATCHES "^(EXECUTABLE|SHARED_LIBRARY|MODULE_LIBRARY)$")
      continue()
    endif ()

    get_target_property(links ${target_name} LINK_LIBRARIES)
    if (NOT links)
      continue()
    endif ()
    set(resolved "")
    foreach (lib IN LISTS links)
      _rexglue_resolve_alias("${lib}" real)
      list(APPEND resolved "${real}")
    endforeach ()
    if (NOT "rexkernel" IN_LIST resolved)
      continue()
    endif ()

    foreach (lib IN LISTS REXGLUE_RUNTIME_FOLDED_LIBS)
      if ("${lib}" IN_LIST resolved)
        list(APPEND violations "  ${target_name} links rexkernel and ${lib}")
      endif ()
    endforeach ()
  endforeach ()

  if (violations)
    list(JOIN violations "\n" detail)
    message(FATAL_ERROR "rexbase objects would be duplicated:\n${detail}\n"
                        "rexkernel contains them. Drop the direct link."
    )
  endif ()
endfunction ()

# Rejects an SDK target above rexbase in the rexbase link list.
function (rexglue_audit_core_tier)
  get_target_property(links rexbase LINK_LIBRARIES)
  set(violations "")
  foreach (lib IN LISTS links)
    _rexglue_resolve_alias("${lib}" real)
    if ("${real}" IN_LIST REXGLUE_ABOVE_CORE_TARGETS)
      list(APPEND violations "  rexbase links ${real}")
    endif ()
  endforeach ()
  if (violations)
    list(JOIN violations "
" detail
    )
    message(FATAL_ERROR "rexbase is tier 0 and links no SDK target:
${detail}"
    )
  endif ()
endfunction ()

# Rejects a target that links any of the given libraries.
function (_rexglue_audit_forbidden target_name)
  if (NOT TARGET ${target_name})
    return()
  endif ()
  get_target_property(links ${target_name} LINK_LIBRARIES)
  get_target_property(interface_links ${target_name} INTERFACE_LINK_LIBRARIES)
  foreach (link IN LISTS links interface_links)
    _rexglue_resolve_alias("${link}" real)
    if ("${link}" IN_LIST ARGN OR "${real}" IN_LIST ARGN)
      message(FATAL_ERROR "link audit: ${target_name} links ${link}")
    endif ()
  endforeach ()
endfunction ()

# Keeps SDL and ImGui out of core and runtime, and SDL, ImGui and the GPU drivers out of ui.
# rexglue_audit_host_service_links covers rexgpu.
function (rexglue_audit_ui_links)
  set(sdl_libs SDL3::SDL3 SDL3::SDL3-shared SDL3::SDL3-static)
  set(imgui_libs imgui imgui::imgui)
  set(driver_libs
      rexgpu-xenos-vulkan
      rexgpu-xenos-d3d12
      d3d12
      dxgi
      dxguid
      Vulkan::Headers
      dxc-headers
  )
  _rexglue_audit_forbidden(rexbase ${sdl_libs})
  _rexglue_audit_forbidden(rexkernel ${sdl_libs} ${imgui_libs})
endfunction ()

rexglue_audit_core_linkage()
rexglue_audit_core_tier()
rexglue_audit_ui_links()

# Keeps host libraries out of the neutral targets and out of rexkernel.
function (rexglue_audit_host_service_links)
  set(host_libs
      SDL3::SDL3
      SDL3::SDL3-shared
      SDL3::SDL3-static
      xinput
      d3d12
      dxgi
      dxguid
      Vulkan::Headers
  )
  foreach (target_name IN ITEMS rexgpu)
    if (NOT TARGET ${target_name})
      continue()
    endif ()
    get_target_property(links ${target_name} LINK_LIBRARIES)
    get_target_property(interface_links ${target_name} INTERFACE_LINK_LIBRARIES)
    foreach (link IN LISTS links interface_links)
      if (link IN_LIST host_libs)
        message(FATAL_ERROR "link audit: ${target_name} links the host library ${link}")
      endif ()
    endforeach ()
  endforeach ()
  set(ui_sdl3_allowed "rexui-sdl3=rexbase|SDL3::SDL3")
  if (APPLE)
    string(APPEND ui_sdl3_allowed "|-framework CoreFoundation")
  endif ()
  set(module_allowed
      "rexfilesystem=rexbase|excrypt|mspack"
      "rexapu=rexbase"
      "rexhid=rexbase"
      "rexui=rexbase"
      "rexxbdm=rexbase|rexppc|simde"
      "rexapu-sdl3=rexbase|SDL3::SDL3"
      "rexhid-sdl3=rexbase|SDL3::SDL3"
      "rexhid-xinput=rexbase"
      "${ui_sdl3_allowed}"
  )
  foreach (entry IN LISTS module_allowed)
    string(REPLACE "=" ";" pair "${entry}")
    list(GET pair 0 module)
    list(GET pair 1 allowed)
    string(REPLACE "|" ";" allowed "${allowed}")
    if (NOT TARGET ${module})
      continue()
    endif ()
    get_target_property(links ${module} LINK_LIBRARIES)
    foreach (link IN LISTS links)
      if (NOT link IN_LIST allowed)
        message(FATAL_ERROR "link audit: ${module} links ${link}, allowed are ${allowed}")
      endif ()
    endforeach ()
  endforeach ()
  get_target_property(runtime_links rexkernel LINK_LIBRARIES)
  foreach (link IN LISTS runtime_links)
    if (link MATCHES "^rex(apu|hid|ui)-")
      message(FATAL_ERROR "link audit: rexkernel links the host module ${link}")
    endif ()
  endforeach ()
endfunction ()

rexglue_audit_host_service_links()

# Rejects an imgui OBJECT target and a rexkernel link to it.
function (rexglue_audit_imgui_links)
  if (TARGET imgui)
    message(FATAL_ERROR "link audit: the imgui OBJECT target exists, rexbase builds ImGui")
  endif ()
  get_target_property(runtime_links rexkernel LINK_LIBRARIES)
  foreach (link IN LISTS runtime_links)
    _rexglue_resolve_alias("${link}" real)
    if ("${real}" STREQUAL "imgui")
      message(FATAL_ERROR "link audit: rexkernel links the imgui OBJECT")
    endif ()
  endforeach ()
endfunction ()

rexglue_audit_imgui_links()

# Rejects an SDK target that exports every symbol or lacks hidden visibility.
function (rexglue_audit_export_visibility)
  _rexglue_collect_targets("${CMAKE_CURRENT_SOURCE_DIR}" all_targets)
  set(violations "")
  foreach (target_name IN LISTS all_targets)
    get_target_property(source_dir ${target_name} SOURCE_DIR)
    cmake_path(IS_PREFIX PROJECT_SOURCE_DIR "${source_dir}" NORMALIZE in_tree)
    string(FIND "${source_dir}" "${PROJECT_SOURCE_DIR}/thirdparty" thirdparty_pos)
    if (NOT in_tree OR thirdparty_pos EQUAL 0)
      continue()
    endif ()
    get_target_property(export_all ${target_name} WINDOWS_EXPORT_ALL_SYMBOLS)
    if (export_all)
      list(APPEND violations "  ${target_name} sets WINDOWS_EXPORT_ALL_SYMBOLS")
    endif ()
    get_target_property(type ${target_name} TYPE)
    if (type MATCHES "^(SHARED_LIBRARY|MODULE_LIBRARY)$" AND target_name MATCHES "^rex")
      get_target_property(visibility ${target_name} CXX_VISIBILITY_PRESET)
      if (NOT visibility STREQUAL "hidden")
        list(APPEND violations "  ${target_name} lacks CXX_VISIBILITY_PRESET hidden")
      endif ()
    endif ()
  endforeach ()
  if (violations)
    list(JOIN violations "\n" detail)
    message(FATAL_ERROR "SDK binaries export only what they mark:\n${detail}")
  endif ()
endfunction ()

rexglue_audit_export_visibility()

# Requires the host allocator forwarders in every executable and shared or module library the SDK
# builds, since a binary without them frees blocks of other binaries with the CRT.
function (rexglue_audit_host_allocator)
  # This one models a library that is no module of the SDK, and has no forwarders.
  set(allocator_exempt rexmod-test-nomodule)
  _rexglue_collect_targets("${PROJECT_SOURCE_DIR}" all_targets)
  _rexglue_collect_targets("${PROJECT_SOURCE_DIR}/thirdparty" third_party)
  set(missing "")
  foreach (target_name IN LISTS all_targets)
    if (target_name IN_LIST third_party OR target_name IN_LIST allocator_exempt)
      continue()
    endif ()
    get_target_property(target_type ${target_name} TYPE)
    if (NOT target_type MATCHES "^(EXECUTABLE|SHARED_LIBRARY|MODULE_LIBRARY)$")
      continue()
    endif ()
    get_target_property(sources ${target_name} SOURCES)
    set(has_stub FALSE)
    foreach (source IN LISTS sources)
      if (source MATCHES "rexglue_host_alloc_stub\.cpp$")
        set(has_stub TRUE)
      endif ()
    endforeach ()
    if (NOT has_stub)
      list(APPEND missing ${target_name})
    endif ()
  endforeach ()
  if (missing)
    list(JOIN missing " " missing_text)
    message(FATAL_ERROR "link audit: missing rexglue_link_host_allocator on ${missing_text}")
  endif ()
endfunction ()

rexglue_audit_host_allocator()
