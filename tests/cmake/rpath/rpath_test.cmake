# Builds the probe project with changing install folders and reads the run path of the host and the
# module. Run with -DREXGLUE_ROOT=<sdk root> -DWORK_DIR=<scratch folder> -DCXX_COMPILER=<compiler>
# -DNINJA=<ninja>.
if (NOT CMAKE_HOST_UNIX OR CMAKE_HOST_APPLE)
  message(STATUS "run paths are checked on Linux only")
  return()
endif ()
find_program(READELF readelf)
if (NOT READELF)
  message(FATAL_ERROR "readelf is needed to read the run path")
endif ()

set(PROBE ${REXGLUE_ROOT}/tests/cmake/rpath)
set(TREE ${WORK_DIR}/tree)

# run(<command...>) runs a command and fails on a bad exit.
function (run)
  execute_process(
    COMMAND ${ARGN}
    RESULT_VARIABLE code
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output
  )
  if (NOT code EQUAL 0)
    message(FATAL_ERROR "${ARGN} failed with ${code}\n${output}")
  endif ()
endfunction ()

# expect_rpath(<file> <run path>) checks the run path of a built file.
function (expect_rpath file expected)
  execute_process(COMMAND ${READELF} -d ${file} OUTPUT_VARIABLE dynamic)
  string(REGEX MATCH "(RPATH|RUNPATH)[^\n]*\\[([^]]*)\\]" found "${dynamic}")
  if (NOT found OR NOT CMAKE_MATCH_2 STREQUAL expected)
    message(FATAL_ERROR "${file} has the run path '${CMAKE_MATCH_2}', expected '${expected}'")
  endif ()
endfunction ()

# check(<run path> <settings...>) builds a fresh tree with the settings and checks both files.
function (check expected)
  file(REMOVE_RECURSE ${TREE})
  run(${CMAKE_COMMAND}
      -S
      ${PROBE}
      -B
      ${TREE}
      -G
      Ninja
      -DCMAKE_MAKE_PROGRAM=${NINJA}
      -DCMAKE_CXX_COMPILER=${CXX_COMPILER}
      -DREXGLUE_ROOT=${REXGLUE_ROOT}
      ${ARGN}
  )
  run(${NINJA} -C ${TREE})
  expect_rpath(${TREE}/probe_host "${expected}")
  expect_rpath(${TREE}/libprobe_module.so "${expected}")
endfunction ()

check("$ORIGIN:$ORIGIN/../lib")
check("$ORIGIN:$ORIGIN/../lib64" -DCMAKE_INSTALL_LIBDIR=lib64)
check("$ORIGIN:$ORIGIN/../lib/x86_64-linux-gnu" -DCMAKE_INSTALL_LIBDIR=lib/x86_64-linux-gnu)
check("$ORIGIN" -DCMAKE_INSTALL_BINDIR=. -DCMAKE_INSTALL_LIBDIR=.)
check("$ORIGIN:$ORIGIN/../lib" -DPROBE_SCOPED=ON)
