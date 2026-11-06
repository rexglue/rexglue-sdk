#==========================================================
# rex_compute_version(<out_var>
#     FLOOR_MAJOR <int>
#     FLOOR_MINOR <int>
#     GIT_DESCRIBE_LONG  <string>   # output of git describe --tags --long ...
#     GIT_DESCRIBE_EXACT <string>   # output of git describe --tags --exact-match
#     BRANCH_NAME        <string>)  # output of git symbolic-ref --short HEAD
#
# Emits a CMake-style version string: MAJOR.MINOR[.PATCH[.TWEAK]][-id].
# See https://cmake.org/cmake/help/latest/variable/CMAKE_VERSION.html
#
# Tagged commit (vX.Y[.Z[.W]]): emits the tag verbatim (without the v).
# Untagged commit, floor at the last tag's major.minor:
#   emits MAJOR.MINOR.<tag-patch>.<commit-count>-<id> (the released base plus
#   commits; the patch is not pre-incremented, so the next tag lands on it).
# Untagged commit, floor ahead of the last tag (minor/major bumped on development
#   but not yet released): holds at MAJOR.MINOR.0-<id> until main catches up.
#   id is "dev.gSHA" on any branch and "rc.gSHA" on a release/* branch.
#==========================================================
function(rex_compute_version out_var)
    set(one_value FLOOR_MAJOR FLOOR_MINOR GIT_DESCRIBE_LONG GIT_DESCRIBE_EXACT BRANCH_NAME)
    cmake_parse_arguments(ARG "" "${one_value}" "" ${ARGN})

    if(NOT "${ARG_GIT_DESCRIBE_EXACT}" STREQUAL "")
        if(ARG_GIT_DESCRIBE_EXACT MATCHES "^v([0-9]+\\.[0-9]+(\\.[0-9]+)?(\\.[0-9]+)?)$")
            set(${out_var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
            return()
        endif()
        message(FATAL_ERROR "rex_compute_version: unparseable exact tag '${ARG_GIT_DESCRIBE_EXACT}'")
    endif()

    if("${ARG_GIT_DESCRIBE_LONG}" STREQUAL "")
        message(WARNING
            "rex_compute_version: no v* tag reachable from HEAD. "
            "Falling back to ${ARG_FLOOR_MAJOR}.${ARG_FLOOR_MINOR}.0.0-dev.unknown. "
            "If this is a CI build, ensure tags are fetched (fetch-depth: 0 + fetch-tags: true). "
            "If this is a fork, push tags with: git push <fork-remote> --tags")
        set(${out_var} "${ARG_FLOOR_MAJOR}.${ARG_FLOOR_MINOR}.0.0-dev.unknown" PARENT_SCOPE)
        return()
    endif()

    if(NOT ARG_GIT_DESCRIBE_LONG MATCHES "^v([0-9]+)\\.([0-9]+)(\\.([0-9]+))?(\\.([0-9]+))?-([0-9]+)-g([0-9a-f]+)$")
        message(FATAL_ERROR "rex_compute_version: unparseable describe output '${ARG_GIT_DESCRIBE_LONG}'")
    endif()
    set(tag_major ${CMAKE_MATCH_1})
    set(tag_minor ${CMAKE_MATCH_2})
    if(CMAKE_MATCH_4)
        set(tag_patch ${CMAKE_MATCH_4})
    else()
        set(tag_patch 0)
    endif()
    set(commit_count ${CMAKE_MATCH_7})
    set(short_sha ${CMAKE_MATCH_8})

    if(ARG_BRANCH_NAME MATCHES "^release/")
        set(id "rc.g${short_sha}")
    else()
        set(id "dev.g${short_sha}")
    endif()

    # The floor (major, minor) must never regress behind the last tag.
    if(ARG_FLOOR_MAJOR LESS tag_major OR
       (ARG_FLOOR_MAJOR EQUAL tag_major AND ARG_FLOOR_MINOR LESS tag_minor))
        message(FATAL_ERROR
            "rex_compute_version: floor version (${ARG_FLOOR_MAJOR}.${ARG_FLOOR_MINOR}) is behind "
            "tag version (${tag_major}.${tag_minor}). The floor in CMakeLists.txt must not regress.")
    endif()

    if(ARG_FLOOR_MAJOR EQUAL tag_major AND ARG_FLOOR_MINOR EQUAL tag_minor)
        # main has caught up to the floor: track the released base plus commits.
        # Do not pre-increment the patch; the next release tag lands on it directly.
        set(${out_var} "${ARG_FLOOR_MAJOR}.${ARG_FLOOR_MINOR}.${tag_patch}.${commit_count}-${id}" PARENT_SCOPE)
    else()
        # Floor is ahead of the last tag (minor/major bumped on development but not
        # yet released): hold at MAJOR.MINOR.0 until main catches up, ignoring the
        # commit count so the version does not track commits-ahead-of-main.
        set(${out_var} "${ARG_FLOOR_MAJOR}.${ARG_FLOOR_MINOR}.0-${id}" PARENT_SCOPE)
    endif()
endfunction()

#==========================================================
# rex_version_channel(<out_var> VERSION <string>)
#
# release = exact v* tag, rc = release/* branch, dev = anything else.
# Stable across commits, unlike the full version, so it is safe in cache keys.
#==========================================================
function(rex_version_channel out_var)
    set(one_value VERSION)
    cmake_parse_arguments(ARG "" "${one_value}" "" ${ARGN})

    if(ARG_VERSION MATCHES "-rc\\.")
        set(${out_var} "rc" PARENT_SCOPE)
    elseif(ARG_VERSION MATCHES "-dev\\.")
        set(${out_var} "dev" PARENT_SCOPE)
    else()
        set(${out_var} "release" PARENT_SCOPE)
    endif()
endfunction()

#==========================================================
# rex_resolve_version(<out_var>
#     FLOOR_MAJOR <int>
#     FLOOR_MINOR <int>
#     [SOURCE_DIR <path>])    # defaults to CMAKE_CURRENT_SOURCE_DIR
#
# Runs git in the working tree, then delegates to rex_compute_version.
#==========================================================
function(rex_resolve_version out_var)
    set(one_value FLOOR_MAJOR FLOOR_MINOR SOURCE_DIR)
    cmake_parse_arguments(ARG "" "${one_value}" "" ${ARGN})

    if(NOT ARG_SOURCE_DIR)
        set(ARG_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()

    find_program(GIT_EXECUTABLE git)
    if(NOT GIT_EXECUTABLE)
        rex_compute_version(result
            FLOOR_MAJOR ${ARG_FLOOR_MAJOR}
            FLOOR_MINOR ${ARG_FLOOR_MINOR}
            GIT_DESCRIBE_LONG ""
            GIT_DESCRIBE_EXACT ""
            BRANCH_NAME "")
        set(${out_var} "${result}" PARENT_SCOPE)
        return()
    endif()

    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --exact-match
            --match "v[0-9]*.[0-9]*.[0-9]*"
            --exclude "*-*"
        WORKING_DIRECTORY "${ARG_SOURCE_DIR}"
        OUTPUT_VARIABLE describe_exact
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE describe_exact_rc)
    if(NOT describe_exact_rc EQUAL 0)
        set(describe_exact "")
    endif()

    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --long
            --match "v[0-9]*.[0-9]*"
            --exclude "*-*"
        WORKING_DIRECTORY "${ARG_SOURCE_DIR}"
        OUTPUT_VARIABLE describe_long
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE describe_long_rc)
    if(NOT describe_long_rc EQUAL 0)
        set(describe_long "")
    endif()

    execute_process(
        COMMAND ${GIT_EXECUTABLE} symbolic-ref --short HEAD
        WORKING_DIRECTORY "${ARG_SOURCE_DIR}"
        OUTPUT_VARIABLE branch_name
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE branch_rc)
    if(NOT branch_rc EQUAL 0)
        set(branch_name "")
    endif()

    rex_compute_version(result
        FLOOR_MAJOR ${ARG_FLOOR_MAJOR}
        FLOOR_MINOR ${ARG_FLOOR_MINOR}
        GIT_DESCRIBE_LONG "${describe_long}"
        GIT_DESCRIBE_EXACT "${describe_exact}"
        BRANCH_NAME "${branch_name}")
    set(${out_var} "${result}" PARENT_SCOPE)
endfunction()

function(rexglue_add_version_resource target_name)
    if(NOT WIN32)
        return()
    endif()
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "DESCRIPTION" "")
    get_target_property(_type ${target_name} TYPE)
    if(_type STREQUAL "EXECUTABLE")
        set(REX_RC_FILETYPE 0x1)
    else()
        set(REX_RC_FILETYPE 0x2)
    endif()
    set(REX_RC_FILE_VERSION
        "${REXGLUE_DERIVED_MAJOR},${REXGLUE_DERIVED_MINOR},${REXGLUE_DERIVED_PATCH},${REXGLUE_DERIVED_TWEAK}")
    set(REX_RC_VERSION_STRING "${REXGLUE_FULL_VERSION}")
    set(REX_RC_TARGET "${target_name}")
    set(REX_RC_DESCRIPTION "${ARG_DESCRIPTION}")
    set(_rc_file "${CMAKE_CURRENT_BINARY_DIR}/rexglue_rc/${target_name}.rc")
    configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/version.rc.in" "${_rc_file}" @ONLY)
    target_sources(${target_name} PRIVATE "${_rc_file}")
endfunction()
