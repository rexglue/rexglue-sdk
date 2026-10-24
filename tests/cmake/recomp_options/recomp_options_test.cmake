# Configures and builds the probe project in every configuration with changing recompiled option
# settings. Run with -DREXGLUE_ROOT=<sdk root> -DWORK_DIR=<scratch folder> -DCXX_COMPILER=<compiler>
# -DNINJA=<ninja>.
set(PROBE ${REXGLUE_ROOT}/tests/cmake/recomp_options)
set(TREE ${WORK_DIR}/tree)
set(CONFIGS Debug Release RelWithDebInfo)

# configure(<result> <settings...>) configures a fresh tree, keeps the output in <result> and
# returns the exit code in configure_code.
function (configure result)
  file(REMOVE_RECURSE ${TREE})
  execute_process(
    COMMAND
      ${CMAKE_COMMAND} -S ${PROBE} -B ${TREE} -G "Ninja Multi-Config" -DCMAKE_MAKE_PROGRAM=${NINJA}
      -DCMAKE_CXX_COMPILER=${CXX_COMPILER} -DREXGLUE_ROOT=${REXGLUE_ROOT} ${ARGN}
    RESULT_VARIABLE code
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output
  )
  set(${result}
      "${output}"
      PARENT_SCOPE
  )
  set(configure_code
      ${code}
      PARENT_SCOPE
  )
endfunction ()

# compile_line(<result> <config>) builds the probe object in a configuration and returns the
# compile command.
function (compile_line result config)
  execute_process(
    COMMAND ${CMAKE_COMMAND} --build ${TREE} --config ${config} -- -v
    RESULT_VARIABLE code
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output
  )
  if (NOT code EQUAL 0)
    message(FATAL_ERROR "the ${config} build failed with ${code}\n${output}")
  endif ()
  set(${result}
      "${output}"
      PARENT_SCOPE
  )
endfunction ()

# expect(<config> <flag> <present>) checks that a flag is on or off the compile command of a
# configuration.
function (expect config flag present)
  string(REGEX MATCH "[ ]${flag}[ ]" found "${line_${config}}")
  if (present AND NOT found)
    message(FATAL_ERROR "${config} should compile with ${flag}\n${line_${config}}")
  elseif (NOT present AND found)
    message(FATAL_ERROR "${config} should not compile with ${flag}\n${line_${config}}")
  endif ()
endfunction ()

# build_all(<settings...>) configures with the settings and records the compile command of every
# configuration in line_<config>.
macro (build_all)
  configure(ignored ${ARGN})
  if (NOT configure_code EQUAL 0)
    message(FATAL_ERROR "configure failed with ${configure_code}\n${ignored}")
  endif ()
  foreach (config IN LISTS CONFIGS)
    compile_line(line_${config} ${config})
  endforeach ()
endmacro ()

# With nothing set Debug takes O1 and line tables, Release takes no debug info and
# RelWithDebInfo takes line tables with the build type optimization.
build_all()
expect(Debug -O1 TRUE)
expect(Debug -gline-tables-only TRUE)
expect(Release -O1 FALSE)
expect(Release -g0 TRUE)
expect(Release -gline-tables-only FALSE)
expect(RelWithDebInfo -O1 FALSE)
expect(RelWithDebInfo -g0 FALSE)
expect(RelWithDebInfo -gline-tables-only TRUE)

# The plain settings apply to every configuration.
build_all(-DREXGLUE_RECOMP_OPTIMIZATION=O0 -DREXGLUE_RECOMP_DEBUG_INFO=none)
foreach (config IN LISTS CONFIGS)
  expect(${config} -O0 TRUE)
  expect(${config} -O1 FALSE)
  expect(${config} -g0 TRUE)
  expect(${config} -gline-tables-only FALSE)
endforeach ()

# A setting for one configuration wins over the plain one and leaves the others alone.
build_all(-DREXGLUE_RECOMP_OPTIMIZATION=O0 -DREXGLUE_RECOMP_OPTIMIZATION_RELEASE=O1
          -DREXGLUE_RECOMP_DEBUG_INFO_DEBUG=full
)
expect(Release -O1 TRUE)
expect(Release -O0 FALSE)
expect(Debug -O0 TRUE)
expect(Debug -gline-tables-only FALSE)
expect(RelWithDebInfo -O0 TRUE)
expect(RelWithDebInfo -gline-tables-only TRUE)

# A bad value stops the configure and names the setting.
configure(output -DREXGLUE_RECOMP_OPTIMIZATION_RELEASE=O9)
if (configure_code EQUAL 0 OR NOT output MATCHES "must be empty or one of O0, O1, O2, O3")
  message(FATAL_ERROR "a bad optimization level was accepted\n${output}")
endif ()
configure(output -DREXGLUE_RECOMP_DEBUG_INFO=huge)
if (configure_code EQUAL 0 OR NOT output MATCHES "must be empty, line-tables-only, full or none")
  message(FATAL_ERROR "a bad debug info level was accepted\n${output}")
endif ()

# Under cl style flags the mapping holds with CMP0184 set, and without it the Debug default is
# skipped and an explicit level is an error. The policy exists from CMake 4.0.
if (POLICY CMP0184)
  configure(output -DPROBE_MSVC=ON -DPROBE_POLICY=ON -DREXGLUE_RECOMP_OPTIMIZATION_DEBUG=O0
            -DREXGLUE_RECOMP_OPTIMIZATION_RELEASE=O1
            -DREXGLUE_RECOMP_OPTIMIZATION_RELWITHDEBINFO=O2
  )
  if (NOT configure_code EQUAL 0)
    message(FATAL_ERROR "the cl style configure failed\n${output}")
  endif ()
  foreach (expected "$<$<CONFIG:DEBUG>:/Od>" "$<$<CONFIG:RELEASE>:/O2>" "$<$<CONFIG:RELEASE>:/Ob1>"
                    "$<$<CONFIG:RELWITHDEBINFO>:/Ob2>" "/EHa"
  )
    string(FIND "${output}" "${expected}" at)
    if (at EQUAL -1)
      message(FATAL_ERROR "the cl style options lack ${expected}\n${output}")
    endif ()
  endforeach ()

  configure(output -DPROBE_MSVC=ON)
  if (NOT configure_code EQUAL 0
      OR output MATCHES "CONFIG:DEBUG>:/O"
      OR NOT output MATCHES "RECOMP_OPTIMIZED=OFF"
  )
    message(FATAL_ERROR "the cl style default without the policy was not skipped\n${output}")
  endif ()

  configure(output -DPROBE_MSVC=ON -DREXGLUE_RECOMP_OPTIMIZATION=O1)
  if (configure_code EQUAL 0 OR NOT output MATCHES "CMP0184")
    message(FATAL_ERROR "an explicit level without the policy was accepted\n${output}")
  endif ()
endif ()
