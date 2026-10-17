#==========================================================
# rexglue_target.cmake
#
#   rexglue_apply_target_settings(<target>)  - common compile/platform flags
#   rexglue_configure_target(<target>)       - host application
#==========================================================
include_guard(GLOBAL)

function(_rexglue_stage_macos_vulkan_runtime target_name)
    if(TARGET Vulkan::Loader AND TARGET MoltenVK::MoltenVK)
        set(_rexglue_vulkan_loader Vulkan::Loader)
        set(_rexglue_moltenvk MoltenVK::MoltenVK)
        set(_rexglue_moltenvk_icd "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/MoltenVK_icd.json")
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
# Applied to both host apps and title DLL modules. Compile/link flags only;
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
#   - $ORIGIN RPATH on UNIX so the host finds librexruntime.so next to itself
#   - Windows POST_BUILD copy of TARGET_RUNTIME_DLLS.
#     Title modules colocate with the host (see rexglue_configure_module_target),
#     so this single copy handles them transitively.
#==========================================================
function(rexglue_configure_target target_name)
    cmake_parse_arguments(ARG "" "" "GPU_PLUGINS" ${ARGN})

    target_compile_definitions(${target_name} PRIVATE
        REXGLUE_BUILD_CONFIG="$<CONFIG>")

    if(MSVC)
        target_compile_options(${target_name} PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:/EHs> $<$<COMPILE_LANGUAGE:CXX>:/EHc->)
    endif()

    if(UNIX AND NOT APPLE)
        set_target_properties(${target_name} PROPERTIES
            INSTALL_RPATH "$ORIGIN"
            BUILD_WITH_INSTALL_RPATH ON
        )
    elseif(APPLE)
        # macOS analogue of $ORIGIN: resolve @rpath dylibs (librexruntime,
        # libTracyClient, ...) next to the executable. Pairs with the runtime
        # dylib staging below so the app is self-contained.
        set_target_properties(${target_name} PROPERTIES
            INSTALL_RPATH "@executable_path"
            BUILD_WITH_INSTALL_RPATH ON
        )
    endif()

    rexglue_apply_target_settings(${target_name})

    if(WIN32)
        # Stage runtime DLLs (rexruntime, TracyClient, etc.) next to the host
        # binary on every link. The host relinks when a staged DLL changes,
        # since an unchanged import library skips the link otherwise.
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND "$<$<BOOL:$<TARGET_RUNTIME_DLLS:${target_name}>>:${CMAKE_COMMAND};-E;copy_if_different;$<TARGET_RUNTIME_DLLS:${target_name}>;$<TARGET_FILE_DIR:${target_name}>>"
            COMMAND_EXPAND_LISTS
            VERBATIM
        )
        set_property(TARGET ${target_name} APPEND PROPERTY
            LINK_DEPENDS "$<TARGET_RUNTIME_DLLS:${target_name}>")
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND "$<$<BOOL:$<TARGET_RUNTIME_DLLS:${target_name}>>:${CMAKE_COMMAND};-DDEST=$<TARGET_FILE_DIR:${target_name}>;-P;${CMAKE_CURRENT_FUNCTION_LIST_DIR}/rexglue_stage_pdbs.cmake;$<TARGET_RUNTIME_DLLS:${target_name}>>"
            COMMAND_EXPAND_LISTS
            VERBATIM
        )
    else()
        # macOS and Linux: $<TARGET_RUNTIME_DLLS> does not resolve imported
        # shared libraries, so stage them explicitly next to the executable
        # (paired with the @executable_path or $ORIGIN rpath above). Everything
        # else the runtime links (SDL3, ...) is static. Target names
        # differ between an in-tree build and an installed SDK import.
        foreach(_rexglue_runtime_lib rex::core rexcore rex::imgui reximgui rex::runtime rexruntime rex::TracyClient TracyClient)
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
        set(_plugin_targets "")
        foreach(_candidate rexgpu-${_plugin} rexgpu-${_plugin}-d3d12 rexgpu-${_plugin}-vulkan)
            if(TARGET ${_candidate})
                list(APPEND _plugin_targets ${_candidate})
                add_dependencies(${target_name} ${_candidate})
            endif()
        endforeach()
        foreach(_candidate rex::gpu-${_plugin} rex::gpu-${_plugin}-d3d12 rex::gpu-${_plugin}-vulkan)
            if(TARGET ${_candidate})
                list(APPEND _plugin_targets ${_candidate})
            endif()
        endforeach()
        if(NOT _plugin_targets)
            message(FATAL_ERROR
                "rexglue_configure_target: unknown GPU plugin '${_plugin}' "
                "(no target rexgpu-${_plugin} or rex::gpu-${_plugin})")
        endif()
        foreach(_plugin_target IN LISTS _plugin_targets)
            add_custom_command(TARGET ${target_name} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:${_plugin_target}>
                    $<TARGET_FILE_DIR:${target_name}>
                VERBATIM
            )
            if(WIN32)
                add_custom_command(TARGET ${target_name} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -DDEST=$<TARGET_FILE_DIR:${target_name}>
                        -P ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/rexglue_stage_pdbs.cmake
                        $<TARGET_FILE:${_plugin_target}>
                    VERBATIM
                )
            endif()
            set_property(TARGET ${target_name} APPEND PROPERTY
                LINK_DEPENDS "$<TARGET_FILE:${_plugin_target}>")
        endforeach()
    endforeach()

    if(WIN32)
        set(_optimus_source "${CMAKE_CURRENT_BINARY_DIR}/${target_name}_optimus.cpp")
        configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/rexglue_optimus.cpp.in"
            "${_optimus_source}" COPYONLY)
        target_sources(${target_name} PRIVATE "${_optimus_source}")
    endif()

    set(_rexglue_staged_targets rexfilesystem rexapu-sdl3 rexhid-sdl3 rexui-sdl3)
    set(_rexglue_staged_aliases rex::filesystem rex::apu-sdl3 rex::hid-sdl3 rex::ui-sdl3)
    if(WIN32)
        list(APPEND _rexglue_staged_targets rexhid-xinput)
        list(APPEND _rexglue_staged_aliases rex::hid-xinput)
    endif()
    foreach(_module _imported IN ZIP_LISTS _rexglue_staged_targets _rexglue_staged_aliases)
        if(TARGET ${_module})
            set(_module_target ${_module})
            add_dependencies(${target_name} ${_module})
        elseif(TARGET ${_imported})
            set(_module_target ${_imported})
        else()
            continue()
        endif()
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:${_module_target}>
                $<TARGET_FILE_DIR:${target_name}>
            VERBATIM
        )
        if(WIN32)
            add_custom_command(TARGET ${target_name} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -DDEST=$<TARGET_FILE_DIR:${target_name}>
                    -P ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/rexglue_stage_pdbs.cmake
                    $<TARGET_FILE:${_module_target}>
                VERBATIM
            )
        endif()
        set_property(TARGET ${target_name} APPEND PROPERTY
            LINK_DEPENDS "$<TARGET_FILE:${_module_target}>")
        unset(_module_target)
    endforeach()
    unset(_rexglue_staged_targets)
    unset(_rexglue_staged_aliases)

    if(APPLE)
        _rexglue_stage_macos_vulkan_runtime(${target_name})
    endif()
endfunction()
