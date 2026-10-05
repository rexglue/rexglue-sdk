#==========================================================
# rexglue_helpers.cmake
#
# Three helpers, each with a single responsibility:
#   rexglue_apply_target_settings(<target>)        - common compile/platform flags
#   rexglue_configure_target(<target>)             - host application
#   rexglue_configure_module_target(<target> ...)  - guest DLL module
#==========================================================
include_guard(GLOBAL)
set(_REXGLUE_HELPERS_DIR ${CMAKE_CURRENT_LIST_DIR})

function(_rexglue_stage_macos_vulkan_runtime target_name)
    if(TARGET Vulkan::Loader AND TARGET MoltenVK::MoltenVK)
        set(_rexglue_vulkan_loader Vulkan::Loader)
        set(_rexglue_moltenvk MoltenVK::MoltenVK)
        set(_rexglue_moltenvk_icd "${REXGLUE_ROOT}/cmake/MoltenVK_icd.json")
    elseif(TARGET rex::vulkan-loader AND TARGET rex::moltenvk)
        set(_rexglue_vulkan_loader rex::vulkan-loader)
        set(_rexglue_moltenvk rex::moltenvk)
        set(_rexglue_moltenvk_icd "${REXGLUE_MOLTENVK_ICD}")
    else()
        message(FATAL_ERROR "rexglue: pinned macOS Vulkan runtime targets are unavailable")
    endif()

    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:${target_name}>/vulkan/lib"
            "$<TARGET_FILE_DIR:${target_name}>/vulkan/share/vulkan/icd.d"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:${_rexglue_vulkan_loader}>"
            "$<TARGET_FILE_DIR:${target_name}>/vulkan/lib/libvulkan.1.dylib"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:${_rexglue_moltenvk}>"
            "$<TARGET_FILE_DIR:${target_name}>/vulkan/lib/libMoltenVK.dylib"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_rexglue_moltenvk_icd}"
            "$<TARGET_FILE_DIR:${target_name}>/vulkan/share/vulkan/icd.d/MoltenVK_icd.json"
        VERBATIM
    )
endfunction()

#==========================================================
# rexglue_apply_target_settings(<target>) - Common flags
#
# Applied to both host apps and guest DLL modules. Compile/link flags only;
# runtime DLL staging is the host's job (see rexglue_configure_target).
#==========================================================
function(rexglue_apply_target_settings target_name)
    if(UNIX AND NOT APPLE)
        # Large executable support
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
            target_link_options(${target_name} PRIVATE -Wl,--no-relax)
            target_compile_options(${target_name} PRIVATE -mcmodel=large)
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64")
            target_compile_options(${target_name} PRIVATE -march=armv8-a)
        endif()
    endif()

    if(NOT MSVC)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
            target_compile_options(${target_name} PRIVATE -msse4.1)
        endif()
    endif()
endfunction()

#==========================================================
# rexglue_configure_target(<target>) - Host application
#
# Adds:
#   - Build-config define for the version stamp
#   - $ORIGIN RPATH on UNIX so the host finds librex-runtime.so next to itself
#   - Windows POST_BUILD copy of TARGET_RUNTIME_DLLS.
#     Guest modules colocate with the host (see rexglue_configure_module_target),
#     so this single copy handles them transitively.
#==========================================================
function(rexglue_configure_target target_name)
    cmake_parse_arguments(ARG "" "" "GPU_PLUGINS" ${ARGN})

    target_compile_definitions(${target_name} PRIVATE
        REXGLUE_BUILD_CONFIG="$<CONFIG>")

    if(UNIX AND NOT APPLE)
        set_target_properties(${target_name} PROPERTIES
            INSTALL_RPATH "$ORIGIN"
            BUILD_WITH_INSTALL_RPATH ON
        )
    elseif(APPLE)
        # macOS analogue of $ORIGIN: resolve @rpath dylibs (librex-runtime,
        # libTracyClient, ...) next to the executable. Pairs with the runtime
        # dylib staging below so the app is self-contained.
        set_target_properties(${target_name} PROPERTIES
            INSTALL_RPATH "@executable_path"
            BUILD_WITH_INSTALL_RPATH ON
        )
    endif()

    rexglue_apply_target_settings(${target_name})

    if(WIN32)
        # Stage runtime DLLs (rex-runtime, TracyClient, etc.) next to the host
        # binary on every link. The host relinks when a staged DLL changes,
        # since an unchanged import library skips the link otherwise.
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND "$<$<BOOL:$<TARGET_RUNTIME_DLLS:${target_name}>>:${CMAKE_COMMAND};-E;copy_if_different;$<TARGET_RUNTIME_DLLS:${target_name}>;$<TARGET_FILE_DIR:${target_name}>>"
            COMMAND_EXPAND_LISTS
            VERBATIM
        )
        set_property(TARGET ${target_name} APPEND PROPERTY
            LINK_DEPENDS "$<TARGET_RUNTIME_DLLS:${target_name}>")
    elseif(APPLE)
        # macOS: $<TARGET_RUNTIME_DLLS> does not resolve imported dylibs, so
        # stage the shared runtime libraries explicitly next to the executable
        # (paired with the @executable_path rpath above). Everything else the
        # runtime links (fmt, SDL3, ...) is static. Target names
        # differ between an in-tree build and an installed SDK import.
        foreach(_rexglue_runtime_lib rex::core rex-core rex::runtime rex-runtime rex::TracyClient TracyClient)
            if(TARGET ${_rexglue_runtime_lib})
                add_custom_command(TARGET ${target_name} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        $<TARGET_FILE:${_rexglue_runtime_lib}>
                        $<TARGET_FILE_DIR:${target_name}>
                    VERBATIM
                )
            endif()
        endforeach()
    endif()

    # Stage requested GPU emulation plugins next to the executable. Plugins
    # are runtime-loaded (never linked), so TARGET_RUNTIME_DLLS misses them.
    foreach(_plugin IN LISTS ARG_GPU_PLUGINS)
        if(TARGET rex-gpu-${_plugin})
            # In-tree build: depend on it so it gets built.
            set(_plugin_target rex-gpu-${_plugin})
            add_dependencies(${target_name} ${_plugin_target})
        elseif(TARGET rex::gpu-${_plugin})
            # Installed SDK import.
            set(_plugin_target rex::gpu-${_plugin})
        else()
            message(FATAL_ERROR
                "rexglue_configure_target: unknown GPU plugin '${_plugin}' "
                "(no target rex-gpu-${_plugin} or rex::gpu-${_plugin})")
        endif()
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:${_plugin_target}>
                $<TARGET_FILE_DIR:${target_name}>
            VERBATIM
        )
        set_property(TARGET ${target_name} APPEND PROPERTY
            LINK_DEPENDS "$<TARGET_FILE:${_plugin_target}>")
        unset(_plugin_target)
    endforeach()

    if(WIN32)
        set(_optimus_source "${CMAKE_CURRENT_BINARY_DIR}/${target_name}_optimus.cpp")
        configure_file("${_REXGLUE_HELPERS_DIR}/rexglue_optimus.cpp.in" "${_optimus_source}" COPYONLY)
        target_sources(${target_name} PRIVATE "${_optimus_source}")
    endif()

    if(TARGET rex-filesystem)
        set(_rexglue_filesystem rex-filesystem)
        add_dependencies(${target_name} rex-filesystem)
    elseif(TARGET rex::filesystem)
        set(_rexglue_filesystem rex::filesystem)
    endif()
    if(_rexglue_filesystem)
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:${_rexglue_filesystem}>
                $<TARGET_FILE_DIR:${target_name}>
            VERBATIM
        )
        unset(_rexglue_filesystem)
    endif()

    if(APPLE)
        _rexglue_stage_macos_vulkan_runtime(${target_name})
    endif()
endfunction()

#==========================================================
# rexglue_configure_module_target(<target> [HOST <host_target>])
#   - Guest DLL module
#
# Output is colocated with HOST (or CMAKE_RUNTIME_OUTPUT_DIRECTORY) so the
# host's LoadUserModule finds it; the host is wired to depend on the module
# so a top-level build of the host pulls in all guest DLLs. No runtime DLL
# staging here; the host's POST_BUILD copy covers shared dependencies.
#==========================================================
function(rexglue_configure_module_target target_name)
    cmake_parse_arguments(ARG "" "HOST" "" ${ARGN})

    if(ARG_HOST)
        set_target_properties(${target_name} PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:${ARG_HOST}>
            RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:${ARG_HOST}>
            ARCHIVE_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:${ARG_HOST}>
        )
        # Defer add_dependencies so the host target may be declared after
        # this module call. Wrapping in EVAL CODE expands the variables now,
        # which DEFER CALL otherwise treats as literal argument text.
        cmake_language(EVAL CODE
            "cmake_language(DEFER CALL add_dependencies ${ARG_HOST} ${target_name})")
    elseif(CMAKE_RUNTIME_OUTPUT_DIRECTORY)
        set_target_properties(${target_name} PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
            ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
        )
    endif()

    if(UNIX AND NOT APPLE)
        set_target_properties(${target_name} PROPERTIES
            INSTALL_RPATH "$ORIGIN"
            BUILD_WITH_INSTALL_RPATH ON
        )
    endif()

    rexglue_apply_target_settings(${target_name})
endfunction()

#==========================================================
# rexglue_embed_metadata(<target> DIRECTORY <dir> [PREFIX <relative-prefix>])
#
# Embeds files into the host executable as metadata assets. Runtime lookups use
# paths relative to the metadata root, so embedding metadata/icons with PREFIX
# icons makes "icons/foo.png" resolve without a loose sidecar file.
#==========================================================
function(rexglue_embed_metadata target_name)
    cmake_parse_arguments(ARG "" "DIRECTORY;PREFIX" "" ${ARGN})

    if(NOT TARGET ${target_name})
        message(FATAL_ERROR "rexglue_embed_metadata: target '${target_name}' does not exist")
    endif()
    if(NOT ARG_DIRECTORY)
        message(FATAL_ERROR "rexglue_embed_metadata: DIRECTORY is required")
    endif()
    if(NOT IS_DIRECTORY "${ARG_DIRECTORY}")
        message(FATAL_ERROR "rexglue_embed_metadata: '${ARG_DIRECTORY}' is not a directory")
    endif()

    file(GLOB_RECURSE _rexglue_metadata_files
        CONFIGURE_DEPENDS
        "${ARG_DIRECTORY}/*")

    set(_rexglue_regular_files)
    foreach(_rexglue_file IN LISTS _rexglue_metadata_files)
        if(NOT IS_DIRECTORY "${_rexglue_file}")
            list(APPEND _rexglue_regular_files "${_rexglue_file}")
        endif()
    endforeach()

    if(NOT _rexglue_regular_files)
        message(STATUS "rexglue_embed_metadata: no files found in ${ARG_DIRECTORY}")
        return()
    endif()

    string(MAKE_C_IDENTIFIER "${target_name}" _rexglue_target_id)
    set(_rexglue_output_dir "${CMAKE_CURRENT_BINARY_DIR}/rexglue_embedded_metadata")
    set(_rexglue_output "${_rexglue_output_dir}/${_rexglue_target_id}_embedded_metadata.cpp")
    file(MAKE_DIRECTORY "${_rexglue_output_dir}")

    set(_rexglue_content "// Auto-generated by rexglue_embed_metadata - DO NOT EDIT\n")
    string(APPEND _rexglue_content
        "#include <cstddef>\n"
        "#include <cstdint>\n"
        "#include <rex/embedded_metadata.h>\n\n"
        "namespace {\n\n")

    set(_rexglue_count 0)
    foreach(_rexglue_file IN LISTS _rexglue_regular_files)
        file(RELATIVE_PATH _rexglue_rel "${ARG_DIRECTORY}" "${_rexglue_file}")
        string(REPLACE "\\" "/" _rexglue_rel "${_rexglue_rel}")
        if(ARG_PREFIX)
            string(REPLACE "\\" "/" _rexglue_prefix "${ARG_PREFIX}")
            string(REGEX REPLACE "/$" "" _rexglue_prefix "${_rexglue_prefix}")
            set(_rexglue_asset_path "${_rexglue_prefix}/${_rexglue_rel}")
        else()
            set(_rexglue_asset_path "${_rexglue_rel}")
        endif()
        string(REPLACE "\"" "\\\"" _rexglue_asset_path "${_rexglue_asset_path}")

        string(MD5 _rexglue_asset_id "${_rexglue_asset_path}")
        file(READ "${_rexglue_file}" _rexglue_hex HEX)
        string(REGEX REPLACE "([0-9A-Fa-f][0-9A-Fa-f])" "0x\\1," _rexglue_bytes "${_rexglue_hex}")

        string(APPEND _rexglue_content
            "const std::uint8_t kAsset_${_rexglue_asset_id}[] = {${_rexglue_bytes}};\n"
            "const bool kRegistered_${_rexglue_asset_id} = "
            "::rex::RegisterEmbeddedMetadataAsset(\"${_rexglue_asset_path}\", "
            "kAsset_${_rexglue_asset_id}, sizeof(kAsset_${_rexglue_asset_id}));\n\n")
        math(EXPR _rexglue_count "${_rexglue_count} + 1")
    endforeach()

    string(APPEND _rexglue_content "}  // namespace\n")
    file(WRITE "${_rexglue_output}" "${_rexglue_content}")
    target_sources(${target_name} PRIVATE "${_rexglue_output}")
    message(STATUS "Embedded ${_rexglue_count} metadata asset(s) into ${target_name}")
endfunction()
