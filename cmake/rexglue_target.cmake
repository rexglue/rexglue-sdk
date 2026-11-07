# rexglue_target.cmake
#   rexglue_apply_target_settings(<target>)  - common compile/platform flags
#   rexglue_configure_target(<target> [NO_COPY] [EXCLUDE <name>...]) - host application
#   rexglue_get_runtime_files(<target> <out>) - files staged next to the host
include_guard(GLOBAL)

include(${CMAKE_CURRENT_LIST_DIR}/rexglue_codegen.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/rexglue_recomp_options.cmake)
include(GNUInstallDirs)

# rexglue_set_rpath(<target> <origin>)
# Sets the run path of a target to its own folder and to the library folder of the install
# layout, so a target installed in the bin folder finds the SDK libraries in the lib folder.
# <origin> is $ORIGIN or @executable_path. The library folder is left out when it is the bin
# folder.
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

# Adds the commands that stage the Vulkan loader and MoltenVK with their ICD file next to
# the host to the list named by out_var.
function (_rexglue_stage_macos_vulkan_runtime out_var target_name)
  if (TARGET Vulkan::Vulkan AND TARGET Vulkan::MoltenVK)
    set(vulkan_loader Vulkan::Vulkan)
    set(moltenvk Vulkan::MoltenVK)
    set(moltenvk_icd "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/MoltenVK_icd.json")
  elseif (TARGET rex::vulkan-loader AND TARGET rex::moltenvk)
    set(vulkan_loader rex::vulkan-loader)
    set(moltenvk rex::moltenvk)
    set(moltenvk_icd "${REXGLUE_MOLTENVK_ICD}")
  else ()
    message(FATAL_ERROR "rexglue: macOS Vulkan runtime targets are unavailable")
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

# rexglue_cpu_flags(<out_var>)
# The CPU floor flags of every ReXGlue binary, the only place they are defined. The SDK applies
# them to its own targets with add_compile_options. Projects that use the SDK get them through
# rexglue_apply_target_settings, since their presets carry no -march.
# Large code model on Linux x86_64 for linking with large recompiled executables (35MB+). macOS
# uses the small model because the ld64 linker on macOS 15+ rejects text relocations produced by
# -mcmodel=large, and macOS maps binaries in the low address space so +/-2GB is sufficient.
function (rexglue_cpu_flags out_var)
  set(flags "")
  if (UNIX AND NOT APPLE)
    if (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
      list(APPEND flags -mcmodel=large)
    elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64")
      list(APPEND flags -march=armv8-a)
    endif ()
  endif ()
  if (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    list(APPEND flags -msse4.1)
  endif ()
  set(${out_var}
      "${flags}"
      PARENT_SCOPE
  )
endfunction ()

# rexglue_apply_target_settings(<target>) - Common flags
# Applied to both host apps and title DLL modules. Compile/link flags only.
# Runtime DLL staging is the host's job (see rexglue_configure_target).
function (rexglue_apply_target_settings target_name)
  rexglue_cpu_flags(flags)
  target_compile_options(${target_name} PRIVATE ${flags})
  if (UNIX
      AND NOT APPLE
      AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64"
  )
    target_link_options(${target_name} PRIVATE -Wl,--no-relax)
  endif ()
endfunction ()

# rexglue_link_host_allocator(<target>)
# Adds the operator new and delete forwarders to an executable or a shared library, so memory
# allocated in one binary can be freed in another. Static libraries are skipped because they link
# into a binary that has the forwarders.
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

# Adds a file staged next to the host to the REXGLUE_RUNTIME_FILES property of the host, as the
# path it has in the host folder. rexglue_get_runtime_files reads the property.
function (_rexglue_add_runtime_file target_name file_name)
  set_property(
    TARGET ${target_name}
    APPEND
    PROPERTY REXGLUE_RUNTIME_FILES "$<TARGET_FILE_DIR:${target_name}>/${file_name}"
  )
endfunction ()

# Adds the commands that copy a library next to the host to the list named by out_var, and keeps
# the host relinking when it changes.
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

# Lists the names the EXCLUDE keyword of rexglue_configure_target accepts: the staged provider
# modules, the SDK libraries a host links and SDL3. Names that exist on another platform only are
# included.
function (_rexglue_get_copy_names out_var)
  _rexglue_get_staged_modules(modules aliases)
  set(${out_var}
      "${modules};rexbase;rexppc;rexkernel;rexapp;TracyClient;SDL3"
      PARENT_SCOPE
  )
endfunction ()

# Lists the SDK libraries a host links, the in-tree target or the installed import of each. The
# libraries named after out_var are left out.
function (_rexglue_get_runtime_libs out_var)
  set(excluded ${ARGN})
  set(libs "")
  foreach (pair "rex::base|rexbase" "rex::ppc|rexppc" "rex::kernel|rexkernel" "rex::app|rexapp"
                "rex::TracyClient|TracyClient"
  )
    string(REPLACE "|" ";" candidates "${pair}")
    list(GET candidates -1 pair_name)
    if (pair_name IN_LIST excluded)
      continue()
    endif ()
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

# macOS and Linux host. $<TARGET_RUNTIME_DLLS> does not resolve imported shared
# libraries, so stage them next to the executable (paired with the
# @executable_path or $ORIGIN rpath). SDL3 is staged by _rexglue_stage_sdl. Target names differ
# between an in-tree build and an installed SDK import.
function (_rexglue_stage_unix_runtime out_var target_name)
  set(commands ${${out_var}})
  _rexglue_get_runtime_libs(runtime_libs ${ARGN})
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

# Adds the one POST_BUILD step that stages every runtime file next to the host, so the build
# prints a single line for the target. On Windows it starts with the runtime DLLs and their PDBs,
# and the host relinks when a staged DLL changes, since an unchanged import library skips the link
# otherwise. The runtime DLLs leave out the libraries named in dll_excludes.
function (_rexglue_add_stage_step target_name dll_excludes)
  if (WIN32)
    set(dlls "$<TARGET_RUNTIME_DLLS:${target_name}>")
    foreach (excluded_target IN LISTS dll_excludes)
      set(dlls "$<FILTER:${dlls},EXCLUDE,[/\\\\]$<TARGET_FILE_NAME:${excluded_target}>$>")
    endforeach ()
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

# Sets the two variables to the provider module targets and the installed import of each, in the
# same order.
function (_rexglue_get_staged_modules out_targets out_aliases)
  set(${out_targets}
      rexfilesystem
      rexapu
      rexhid
      rexui
      rexgpu
      rexxam
      rexxbdm
      rexgpu-xenos-null
      rexgpu-xenos-vulkan
      rexgpu-xenos-d3d12
      rexapu-sdl3
      rexhid-sdl3
      rexui-sdl3
      rexhid-xinput
      PARENT_SCOPE
  )
  set(${out_aliases}
      rex::filesystem
      rex::apu
      rex::hid
      rex::ui
      rex::gpu
      rex::xam
      rex::xbdm
      rex::gpu-xenos-null
      rex::gpu-xenos-vulkan
      rex::gpu-xenos-d3d12
      rex::apu-sdl3
      rex::hid-sdl3
      rex::ui-sdl3
      rex::hid-xinput
      PARENT_SCOPE
  )
endfunction ()

# Stages every built provider module next to the executable, from the in-tree
# target when it exists and from the installed import otherwise. The modules load
# at runtime and are never linked, so TARGET_RUNTIME_DLLS misses them.
function (_rexglue_stage_modules out_var target_name)
  set(excluded ${ARGN})
  set(commands ${${out_var}})
  _rexglue_get_staged_modules(staged_targets staged_aliases)
  foreach (module imported IN ZIP_LISTS staged_targets staged_aliases)
    if (module IN_LIST excluded OR (NOT WIN32 AND module STREQUAL "rexhid-xinput"))
      continue()
    endif ()
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

# Stages the shared SDL3 next to the executable, where the modules find it. The modules load at
# runtime, so the executable does not link SDL3 and $<TARGET_RUNTIME_DLLS> misses it. On Linux and
# macOS the copy takes the soname, since the modules refer to that name.
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

# rexglue_get_runtime_files(<host_target> <out_var>)
# Sets out_var to the files rexglue_configure_target stages next to the host, being the SDK
# libraries the host links, every provider module and the shared SDL3 under its staged
# name. Each entry is the absolute path in the host folder, with generator expressions where the
# path depends on the configuration. Call it after rexglue_configure_target and use it where
# generator expressions are accepted, such as install(FILES) and file(GENERATE).
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

# Fails configure when the keywords of rexglue_configure_target are wrong. Takes the unparsed
# arguments, the names with missing values, NO_COPY as a boolean and the EXCLUDE names in
# quotes.
function (_rexglue_check_copy_args unparsed missing no_copy excluded)
  if (unparsed)
    list(GET unparsed 0 rejected)
    message(FATAL_ERROR "rexglue_configure_target: '${rejected}' is not accepted. The keywords "
                        "are NO_COPY and EXCLUDE <name>..."
    )
  endif ()
  if ("EXCLUDE" IN_LIST missing)
    message(FATAL_ERROR "rexglue_configure_target: EXCLUDE needs at least one name")
  endif ()
  if (no_copy AND excluded)
    message(FATAL_ERROR "rexglue_configure_target: NO_COPY copies nothing, so it cannot be "
                        "combined with EXCLUDE"
    )
  endif ()
  _rexglue_get_copy_names(copy_names)
  foreach (excluded_name IN LISTS excluded)
    if (NOT excluded_name IN_LIST copy_names)
      string(REPLACE ";" ", " valid_names "${copy_names}")
      message(FATAL_ERROR "rexglue_configure_target: EXCLUDE name '${excluded_name}' is not "
                          "copied. Valid names: ${valid_names}"
      )
    endif ()
  endforeach ()
endfunction ()

# Adds the POST_BUILD step that stages the runtime files of a host, leaving out the EXCLUDE names
# after target_name.
function (_rexglue_stage_runtime_files target_name)
  set(excluded ${ARGN})
  set(stage_commands "")
  if (NOT WIN32)
    _rexglue_stage_unix_runtime(stage_commands ${target_name} ${excluded})
  else ()
    _rexglue_get_runtime_libs(runtime_libs ${excluded})
    foreach (runtime_lib IN LISTS runtime_libs)
      _rexglue_stage_library(stage_commands ${target_name} ${runtime_lib})
    endforeach ()
  endif ()

  _rexglue_stage_modules(stage_commands ${target_name} ${excluded})
  if (NOT "SDL3" IN_LIST excluded)
    _rexglue_stage_sdl(stage_commands ${target_name})
  endif ()

  if (APPLE)
    _rexglue_stage_macos_vulkan_runtime(stage_commands ${target_name})
  endif ()

  _rexglue_get_runtime_libs(dll_excludes)
  _rexglue_get_runtime_libs(kept_libs ${excluded})
  if (kept_libs)
    list(REMOVE_ITEM dll_excludes ${kept_libs})
  endif ()
  _rexglue_add_stage_step(${target_name} "${dll_excludes}" ${stage_commands})
endfunction ()

# rexglue_configure_target(<target> [NO_COPY] [EXCLUDE <name>...]) - Host application
# It adds
#   - Build-config define for the version stamp
#   - $ORIGIN RPATH on UNIX so the host finds librexkernel.so next to itself, and in the install
#     layout library folder
#   - A POST_BUILD copy of the SDK libraries the host links, every built provider module and the
#     shared SDL3 into the executable folder. On Windows it also copies TARGET_RUNTIME_DLLS.
#     Title modules colocate with the host (see rexglue_configure_module_target),
#     so this single copy handles them transitively.
# Keywords:
#   NO_COPY           Copies nothing. The RPATH, the defines, the allocator and the optimus source
#                     still apply.
#   EXCLUDE <name>... Skips the named items. A name is a provider module target (rexfilesystem,
#                     rexapu, rexhid, rexui, rexgpu, rexxam, rexxbdm, rexgpu-xenos-null,
#                     rexgpu-xenos-vulkan, rexgpu-xenos-d3d12, rexapu-sdl3, rexhid-sdl3,
#                     rexui-sdl3, rexhid-xinput), a linked SDK library (rexbase, rexppc,
#                     rexkernel, rexapp, TracyClient) or SDL3. A name of another platform is
#                     accepted and ignored. NO_COPY with EXCLUDE is an error.
function (rexglue_configure_target target_name)
  cmake_parse_arguments(PARSE_ARGV 1 ARG "NO_COPY" "" "EXCLUDE")
  _rexglue_check_copy_args(
    "${ARG_UNPARSED_ARGUMENTS}" "${ARG_KEYWORDS_MISSING_VALUES}" "${ARG_NO_COPY}" "${ARG_EXCLUDE}"
  )

  target_compile_definitions(${target_name} PRIVATE REXGLUE_BUILD_CONFIG="$<CONFIG>")

  if (UNIX AND NOT APPLE)
    rexglue_set_rpath(${target_name} "$ORIGIN")
  elseif (APPLE)
    # macOS analogue of $ORIGIN to resolve @rpath dylibs (librexkernel,
    # libTracyClient, ...) next to the executable. Pairs with the runtime
    # dylib staging below so the app is self-contained.
    rexglue_set_rpath(${target_name} "@executable_path")
  endif ()

  rexglue_apply_target_settings(${target_name})
  rexglue_link_host_allocator(${target_name})

  if (WIN32)
    set(optimus_source "${CMAKE_CURRENT_BINARY_DIR}/${target_name}_optimus.cpp")
    configure_file(
      "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/rexglue_optimus.cpp.in" "${optimus_source}" COPYONLY
    )
    target_sources(${target_name} PRIVATE "${optimus_source}")
  endif ()

  if (NOT ARG_NO_COPY)
    _rexglue_stage_runtime_files(${target_name} ${ARG_EXCLUDE})
  endif ()
endfunction ()
