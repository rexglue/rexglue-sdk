# Configures the probe project twice in one tree and once in another, then checks the nonce.
# Run with -DREXGLUE_ROOT=<sdk root> -DWORK_DIR=<scratch folder>.
set(PROBE ${REXGLUE_ROOT}/tests/cmake/build_nonce)
file(REMOVE_RECURSE ${WORK_DIR})

# configure(<tree>) configures the probe project in the folder <tree> of the work dir.
function (configure tree)
  execute_process(
    COMMAND ${CMAKE_COMMAND} -S ${PROBE} -B ${WORK_DIR}/${tree} -DREXGLUE_ROOT=${REXGLUE_ROOT}
    RESULT_VARIABLE result
    OUTPUT_QUIET
    ERROR_VARIABLE error
  )
  if (NOT result EQUAL 0)
    message(FATAL_ERROR "configuring ${tree} failed: ${error}")
  endif ()
endfunction ()

# read_nonce(<tree> <out>) stores the nonce the probe wrote in <tree> into <out>.
function (read_nonce tree out)
  file(READ ${WORK_DIR}/${tree}/nonce.txt text)
  set(${out}
      "${text}"
      PARENT_SCOPE
  )
endfunction ()

configure(a)
read_nonce(a first)
file(TIMESTAMP ${WORK_DIR}/a/identity.h first_header "%s")
execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 1.1)
configure(a)
read_nonce(a second)
file(TIMESTAMP ${WORK_DIR}/a/identity.h second_header "%s")
configure(b)
read_nonce(b other)

if (NOT first MATCHES "^[0-9a-f]+$" OR NOT other MATCHES "^[0-9a-f]+$")
  message(FATAL_ERROR "the nonce is not hex: '${first}' '${other}'")
endif ()
if (NOT first STREQUAL second)
  message(FATAL_ERROR "a reconfigure changed the nonce from ${first} to ${second}")
endif ()
if (NOT first_header STREQUAL second_header)
  message(FATAL_ERROR "a reconfigure rewrote an unchanged header")
endif ()
if (first STREQUAL other)
  message(FATAL_ERROR "two build trees share the nonce ${first}")
endif ()
