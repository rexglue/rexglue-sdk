#==========================================================
# rexglue_target.cmake
#
#   rexglue_apply_target_settings(<target>)  - common compile/platform flags
#   rexglue_configure_target(<target>)       - host application
#   rexglue_get_runtime_files(<target> <out>) - files staged next to the host
#==========================================================
include_guard(GLOBAL)

include(${CMAKE_CURRENT_LIST_DIR}/rexglue_codegen.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/rexglue_recomp_options.cmake)
include(GNUInstallDirs)

#==========================================================
# rexglue_set_rpath(<target> <origin>)
#
# Sets the run path of a target to its own folder and to the library folder of the install
# layout, so a target installed in the bin folder finds the SDK libraries in the lib folder.
# <origin> is $ORIGIN or @executable_path. The library folder is left out when it is the bin
# folder.
#==========================================================
function (rexglue_set_rpath target_name origin)
  include(GNUInstallDirs)
  file(RELATIVE_PATH libdir_from_bindir "${CMAKE_INSTALL_FULL_BINDIR}"
       "${CMAKE_INSTALL_FULL_LIBDIR}"
  )
  set(rpath "${origin}")
  if (NOT libdir_from_bindir STREQUAL "")
    list(APPEND rpath "${origin}/${libdir_from_bindir}")
  endif ()
  set_target_properties(
    ${target_name} PROPERTIES INSTALL_RPATH "${rpath}" BUILD_WITH_INSTALL_RPATH ON
  )
endfunction ()

# Adds the commands that stage the pinned Vulkan loader and MoltenVK with their ICD file next to
# the host to the list named by out_var.
function (_rexglue_stage_macos_vulkan_runtime out_var target_name)
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

  set(commands ${${out_var}})
  list(
    APPEND
    commands
    COMMAND
    ${CMAKE_COMMAND}
    -E
    make_directory
    "$<TARGET_FILE_DIR:${target_name}>/vulkan/lib"
    "$<TARGET_FILE_DIR:${target_name}>/vulkan/share/vulkan/icd.d"
    COMMAND
    ${CMAKE_COMMAND}
    -E
    copy_if_different
    "$<TARGET_FILE:${vulkan_loader}>"
    "$<TARGET_FILE_DIR:${target_name}>/vulkan/lib/libvulkan.1.dylib"
    COMMAND
    ${CMAKE_COMMAND}
    -E
    copy_if_different
    "$<TARGET_FILE:${moltenvk}>"
    "$<TARGET_FILE_DIR:${target_name}>/vulkan/lib/libMoltenVK.dylib"
    COMMAND
    ${CMAKE_COMMAND}
    -E
    copy_if_different
    "${moltenvk_icd}"
    "$<TARGET_FILE_DIR:${target_name}>/vulkan/share/vulkan/icd.d/MoltenVK_icd.json"
  )
  set(${out_var}
      "${commands}"
      PARENT_SCOPE
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
# rexglue_link_host_allocator(<target>)
#
# Adds the operator new and delete forwarders to an executable or a shared library, so memory
# allocated in one binary can be freed in another. Static libraries are skipped because they link
# into a binary that has the forwarders.
#==========================================================
function (rexglue_link_host_allocator target_name)
  get_target_property(target_type ${target_name} TYPE)
  if (target_type STREQUAL "EXECUTABLE"
      OR target_type STREQUAL "SHARED_LIBRARY"
      OR target_type STREQUAL "MODULE_LIBRARY"
  )
    target_sources(
      ${target_name} PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/rexglue_host_alloc_stub.cpp"
    )
  endif ()
endfunction ()

#==========================================================
# Records a file staged next to the host in the REXGLUE_RUNTIME_FILES property of the host, as the
# path it has in the host folder. rexglue_get_runtime_files reads the property.
#==========================================================
function (_rexglue_add_runtime_file target_name file_name)
  set_property(
    TARGET ${target_name}
    APPEND
    PROPERTY REXGLUE_RUNTIME_FILES "$<TARGET_FILE_DIR:${target_name}>/${file_name}"
  )
endfunction ()

#==========================================================
# Adds the commands that copy a library next to the host to the list named by out_var, and keeps
# the host relinking when it changes.
#==========================================================
function (_rexglue_stage_library out_var target_name library)
  _rexglue_add_runtime_file(${target_name} "$<TARGET_FILE_NAME:${library}>")
  set(commands ${${out_var}})
  list(
    APPEND
    commands
    COMMAND
    ${CMAKE_COMMAND}
    -E
    copy_if_different
    $<TARGET_FILE:${library}>
    $<TARGET_FILE_DIR:${target_name}>
  )
  if (WIN32)
    list(
      APPEND
      commands
      COMMAND
      ${CMAKE_COMMAND}
      -DDEST=$<TARGET_FILE_DIR:${target_name}>
      -P
      ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/rexglue_stage_pdbs.cmake
      $<TARGET_FILE:${library}>
    )
  endif ()
  set(${out_var}
      "${commands}"
      PARENT_SCOPE
  )
  set_property(
    TARGET ${target_name}
    APPEND
    PROPERTY LINK_DEPENDS "$<TARGET_FILE:${library}>"
  )
endfunction ()

#==========================================================
# Lists the SDK libraries a host links, the in-tree target or the installed import of each.
#==========================================================
function (_rexglue_get_runtime_libs out_var)
  set(libs "")
  foreach (pair "rex::core|rexcore" "rex::imgui|reximgui" "rex::runtime|rexruntime"
                "rex::TracyClient|TracyClient"
  )
    string(REPLACE "|" ";" candidates "${pair}")
    foreach (candidate IN LISTS candidates)
      if (TARGET ${candidate})
        list(APPEND libs ${candidate})
        break()
      endif ()
    endforeach ()
  endforeach ()
  set(${out_var}
      "${libs}"
      PARENT_SCOPE
  )
endfunction ()

#==========================================================
# macOS and Linux host. $<TARGET_RUNTIME_DLLS> does not resolve imported shared
# libraries, so stage them next to the executable (paired with the
# @executable_path or $ORIGIN rpath). Everything else the runtime links (SDL3,
# ...) is static. Target names differ between an in-tree build and an installed
# SDK import.
#==========================================================
function (_rexglue_stage_unix_runtime out_var target_name)
  set(commands ${${out_var}})
  _rexglue_get_runtime_libs(runtime_libs)
  foreach (runtime_lib IN LISTS runtime_libs)
    _rexglue_add_runtime_file(${target_name} "$<TARGET_FILE_NAME:${runtime_lib}>")
    list(
      APPEND
      commands
      COMMAND
      ${CMAKE_COMMAND}
      -E
      copy_if_different
      $<TARGET_FILE:${runtime_lib}>
      $<TARGET_FILE_DIR:${target_name}>
    )
  endforeach ()
  set(${out_var}
      "${commands}"
      PARENT_SCOPE
  )
endfunction ()

#==========================================================
# Adds the one POST_BUILD step that stages every runtime file next to the host, so the build
# prints a single line for the target. On Windows it starts with the runtime DLLs and their PDBs,
# and the host relinks when a staged DLL changes, since an unchanged import library skips the link
# otherwise.
#==========================================================
function (_rexglue_add_stage_step target_name)
  if (WIN32)
    set(dlls "$<TARGET_RUNTIME_DLLS:${target_name}>")
    set(dll_dir "$<TARGET_FILE_DIR:${target_name}>")
    set(stage_pdbs "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/rexglue_stage_pdbs.cmake")
    set_property(
      TARGET ${target_name}
      APPEND
      PROPERTY LINK_DEPENDS "${dlls}"
    )
    add_custom_command(
      TARGET ${target_name}
      POST_BUILD
      COMMAND "$<$<BOOL:${dlls}>:${CMAKE_COMMAND};-E;copy_if_different;${dlls};${dll_dir}>"
      COMMAND "$<$<BOOL:${dlls}>:${CMAKE_COMMAND};-DDEST=${dll_dir};-P;${stage_pdbs};${dlls}>"
              ${ARGN}
      COMMENT "Copying runtime files for ${target_name}"
      COMMAND_EXPAND_LISTS VERBATIM
    )
  elseif (ARGN)
    set(commands ${ARGN})
    list(POP_FRONT commands)
    add_custom_command(
      TARGET ${target_name}
      POST_BUILD
      COMMAND ${commands}
      COMMENT "Copying runtime files for ${target_name}"
      VERBATIM
    )
  endif ()
endfunction ()

#==========================================================
# Stages the requested GPU emulation plugins next to the executable. Plugins are
# runtime-loaded (never linked), so TARGET_RUNTIME_DLLS misses them.
#==========================================================
function (_rexglue_stage_gpu_plugins out_var target_name)
  set(commands ${${out_var}})
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
      _rexglue_stage_library(commands ${target_name} ${plugin_target})
    endforeach ()
  endforeach ()
  set(${out_var}
      "${commands}"
      PARENT_SCOPE
  )
endfunction ()

#==========================================================
# Stages the module libraries next to the executable, from the in-tree target
# when it exists and from the installed import otherwise.
#==========================================================
function (_rexglue_stage_modules out_var target_name)
  set(commands ${${out_var}})
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
    _rexglue_stage_library(commands ${target_name} ${module_target})
  endforeach ()
  set(${out_var}
      "${commands}"
      PARENT_SCOPE
  )
endfunction ()

#==========================================================
# Stages the shared SDL3 next to the executable, where the modules find it. The modules load at
# runtime, so the executable does not link SDL3 and $<TARGET_RUNTIME_DLLS> misses it. On Linux and
# macOS the copy takes the soname, since the modules record that name.
#==========================================================
function (_rexglue_stage_sdl out_var target_name)
  if (NOT TARGET SDL3::SDL3-shared)
    return()
  endif ()
  set(commands ${${out_var}})
  if (WIN32)
    _rexglue_stage_library(commands ${target_name} SDL3::SDL3-shared)
  else ()
    list(
      APPEND
      commands
      COMMAND
      ${CMAKE_COMMAND}
      -E
      copy_if_different
      $<TARGET_FILE:SDL3::SDL3-shared>
      $<TARGET_FILE_DIR:${target_name}>/$<TARGET_SONAME_FILE_NAME:SDL3::SDL3-shared>
    )
    set_property(
      TARGET ${target_name}
      APPEND
      PROPERTY LINK_DEPENDS "$<TARGET_FILE:SDL3::SDL3-shared>"
    )
    _rexglue_add_runtime_file(${target_name} "$<TARGET_SONAME_FILE_NAME:SDL3::SDL3-shared>")
  endif ()
  set(${out_var}
      "${commands}"
      PARENT_SCOPE
  )
endfunction ()

#==========================================================
# rexglue_get_runtime_files(<host_target> <out_var>)
#
# Sets out_var to the files rexglue_configure_target stages next to the host: the SDK libraries
# the host links, the modules, the requested GPU plugins and the shared SDL3 under its staged
# name. Each entry is the absolute path in the host folder, with generator expressions where the
# path depends on the configuration. Call it after rexglue_configure_target and use it where
# generator expressions are accepted, such as install(FILES) and file(GENERATE).
#==========================================================
function (rexglue_get_runtime_files target_name out_var)
  get_target_property(files ${target_name} REXGLUE_RUNTIME_FILES)
  if (NOT files)
    set(files "")
  endif ()
  set(${out_var}
      "${files}"
      PARENT_SCOPE
  )
endfunction ()

#==========================================================
# rexglue_configure_target(<target>) - Host application
#
# Adds:
#   - Build-config define for the version stamp
#   - $ORIGIN RPATH on UNIX so the host finds librexruntime.so next to itself, and in the install
#     layout library folder
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
    rexglue_set_rpath(${target_name} "$ORIGIN")
  elseif (APPLE)
    # macOS analogue of $ORIGIN to resolve @rpath dylibs (librexruntime,
    # libTracyClient, ...) next to the executable. Pairs with the runtime
    # dylib staging below so the app is self-contained.
    rexglue_set_rpath(${target_name} "@executable_path")
  endif ()

  rexglue_apply_target_settings(${target_name})

  set(stage_commands "")
  if (NOT WIN32)
    _rexglue_stage_unix_runtime(stage_commands ${target_name})
  else ()
    _rexglue_get_runtime_libs(runtime_libs)
    foreach (runtime_lib IN LISTS runtime_libs)
      _rexglue_stage_library(stage_commands ${target_name} ${runtime_lib})
    endforeach ()
  endif ()

  _rexglue_stage_gpu_plugins(stage_commands ${target_name} ${ARG_GPU_PLUGINS})

  if (WIN32)
    set(optimus_source "${CMAKE_CURRENT_BINARY_DIR}/${target_name}_optimus.cpp")
    configure_file(
      "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/rexglue_optimus.cpp.in" "${optimus_source}" COPYONLY
    )
    target_sources(${target_name} PRIVATE "${optimus_source}")
  endif ()

  _rexglue_stage_modules(stage_commands ${target_name})

  if (APPLE)
    _rexglue_stage_macos_vulkan_runtime(stage_commands ${target_name})
  endif ()

  _rexglue_add_stage_step(${target_name} ${stage_commands})
endfunction ()
