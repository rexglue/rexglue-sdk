# Enforces the single-rexcore-copy invariant.
#
# rexcore is an OBJECT library whose globals (the cvar registry, the logging
# singletons) must live in exactly one loaded module. rexruntime is that module.
# A linkable target that links rexruntime and also links an OBJECT library
# carrying rexcore objects ends up with two registries, which is what produced
# the duplicate cvar registrations and the codegen SIGSEGV on exit.

set(REXGLUE_CORE_OBJECT_LIBS rexcore rexfilesystem rexui rexaudio)

function(_rexglue_collect_targets dir out_var)
    get_property(targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    get_property(subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(subdir IN LISTS subdirs)
        _rexglue_collect_targets("${subdir}" sub_targets)
        list(APPEND targets ${sub_targets})
    endforeach()
    set(${out_var} "${targets}" PARENT_SCOPE)
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
        if(NOT links OR NOT "rexruntime" IN_LIST links)
            continue()
        endif()

        foreach(lib IN LISTS REXGLUE_CORE_OBJECT_LIBS)
            if("${lib}" IN_LIST links)
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

rexglue_audit_core_linkage()
