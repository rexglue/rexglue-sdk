# Installs the SDK from a built tree and checks that the package ships SDL3 as a shared library and
# that a consumer resolves SDL3::SDL3 to it. Run with -DREXGLUE_ROOT=<sdk root> -DBUILD_DIR=<build
# tree> -DWORK_DIR=<scratch folder> -DNINJA=<ninja>.
set(PREFIX ${WORK_DIR}/prefix)
set(CONSUMER ${WORK_DIR}/consumer)

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

file(REMOVE_RECURSE ${WORK_DIR})
run(${CMAKE_COMMAND}
    --install
    ${BUILD_DIR}
    --config
    Release
    --prefix
    ${PREFIX}
)

if (CMAKE_HOST_WIN32)
  file(GLOB shared ${PREFIX}/bin/SDL3.dll)
  file(GLOB static ${PREFIX}/lib/SDL3-static.lib)
elseif (CMAKE_HOST_APPLE)
  file(GLOB shared ${PREFIX}/lib/libSDL3*.dylib)
  file(GLOB static ${PREFIX}/lib/libSDL3.a)
else ()
  file(GLOB shared ${PREFIX}/lib*/libSDL3.so*)
  file(GLOB static ${PREFIX}/lib*/libSDL3.a)
endif ()
if (NOT shared)
  message(FATAL_ERROR "the install ships no shared SDL3 under ${PREFIX}")
endif ()
if (static)
  message(FATAL_ERROR "the install ships a static SDL3: ${static}")
endif ()

run(${CMAKE_COMMAND}
    -S
    ${REXGLUE_ROOT}/tests/cmake/sdl_shared
    -B
    ${CONSUMER}
    -G
    Ninja
    -DCMAKE_MAKE_PROGRAM=${NINJA}
    -DCMAKE_PREFIX_PATH=${PREFIX}
)
