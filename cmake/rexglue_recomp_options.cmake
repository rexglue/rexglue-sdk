# Returns the build configurations that have their own generated code settings.
function (_rexglue_recomp_configs out_var)
  set(${out_var}
      DEBUG RELEASE RELWITHDEBINFO
      PARENT_SCOPE
  )
endfunction ()

# Declares the cache variables of the plain and the per configuration settings.
function (rexglue_recomp_declare_settings)
  _rexglue_recomp_configs(recomp_configs)
  set(REXGLUE_RECOMP_OPTIMIZATION
      ""
      CACHE STRING "Optimization level for generated code in every configuration (empty keeps the \
per configuration defaults, or O0 to O3)"
  )
  set_property(CACHE REXGLUE_RECOMP_OPTIMIZATION PROPERTY STRINGS "" O0 O1 O2 O3)
  set(REXGLUE_RECOMP_DEBUG_INFO
      ""
      CACHE STRING "Debug info level for generated code in every configuration (empty keeps the \
per configuration defaults, line-tables-only, full or none)"
  )
  set_property(CACHE REXGLUE_RECOMP_DEBUG_INFO PROPERTY STRINGS "" line-tables-only full none)
  foreach (config IN LISTS recomp_configs)
    set(REXGLUE_RECOMP_OPTIMIZATION_${config}
        ""
        CACHE STRING "Optimization level for generated code in the ${config} configuration (empty \
keeps REXGLUE_RECOMP_OPTIMIZATION)"
    )
    set_property(CACHE REXGLUE_RECOMP_OPTIMIZATION_${config} PROPERTY STRINGS "" O0 O1 O2 O3)
    set(REXGLUE_RECOMP_DEBUG_INFO_${config}
        ""
        CACHE STRING "Debug info level for generated code in the ${config} configuration (empty \
keeps REXGLUE_RECOMP_DEBUG_INFO)"
    )
    set_property(
      CACHE REXGLUE_RECOMP_DEBUG_INFO_${config} PROPERTY STRINGS "" line-tables-only full none
    )
  endforeach ()
endfunction ()

# Resolves the optimization and the debug info level of one configuration from the settings and
# the defaults.
function (rexglue_recomp_resolve config optimization_var debug_info_var)
  set(optimization "${REXGLUE_RECOMP_OPTIMIZATION_${config}}")
  if (optimization STREQUAL "")
    set(optimization "${REXGLUE_RECOMP_OPTIMIZATION}")
  endif ()
  if (optimization STREQUAL "" AND config STREQUAL "DEBUG")
    set(optimization O1)
  endif ()
  if (NOT optimization MATCHES "^(O[0-3])?$")
    message(FATAL_ERROR "REXGLUE_RECOMP_OPTIMIZATION must be empty or one of O0, O1, O2, O3 "
                        "(got '${optimization}' for ${config})"
    )
  endif ()

  set(debug_info "${REXGLUE_RECOMP_DEBUG_INFO_${config}}")
  if (debug_info STREQUAL "")
    set(debug_info "${REXGLUE_RECOMP_DEBUG_INFO}")
  endif ()
  if (debug_info STREQUAL "" AND config STREQUAL "RELEASE")
    set(debug_info none)
  elseif (debug_info STREQUAL "")
    set(debug_info line-tables-only)
  endif ()
  if (NOT debug_info MATCHES "^(line-tables-only|full|none)$")
    message(FATAL_ERROR "REXGLUE_RECOMP_DEBUG_INFO must be empty, line-tables-only, full or none "
                        "(got '${debug_info}' for ${config})"
    )
  endif ()

  set(${optimization_var}
      "${optimization}"
      PARENT_SCOPE
  )
  set(${debug_info_var}
      "${debug_info}"
      PARENT_SCOPE
  )
endfunction ()

# Returns the compiler flags of an optimization and a debug info level.
function (rexglue_recomp_flags flags_var optimization debug_info)
  set(flags "")
  if (debug_info STREQUAL "none")
    list(APPEND flags $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-g0>)
  elseif (debug_info STREQUAL "line-tables-only")
    list(APPEND flags $<$<CXX_COMPILER_ID:Clang,AppleClang>:-gline-tables-only>)
  else ()
    list(APPEND flags $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-g>)
  endif ()

  if (optimization STREQUAL "")
    # The build type flags apply.
  else ()
    list(APPEND flags -${optimization})
  endif ()
  set(${flags_var}
      "${flags}"
      PARENT_SCOPE
  )
endfunction ()

#==========================================================
# rexglue_recomp_options(<options_var> <optimized_var>)
#
# Computes the compile options of the recompiled code for every configuration. Each option is
# wrapped in $<CONFIG:...>, so one configure serves a multi-config generator.
#
# The cache variables REXGLUE_RECOMP_OPTIMIZATION (empty, O0, O1, O2 or O3) and
# REXGLUE_RECOMP_DEBUG_INFO (empty, line-tables-only, full or none) apply to every configuration.
# A variable with a _DEBUG, _RELEASE or _RELWITHDEBINFO suffix applies to that configuration and
# wins over the plain one.
#
# With nothing set the defaults are
#   Debug           O1                   line-tables-only
#   Release         build type flags     none
#   RelWithDebInfo  build type flags     line-tables-only
#
# <options_var> receives the options. <optimized_var> receives ON when any configuration sets an
# optimization level.
#==========================================================
function (rexglue_recomp_options options_var optimized_var)
  _rexglue_recomp_configs(recomp_configs)
  # Guest SEH scopes emit __try/__except, which traps hardware faults only under asynchronous EH.
  # clang++ takes that flag in the GNU form.
  set(options "")
  if (WIN32)
    list(APPEND options -fasync-exceptions)
  else ()
    list(APPEND options -fasynchronous-unwind-tables)
  endif ()

  rexglue_recomp_declare_settings()

  set(optimized OFF)
  foreach (config IN LISTS recomp_configs)
    rexglue_recomp_resolve(${config} optimization debug_info)
    if (NOT optimization STREQUAL "")
      set(optimized ON)
    endif ()
    rexglue_recomp_flags(flags "${optimization}" ${debug_info})
    foreach (flag IN LISTS flags)
      list(APPEND options "$<$<CONFIG:${config}>:${flag}>")
    endforeach ()
  endforeach ()

  set(${options_var}
      "${options}"
      PARENT_SCOPE
  )
  set(${optimized_var}
      ${optimized}
      PARENT_SCOPE
  )
endfunction ()
