set(REXGLUE_RUNTIME_FOLDED_LIBS rexui)
set(REXGLUE_FOLDED_LIB_USERS rexui-sdl3 unit_tests)
set(REXGLUE_ABOVE_CORE_TARGETS
    rexruntime
    rexfilesystem
    reximgui
    rexui
    rexui-sdl3
    rexgpu
    rexgpu-xenos
    rexgpu-xenos-vulkan
    rexgpu-xenos-d3d12
    rexcodegen
    rexapu-sdl3
    rexhid
    rexhid-sdl3
    rexhid-xinput
)
set(REXGLUE_REGISTRAR_REGEX "REXCVAR_DEFINE_|REXLOG_DEFINE_CATEGORY|REXLOG_DEFINE_SUBCATEGORY")

function(_rexglue_collect_targets dir out_var)
    get_property(targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    get_property(subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(subdir IN LISTS subdirs)
        _rexglue_collect_targets("${subdir}" sub_targets)
        list(APPEND targets ${sub_targets})
    endforeach()
    set(${out_var} "${targets}" PARENT_SCOPE)
endfunction()

function(_rexglue_resolve_alias name out_var)
    if(TARGET "${name}")
        get_target_property(aliased "${name}" ALIASED_TARGET)
        if(aliased)
            set(${out_var} "${aliased}" PARENT_SCOPE)
            return()
        endif()
    endif()
    set(${out_var} "${name}" PARENT_SCOPE)
endfunction()

function(rexglue_audit_core_linkage)
    _rexglue_collect_targets("${CMAKE_CURRENT_SOURCE_DIR}" all_targets)

    set(violations "")
    foreach(target IN LISTS all_targets)
        if(target STREQUAL "rexruntime" OR target IN_LIST REXGLUE_FOLDED_LIB_USERS)
            continue()
        endif()

        get_target_property(type ${target} TYPE)
        if(NOT type MATCHES "^(EXECUTABLE|SHARED_LIBRARY|MODULE_LIBRARY)$")
            continue()
        endif()

        get_target_property(links ${target} LINK_LIBRARIES)
        if(NOT links)
            continue()
        endif()
        set(resolved "")
        foreach(lib IN LISTS links)
            _rexglue_resolve_alias("${lib}" real)
            list(APPEND resolved "${real}")
        endforeach()
        if(NOT "rexruntime" IN_LIST resolved)
            continue()
        endif()

        foreach(lib IN LISTS REXGLUE_RUNTIME_FOLDED_LIBS)
            if("${lib}" IN_LIST resolved)
                list(APPEND violations "  ${target} links rexruntime and ${lib}")
            endif()
        endforeach()
    endforeach()

    if(violations)
        list(JOIN violations "\n" detail)
        message(FATAL_ERROR
            "rexcore objects would be duplicated:\n${detail}\n"
            "rexruntime already contains them. Drop the direct link.")
    endif()
endfunction()

function(rexglue_audit_module_registrars)
    _rexglue_collect_targets("${CMAKE_CURRENT_SOURCE_DIR}" all_targets)
    set(violations "")
    foreach(target IN LISTS all_targets)
        get_target_property(type ${target} TYPE)
        if(NOT type MATCHES "^(SHARED_LIBRARY|MODULE_LIBRARY)$")
            continue()
        endif()
        get_target_property(sources ${target} SOURCES)
        get_target_property(source_dir ${target} SOURCE_DIR)
        foreach(source IN LISTS sources)
            if(NOT source MATCHES "\\.(cpp|cc|cxx|h|inc)$")
                continue()
            endif()
            if(IS_ABSOLUTE "${source}")
                set(full "${source}")
            else()
                set(full "${source_dir}/${source}")
            endif()
            if(NOT EXISTS "${full}")
                continue()
            endif()
            file(STRINGS "${full}" hits REGEX "${REXGLUE_REGISTRAR_REGEX}")
            if(NOT hits)
                continue()
            endif()
            file(RELATIVE_PATH rel "${PROJECT_SOURCE_DIR}" "${full}")
            list(APPEND violations "  ${target}:${rel}")
        endforeach()
    endforeach()

    if(violations)
        list(JOIN violations "\n" detail)
        message(FATAL_ERROR
            "static registrars in module DLLs (charter 4.7):\n${detail}\n"
            "Register at Connect instead.")
    endif()
endfunction()

function(rexglue_audit_core_tier)
    get_target_property(links rexcore LINK_LIBRARIES)
    set(violations "")
    foreach(lib IN LISTS links)
        _rexglue_resolve_alias("${lib}" real)
        if("${real}" IN_LIST REXGLUE_ABOVE_CORE_TARGETS)
            list(APPEND violations "  rexcore links ${real}")
        endif()
    endforeach()
    if(violations)
        list(JOIN violations "
" detail)
        message(FATAL_ERROR "rexcore is tier 0 and links no SDK target (charter 2.2):
${detail}")
    endif()
endfunction()

function(rexglue_audit_module_links)
    _rexglue_collect_targets("${CMAKE_CURRENT_SOURCE_DIR}" all_targets)
    set(violations "")
    foreach(target IN LISTS all_targets)
        get_target_property(links ${target} LINK_LIBRARIES)
        if(NOT links)
            continue()
        endif()
        foreach(lib IN LISTS links)
            _rexglue_resolve_alias("${lib}" real)
            if("${real}" STREQUAL "rexfilesystem")
                list(APPEND violations "  ${target} links rexfilesystem")
            endif()
        endforeach()
    endforeach()
    if(violations)
        list(JOIN violations "\n" detail)
        message(FATAL_ERROR
            "rexfilesystem is a module loaded through rex::Init and is never linked (charter 4):\n"
            "${detail}")
    endif()
endfunction()

function(_rexglue_audit_forbidden target)
    if(NOT TARGET ${target})
        return()
    endif()
    get_target_property(links ${target} LINK_LIBRARIES)
    get_target_property(interface_links ${target} INTERFACE_LINK_LIBRARIES)
    foreach(link IN LISTS links interface_links)
        _rexglue_resolve_alias("${link}" real)
        if("${link}" IN_LIST ARGN OR "${real}" IN_LIST ARGN)
            message(FATAL_ERROR "link audit: ${target} links ${link}")
        endif()
    endforeach()
endfunction()

function(rexglue_audit_ui_links)
    set(sdl_libs SDL3::SDL3 SDL3::SDL3-shared SDL3::SDL3-static)
    set(imgui_libs imgui imgui::imgui reximgui)
    set(backend_libs rexgpu-xenos rexgpu-xenos-vulkan rexgpu-xenos-d3d12 d3d12 dxgi dxguid Vulkan::Headers
        GPUOpen::VulkanMemoryAllocator spirv-tools-headers dxc-headers)
    _rexglue_audit_forbidden(rexcore ${sdl_libs} ${imgui_libs})
    _rexglue_audit_forbidden(rexruntime ${sdl_libs} ${imgui_libs})
    _rexglue_audit_forbidden(rexui ${sdl_libs} ${imgui_libs} ${backend_libs})
    _rexglue_audit_forbidden(rexgpu ${sdl_libs})
endfunction()

rexglue_audit_core_linkage()
rexglue_audit_core_tier()
rexglue_audit_module_registrars()
rexglue_audit_module_links()
rexglue_audit_ui_links()

# Keeps host libraries out of the neutral targets and out of rexruntime.
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
      GPUOpen::VulkanMemoryAllocator
      reximgui
      rex::imgui
  )
  foreach (target_name IN ITEMS rexhid rexgpu)
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
  set(ui_sdl3_allowed "rexui-sdl3=rexcore|rexui|rexgpu|reximgui|rexruntime|SDL3::SDL3")
  if (APPLE)
    string(APPEND ui_sdl3_allowed "|-framework CoreFoundation")
  endif ()
  set(module_allowed "rexapu-sdl3=rexcore|SDL3::SDL3" "rexhid-sdl3=rexcore|rexhid|SDL3::SDL3"
                     "rexhid-xinput=rexcore|rexhid" "reximgui=rexcore" "${ui_sdl3_allowed}"
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
  get_target_property(runtime_links rexruntime LINK_LIBRARIES)
  foreach (link IN LISTS runtime_links)
    if (link MATCHES "^rex(apu|hid|ui)-")
      message(FATAL_ERROR "link audit: rexruntime links the host module ${link}")
    endif ()
  endforeach ()
endfunction ()

rexglue_audit_host_service_links()

function(rexglue_audit_imgui_links)
    if(TARGET imgui)
        message(FATAL_ERROR "link audit: the imgui OBJECT target exists, reximgui builds ImGui")
    endif()
    get_target_property(runtime_links rexruntime LINK_LIBRARIES)
    foreach(link IN LISTS runtime_links)
        _rexglue_resolve_alias("${link}" real)
        if("${real}" STREQUAL "imgui")
            message(FATAL_ERROR "link audit: rexruntime links the imgui OBJECT")
        endif()
    endforeach()
endfunction()

rexglue_audit_imgui_links()

function(rexglue_audit_export_visibility)
    _rexglue_collect_targets("${CMAKE_CURRENT_SOURCE_DIR}" all_targets)
    set(violations "")
    foreach(target IN LISTS all_targets)
        get_target_property(source_dir ${target} SOURCE_DIR)
        cmake_path(IS_PREFIX PROJECT_SOURCE_DIR "${source_dir}" NORMALIZE in_tree)
        string(FIND "${source_dir}" "${PROJECT_SOURCE_DIR}/thirdparty" third_party_pos)
        if(NOT in_tree OR third_party_pos EQUAL 0)
            continue()
        endif()
        get_target_property(export_all ${target} WINDOWS_EXPORT_ALL_SYMBOLS)
        if(export_all)
            list(APPEND violations "  ${target} sets WINDOWS_EXPORT_ALL_SYMBOLS")
        endif()
        get_target_property(type ${target} TYPE)
        if(type MATCHES "^(SHARED_LIBRARY|MODULE_LIBRARY)$" AND target MATCHES "^rex")
            get_target_property(visibility ${target} CXX_VISIBILITY_PRESET)
            if(NOT visibility STREQUAL "hidden")
                list(APPEND violations "  ${target} lacks CXX_VISIBILITY_PRESET hidden")
            endif()
        endif()
    endforeach()
    if(violations)
        list(JOIN violations "\n" detail)
        message(FATAL_ERROR "SDK binaries export only what they mark (charter 4.8, 7.11):\n${detail}")
    endif()
endfunction()

rexglue_audit_export_visibility()
