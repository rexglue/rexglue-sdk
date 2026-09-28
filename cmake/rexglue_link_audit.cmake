# Enforces the single-rexcore-copy invariant and the no-static-registrar rule for modules.
#
# rexcore is an OBJECT library whose globals (the cvar registry, the logging
# singletons) must live in exactly one loaded module. rexruntime is that module.
# A linkable target that links rexruntime and also links an OBJECT library
# carrying rexcore objects ends up with two registries.
#
# A module DLL (any SHARED or MODULE target other than rexruntime) must not carry
# static registrars.

set(REXGLUE_CORE_OBJECT_LIBS rexcore rexfilesystem rexinput rexui rexaudio)
set(REXGLUE_REGISTRAR_REGEX "REXCVAR_DEFINE_|REXLOG_DEFINE_CATEGORY|REXLOG_DEFINE_SUBCATEGORY|REX_DEFINE_APP")

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
        if(target STREQUAL "rexruntime")
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

        foreach(lib IN LISTS REXGLUE_CORE_OBJECT_LIBS)
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
        if(target STREQUAL "rexruntime")
            continue()
        endif()
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

rexglue_audit_core_linkage()
rexglue_audit_module_registrars()
