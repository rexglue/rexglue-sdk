#==========================================================
# rexglue_helpers.cmake
#
# Three helpers, each with a single responsibility:
#   rexglue_apply_target_settings(<target>)        - common compile/platform flags
#   rexglue_configure_target(<target>)             - host application
#   rexglue_configure_module_target(<target> ...)  - guest DLL module
#==========================================================
macro(_rexglue_normalize_macos_vulkan_root candidate out_var)
    set(${out_var} "")
    if(NOT "${candidate}" STREQUAL "")
        foreach(_rexglue_candidate_root "${candidate}" "${candidate}/macOS")
            if(EXISTS "${_rexglue_candidate_root}/lib/libvulkan.1.dylib"
               OR EXISTS "${_rexglue_candidate_root}/Frameworks/vulkan.framework/vulkan"
               OR EXISTS "${_rexglue_candidate_root}/lib/libMoltenVK.dylib")
                set(${out_var} "${_rexglue_candidate_root}")
                break()
            endif()
        endforeach()
    endif()
endmacro()

macro(_rexglue_append_macos_vulkan_root roots_var candidate)
    _rexglue_normalize_macos_vulkan_root("${candidate}" _rexglue_normalized_vulkan_root)
    if(NOT "${_rexglue_normalized_vulkan_root}" STREQUAL "")
        list(FIND ${roots_var} "${_rexglue_normalized_vulkan_root}" _rexglue_vulkan_root_index)
        if(_rexglue_vulkan_root_index EQUAL -1)
            list(APPEND ${roots_var} "${_rexglue_normalized_vulkan_root}")
        endif()
    endif()
endmacro()

function(rexglue_find_macos_vulkan_runtime out_var)
    set(_rexglue_roots)

    if(DEFINED REXGLUE_VULKAN_RUNTIME_DIR AND NOT "${REXGLUE_VULKAN_RUNTIME_DIR}" STREQUAL "")
        _rexglue_append_macos_vulkan_root(_rexglue_roots "${REXGLUE_VULKAN_RUNTIME_DIR}")
    endif()

    if(DEFINED REXGLUE_SHARE_DIR AND EXISTS "${REXGLUE_SHARE_DIR}/vulkan")
        _rexglue_append_macos_vulkan_root(_rexglue_roots "${REXGLUE_SHARE_DIR}/vulkan")
    endif()

    if(DEFINED ENV{REX_VULKAN_SDK} AND NOT "$ENV{REX_VULKAN_SDK}" STREQUAL "")
        _rexglue_append_macos_vulkan_root(_rexglue_roots "$ENV{REX_VULKAN_SDK}")
    endif()

    if(DEFINED ENV{VULKAN_SDK} AND NOT "$ENV{VULKAN_SDK}" STREQUAL "")
        _rexglue_append_macos_vulkan_root(_rexglue_roots "$ENV{VULKAN_SDK}")
    endif()

    if(DEFINED ENV{HOME} AND IS_DIRECTORY "$ENV{HOME}/VulkanSDK")
        file(GLOB _rexglue_sdk_versions LIST_DIRECTORIES true "$ENV{HOME}/VulkanSDK/*")
        list(SORT _rexglue_sdk_versions COMPARE NATURAL ORDER DESCENDING)
        foreach(_rexglue_sdk_version IN LISTS _rexglue_sdk_versions)
            _rexglue_append_macos_vulkan_root(_rexglue_roots "${_rexglue_sdk_version}")
        endforeach()
    endif()

    _rexglue_append_macos_vulkan_root(_rexglue_roots "/usr/local")
    _rexglue_append_macos_vulkan_root(_rexglue_roots "/opt/homebrew")

    set(_rexglue_runtime_root "")
    if(_rexglue_roots)
        list(GET _rexglue_roots 0 _rexglue_runtime_root)
    endif()

    set(${out_var} "${_rexglue_runtime_root}" PARENT_SCOPE)
endfunction()

function(_rexglue_warn_missing_macos_vulkan_runtime)
    get_property(_rexglue_warning_emitted GLOBAL PROPERTY REXGLUE_MACOS_VULKAN_RUNTIME_WARNING_EMITTED)
    if(NOT _rexglue_warning_emitted)
        message(WARNING
            "No macOS Vulkan runtime was found. Install the LunarG Vulkan SDK or set "
            "REXGLUE_VULKAN_RUNTIME_DIR/REX_VULKAN_SDK to a Vulkan runtime root.")
        set_property(GLOBAL PROPERTY REXGLUE_MACOS_VULKAN_RUNTIME_WARNING_EMITTED TRUE)
    endif()
endfunction()

function(_rexglue_copy_macos_vulkan_runtime target_name runtime_root)
    if("${runtime_root}" STREQUAL "")
        _rexglue_warn_missing_macos_vulkan_runtime()
        return()
    endif()

    set(_rexglue_runtime_files)
    foreach(_rexglue_runtime_file
            lib/libvulkan.1.dylib
            lib/libvulkan.dylib
            lib/libMoltenVK.dylib
            lib/libSPIRV-Tools-shared.dylib
            share/vulkan/icd.d/MoltenVK_icd.json)
        if(EXISTS "${runtime_root}/${_rexglue_runtime_file}")
            list(APPEND _rexglue_runtime_files "${_rexglue_runtime_file}")
        endif()
    endforeach()

    if(NOT _rexglue_runtime_files)
        _rexglue_warn_missing_macos_vulkan_runtime()
        return()
    endif()

    set(_rexglue_runtime_commands
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target_name}>/vulkan/lib"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target_name}>/vulkan/share/vulkan/icd.d"
    )
    foreach(_rexglue_runtime_file IN LISTS _rexglue_runtime_files)
        get_filename_component(_rexglue_runtime_dir "${_rexglue_runtime_file}" DIRECTORY)
        list(APPEND _rexglue_runtime_commands
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target_name}>/vulkan/${_rexglue_runtime_dir}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${runtime_root}/${_rexglue_runtime_file}"
                "$<TARGET_FILE_DIR:${target_name}>/vulkan/${_rexglue_runtime_dir}"
        )
    endforeach()

    add_custom_command(TARGET ${target_name} POST_BUILD
        ${_rexglue_runtime_commands}
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
    set(_rexglue_target_processor "${CMAKE_SYSTEM_PROCESSOR}")
    if(APPLE AND CMAKE_OSX_ARCHITECTURES)
        list(LENGTH CMAKE_OSX_ARCHITECTURES _rexglue_osx_arch_count)
        if(_rexglue_osx_arch_count GREATER 1)
            message(FATAL_ERROR "ReXGlue macOS consumers expect a single CMAKE_OSX_ARCHITECTURES value")
        endif()
        list(GET CMAKE_OSX_ARCHITECTURES 0 _rexglue_target_processor)
    endif()

    if(UNIX AND NOT APPLE)
        # Large executable support
        if(_rexglue_target_processor MATCHES "x86_64|AMD64")
            target_link_options(${target_name} PRIVATE -Wl,--no-relax)
            target_compile_options(${target_name} PRIVATE -mcmodel=large)
        elseif(_rexglue_target_processor MATCHES "aarch64|ARM64|arm64")
            target_compile_options(${target_name} PRIVATE -march=armv8-a)
        endif()
    endif()

    if(NOT MSVC)
        if(_rexglue_target_processor MATCHES "x86_64|AMD64")
            target_compile_options(${target_name} PRIVATE -msse4.1)
        endif()
    endif()
endfunction()

#==========================================================
# rexglue_configure_target(<target>) - Host application
#
# Adds:
#   - SDL3 entry point source (windowed_app_main_sdl.cpp)
#   - ReXApp base class source (rex_app.cpp)
#   - Build-config define for the version stamp
#   - $ORIGIN RPATH on UNIX so the host finds librexruntime.so next to itself
#   - Windows POST_BUILD copy of TARGET_RUNTIME_DLLS and the FidelityFX DLLs.
#     Guest modules colocate with the host (see rexglue_configure_module_target),
#     so this single copy handles them transitively.
#==========================================================
function(rexglue_configure_target target_name)
    cmake_parse_arguments(ARG "" "" "GPU_PLUGINS" ${ARGN})

    target_sources(${target_name} PRIVATE
        ${REXGLUE_SHARE_DIR}/windowed_app_main_sdl.cpp
        ${REXGLUE_SHARE_DIR}/rex_app.cpp)

    target_compile_definitions(${target_name} PRIVATE
        REXGLUE_BUILD_CONFIG="$<CONFIG>")

    if(APPLE)
        set_target_properties(${target_name} PROPERTIES
            INSTALL_RPATH "@executable_path"
            BUILD_WITH_INSTALL_RPATH ON
        )
    elseif(UNIX)
        set_target_properties(${target_name} PROPERTIES
            INSTALL_RPATH "$ORIGIN"
            BUILD_WITH_INSTALL_RPATH ON
        )
    endif()

    rexglue_apply_target_settings(${target_name})

    if(WIN32)
        # Stage runtime DLLs (rexruntime, TracyClient, etc.) next to the host
        # binary on every link. copy_if_different is a no-op when up to date.
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND "$<$<BOOL:$<TARGET_RUNTIME_DLLS:${target_name}>>:${CMAKE_COMMAND};-E;copy_if_different;$<TARGET_RUNTIME_DLLS:${target_name}>;$<TARGET_FILE_DIR:${target_name}>>"
            COMMAND_EXPAND_LISTS
            VERBATIM
        )
        # FidelityFX is linked PRIVATE by rexui (to avoid propagating DLL
        # requirements to tool-mode targets), so copy its DLLs explicitly.
        foreach(_fx amd_fidelityfx_vk amd_fidelityfx_dx12)
            if(TARGET ${_fx})
                add_custom_command(TARGET ${target_name} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        $<TARGET_FILE:${_fx}>
                        $<TARGET_FILE_DIR:${target_name}>
                    VERBATIM
                )
            endif()
        endforeach()
    endif()

    if(APPLE)
        # TARGET_RUNTIME_DLLS doesn't reliably enumerate imported dylibs in the
        # installed-SDK consumer case, so stage the known shared runtime pieces
        # explicitly next to the host binary.
        foreach(_runtime_target rex::runtime rex::TracyClient)
            if(TARGET ${_runtime_target})
                add_custom_command(TARGET ${target_name} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        $<TARGET_FILE:${_runtime_target}>
                        $<TARGET_FILE_DIR:${target_name}>
                    VERBATIM
                )
            endif()
        endforeach()
    endif()

    # Stage requested GPU emulation plugins next to the executable. Plugins
    # are runtime-loaded (never linked), so TARGET_RUNTIME_DLLS misses them.
    foreach(_plugin IN LISTS ARG_GPU_PLUGINS)
        if(TARGET rexgpu-${_plugin})
            # In-tree build: depend on it so it gets built.
            set(_plugin_target rexgpu-${_plugin})
            add_dependencies(${target_name} ${_plugin_target})
        elseif(TARGET rex::gpu-${_plugin})
            # Installed SDK import.
            set(_plugin_target rex::gpu-${_plugin})
        else()
            message(FATAL_ERROR
                "rexglue_configure_target: unknown GPU plugin '${_plugin}' "
                "(no target rexgpu-${_plugin} or rex::gpu-${_plugin})")
        endif()
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:${_plugin_target}>
                $<TARGET_FILE_DIR:${target_name}>
            VERBATIM
        )
        unset(_plugin_target)
    endforeach()

    if(APPLE)
        if(REXGLUE_USE_VULKAN)
            rexglue_find_macos_vulkan_runtime(_rexglue_macos_vulkan_runtime_root)
            _rexglue_copy_macos_vulkan_runtime(${target_name} "${_rexglue_macos_vulkan_runtime_root}")
        endif()
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
