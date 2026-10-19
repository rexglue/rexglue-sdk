#==========================================================
# rexglue_target.cmake
#
#   rexglue_apply_target_settings(<target>)  - common compile/platform flags
#   rexglue_configure_target(<target>)       - host application
#==========================================================
include_guard(GLOBAL)

# Stages the pinned Vulkan loader and MoltenVK with their ICD file next to the host.
function (_rexglue_stage_macos_vulkan_runtime target_name)
  if (TARGET Vulkan::Loader AND TARGET MoltenVK::MoltenVK)
    set(vulkan_loader Vulkan::Loader)
    set(moltenvk MoltenVK::MoltenVK)
    set(moltenvk_icd "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/MoltenVK_icd.json")
  elseif (TARGET rex::vulkan-loader AND TARGET rex::moltenvk)
    set(vulkan_loader rex::vulkan-loader)
    set(moltenvk rex::moltenvk)
    set(moltenvk_icd "${REXGLUE_MOLTENVK_ICD}")
  else ()
    message(FATAL_ERROR "rexglue: pinned macOS Vulkan runtime targets are unavailable")
  endif ()

  add_custom_command(
    TARGET ${target_name}
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target_name}>/vulkan/lib"
            "$<TARGET_FILE_DIR:${target_name}>/vulkan/share/vulkan/icd.d"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${vulkan_loader}>"
            "$<TARGET_FILE_DIR:${target_name}>/vulkan/lib/libvulkan.1.dylib"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${moltenvk}>"
            "$<TARGET_FILE_DIR:${target_name}>/vulkan/lib/libMoltenVK.dylib"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${moltenvk_icd}"
            "$<TARGET_FILE_DIR:${target_name}>/vulkan/share/vulkan/icd.d/MoltenVK_icd.json"
    VERBATIM
  )
endfunction ()

#==========================================================
# rexglue_apply_target_settings(<target>) - Common flags
#
# Applied to both host apps and title DLL modules. Compile/link flags only.
# Runtime DLL staging is the host's job (see rexglue_configure_target).
#==========================================================
function (rexglue_apply_target_settings target_name)
  if (UNIX AND NOT APPLE)
    # Large executable support
    if (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
      target_link_options(${target_name} PRIVATE -Wl,--no-relax)
      target_compile_options(${target_name} PRIVATE -mcmodel=large)
    elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64")
      target_compile_options(${target_name} PRIVATE -march=armv8-a)
    endif ()
  endif ()

  if (NOT MSVC)
    if (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
      target_compile_options(${target_name} PRIVATE -msse4.1)
    endif ()
  endif ()
endfunction ()

#==========================================================
# Copies a library next to the host and keeps the host relinking when it changes.
#==========================================================
function (_rexglue_stage_library target_name library)
  add_custom_command(
    TARGET ${target_name}
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:${library}>
            $<TARGET_FILE_DIR:${target_name}>
    VERBATIM
  )
  if (WIN32)
    add_custom_command(
      TARGET ${target_name}
      POST_BUILD
      COMMAND ${CMAKE_COMMAND} -DDEST=$<TARGET_FILE_DIR:${target_name}> -P
              ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/rexglue_stage_pdbs.cmake $<TARGET_FILE:${library}>
      VERBATIM
    )
  endif ()
  set_property(
    TARGET ${target_name}
    APPEND
    PROPERTY LINK_DEPENDS "$<TARGET_FILE:${library}>"
  )
endfunction ()

#==========================================================
# Windows host. Stages the runtime DLLs and their PDBs on every link. The host
# relinks when a staged DLL changes, since an unchanged import library skips the
# link otherwise.
#==========================================================
function (_rexglue_stage_runtime_dlls target_name)
  set(dlls "$<TARGET_RUNTIME_DLLS:${target_name}>")
  set(dll_dir "$<TARGET_FILE_DIR:${target_name}>")
  set(stage_pdbs "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/rexglue_stage_pdbs.cmake")
  add_custom_command(
    TARGET ${target_name}
    POST_BUILD
    COMMAND "$<$<BOOL:${dlls}>:${CMAKE_COMMAND};-E;copy_if_different;${dlls};${dll_dir}>"
    COMMAND_EXPAND_LISTS VERBATIM
  )
  set_property(
    TARGET ${target_name}
    APPEND
    PROPERTY LINK_DEPENDS "${dlls}"
  )
  add_custom_command(
    TARGET ${target_name}
    POST_BUILD
    COMMAND "$<$<BOOL:${dlls}>:${CMAKE_COMMAND};-DDEST=${dll_dir};-P;${stage_pdbs};${dlls}>"
    COMMAND_EXPAND_LISTS VERBATIM
  )
endfunction ()

#==========================================================
# macOS and Linux host. $<TARGET_RUNTIME_DLLS> does not resolve imported shared
# libraries, so stage them next to the executable (paired with the
# @executable_path or $ORIGIN rpath). Everything else the runtime links (SDL3,
# ...) is static. Target names differ between an in-tree build and an installed
# SDK import.
#==========================================================
function (_rexglue_stage_unix_runtime target_name)
  foreach (
    runtime_lib
    rex::core
    rexcore
    rex::imgui
    reximgui
    rex::runtime
    rexruntime
    rex::TracyClient
    TracyClient
  )
    if (TARGET ${runtime_lib})
      add_custom_command(
        TARGET ${target_name}
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:${runtime_lib}>
                $<TARGET_FILE_DIR:${target_name}>
        VERBATIM
      )
    endif ()
  endforeach ()
endfunction ()

#==========================================================
# Stages the requested GPU emulation plugins next to the executable. Plugins are
# runtime-loaded (never linked), so TARGET_RUNTIME_DLLS misses them.
#==========================================================
function (_rexglue_stage_gpu_plugins target_name)
  foreach (plugin IN LISTS ARGN)
    set(plugin_targets "")
    foreach (candidate rexgpu-${plugin} rexgpu-${plugin}-d3d12 rexgpu-${plugin}-vulkan)
      if (TARGET ${candidate})
        list(APPEND plugin_targets ${candidate})
        add_dependencies(${target_name} ${candidate})
      endif ()
    endforeach ()
    foreach (candidate rex::gpu-${plugin} rex::gpu-${plugin}-d3d12 rex::gpu-${plugin}-vulkan)
      if (TARGET ${candidate})
        list(APPEND plugin_targets ${candidate})
      endif ()
    endforeach ()
    if (NOT plugin_targets)
      message(FATAL_ERROR "rexglue_configure_target: unknown GPU plugin '${plugin}' "
                          "(no target rexgpu-${plugin} or rex::gpu-${plugin})"
      )
    endif ()
    foreach (plugin_target IN LISTS plugin_targets)
      _rexglue_stage_library(${target_name} ${plugin_target})
    endforeach ()
  endforeach ()
endfunction ()

#==========================================================
# Stages the module libraries next to the executable, from the in-tree target
# when it exists and from the installed import otherwise.
#==========================================================
function (_rexglue_stage_modules target_name)
  set(staged_targets rexfilesystem rexapu-sdl3 rexhid-sdl3 rexui-sdl3)
  set(staged_aliases rex::filesystem rex::apu-sdl3 rex::hid-sdl3 rex::ui-sdl3)
  if (WIN32)
    list(APPEND staged_targets rexhid-xinput)
    list(APPEND staged_aliases rex::hid-xinput)
  endif ()
  foreach (module imported IN ZIP_LISTS staged_targets staged_aliases)
    if (TARGET ${module})
      set(module_target ${module})
      add_dependencies(${target_name} ${module})
    elseif (TARGET ${imported})
      set(module_target ${imported})
    else ()
      continue()
    endif ()
    _rexglue_stage_library(${target_name} ${module_target})
  endforeach ()
endfunction ()

#==========================================================
# rexglue_configure_target(<target>) - Host application
#
# Adds:
#   - Build-config define for the version stamp
#   - $ORIGIN RPATH on UNIX so the host finds librexruntime.so next to itself
#   - Windows POST_BUILD copy of TARGET_RUNTIME_DLLS.
#     Title modules colocate with the host (see rexglue_configure_module_target),
#     so this single copy handles them transitively.
#==========================================================
function (rexglue_configure_target target_name)
  cmake_parse_arguments(ARG "" "" "GPU_PLUGINS" ${ARGN})

  target_compile_definitions(${target_name} PRIVATE REXGLUE_BUILD_CONFIG="$<CONFIG>")

  if (MSVC)
    target_compile_options(
      ${target_name} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/EHs> $<$<COMPILE_LANGUAGE:CXX>:/EHc->
    )
  endif ()

  if (UNIX AND NOT APPLE)
    set_target_properties(
      ${target_name} PROPERTIES INSTALL_RPATH "$ORIGIN" BUILD_WITH_INSTALL_RPATH ON
    )
  elseif (APPLE)
    # macOS analogue of $ORIGIN to resolve @rpath dylibs (librexruntime,
    # libTracyClient, ...) next to the executable. Pairs with the runtime
    # dylib staging below so the app is self-contained.
    set_target_properties(
      ${target_name} PROPERTIES INSTALL_RPATH "@executable_path" BUILD_WITH_INSTALL_RPATH ON
    )
  endif ()

  rexglue_apply_target_settings(${target_name})

  if (WIN32)
    _rexglue_stage_runtime_dlls(${target_name})
  else ()
    _rexglue_stage_unix_runtime(${target_name})
  endif ()

  _rexglue_stage_gpu_plugins(${target_name} ${ARG_GPU_PLUGINS})

  if (WIN32)
    set(optimus_source "${CMAKE_CURRENT_BINARY_DIR}/${target_name}_optimus.cpp")
    configure_file(
      "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/rexglue_optimus.cpp.in" "${optimus_source}" COPYONLY
    )
    target_sources(${target_name} PRIVATE "${optimus_source}")
  endif ()

  _rexglue_stage_modules(${target_name})

  if (APPLE)
    _rexglue_stage_macos_vulkan_runtime(${target_name})
  endif ()
endfunction ()
