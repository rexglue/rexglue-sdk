# Runs the benchmark cases of unit_tests with full sampling and writes one CSV file. The
# rextest_benchmark_all target runs it as
#   cmake -DUNIT_TESTS=<unit_tests> -DCONFIG=<config> -DOUTPUT_DIR=<folder> -DSOURCE_DIR=<folder>
#         -DFILTER=<Catch2 filter> -DSAMPLES=<count> -DSDK_VERSION=<version> -DCOMPILER=<text>
#         -P rextest_benchmark.cmake
# Catch2 prints its table to the terminal and writes the same results as XML. The CSV has one row
# per benchmark, in the order of the XML file, and its times are nanoseconds. Every row repeats the
# columns of the host, so files of many machines can be appended as they are. A run writes new files
# named after the commit, the configuration and the time, so runs never overwrite each other. The
# cpu column is the registry value of the processor name on Windows, the first model name line of
# /proc/cpuinfo on Linux and the sysctl brand string on macOS. The schema column is 1. Raise it
# whenever a column is added, removed or renamed.

if (NOT DEFINED CMAKE_SCRIPT_MODE_FILE)
  message(FATAL_ERROR "rextest_benchmark.cmake runs only as a script, with -P")
endif ()

foreach (
  _required IN
  ITEMS UNIT_TESTS
        CONFIG
        OUTPUT_DIR
        SOURCE_DIR
        FILTER
        SAMPLES
        SDK_VERSION
        COMPILER
)
  if (NOT DEFINED ${_required})
    message(
      FATAL_ERROR
        "usage: -DUNIT_TESTS=<exe> -DCONFIG=<config> -DOUTPUT_DIR=<folder> -DSOURCE_DIR=<folder> "
        "-DFILTER=<filter> -DSAMPLES=<count> -DSDK_VERSION=<version> -DCOMPILER=<text> "
        "-P rextest_benchmark.cmake"
    )
  endif ()
endforeach ()

if (NOT EXISTS "${UNIT_TESTS}")
  message(FATAL_ERROR "unit_tests is not built: ${UNIT_TESTS}")
endif ()

if (CONFIG STREQUAL "Debug")
  message(STATUS "Warning: Debug figures do not describe the speed of the SDK, use Release")
endif ()

set(COMMIT "")
find_program(GIT_TOOL git)
if (GIT_TOOL)
  execute_process(
    COMMAND "${GIT_TOOL}" rev-parse --short HEAD
    WORKING_DIRECTORY "${SOURCE_DIR}"
    OUTPUT_VARIABLE COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE GIT_RESULT
    ERROR_QUIET
  )
  if (NOT GIT_RESULT EQUAL 0)
    set(COMMIT "")
  endif ()
endif ()

string(TIMESTAMP RUN_UTC "%Y-%m-%dT%H:%M:%SZ" UTC)
string(REGEX REPLACE "[-:]" "" RUN_STAMP "${RUN_UTC}")
if (COMMIT STREQUAL "")
  set(NAME_COMMIT nogit)
else ()
  set(NAME_COMMIT "${COMMIT}")
endif ()
set(FILE_STEM "${OUTPUT_DIR}/rextest-benchmark-${NAME_COMMIT}-${CONFIG}-${RUN_STAMP}")
set(XML_FILE "${FILE_STEM}.xml")
set(CSV_FILE "${FILE_STEM}.csv")
file(MAKE_DIRECTORY "${OUTPUT_DIR}")

# The CPU name comes from the system, since the query of CMake gives a core count and a family text
set(HOST_CPU "")
if (CMAKE_HOST_WIN32)
  cmake_host_system_information(
    RESULT HOST_CPU QUERY WINDOWS_REGISTRY "HKLM/HARDWARE/DESCRIPTION/System/CentralProcessor/0"
                          VALUE "ProcessorNameString" ERROR_VARIABLE CPU_ERROR
  )
elseif (CMAKE_HOST_APPLE)
  execute_process(
    COMMAND sysctl -n machdep.cpu.brand_string
    OUTPUT_VARIABLE HOST_CPU
    ERROR_QUIET
  )
elseif (EXISTS /proc/cpuinfo)
  file(
    STRINGS /proc/cpuinfo CPU_LINES
    REGEX "^model name"
    LIMIT_COUNT 1
  )
  string(REGEX REPLACE "^[^:]*:" "" HOST_CPU "${CPU_LINES}")
endif ()
string(REGEX REPLACE " +" " " HOST_CPU "${HOST_CPU}")
string(STRIP "${HOST_CPU}" HOST_CPU)
if (HOST_CPU STREQUAL "")
  set(HOST_CPU unknown)
endif ()
cmake_host_system_information(RESULT HOST_LOGICAL_CORES QUERY NUMBER_OF_LOGICAL_CORES)
cmake_host_system_information(RESULT HOST_MEMORY_MB QUERY TOTAL_PHYSICAL_MEMORY)
cmake_host_system_information(RESULT HOST_OS QUERY OS_NAME)
cmake_host_system_information(RESULT HOST_OS_VERSION QUERY OS_RELEASE)
cmake_host_system_information(RESULT HOST_ARCH QUERY OS_PLATFORM)

get_filename_component(UNIT_TESTS_DIR "${UNIT_TESTS}" DIRECTORY)
set(ENV{SDL_ASSERT} always_ignore)
execute_process(
  COMMAND "${UNIT_TESTS}" "${FILTER}" --benchmark-samples "${SAMPLES}" --reporter console --reporter
          "xml::out=${XML_FILE}"
  WORKING_DIRECTORY "${UNIT_TESTS_DIR}"
  RESULT_VARIABLE RUN_RESULT
)
if (NOT RUN_RESULT EQUAL 0)
  message(
    FATAL_ERROR "unit_tests failed with ${RUN_RESULT} for the filter '${FILTER}' with no CSV file"
  )
endif ()
if (NOT EXISTS "${XML_FILE}")
  message(FATAL_ERROR "unit_tests wrote no XML file at ${XML_FILE}")
endif ()

# Returns the text of an XML attribute value with its entities decoded
function (decode_xml text out_var)
  string(REPLACE "&lt;" "<" text "${text}")
  string(REPLACE "&gt;" ">" text "${text}")
  string(REPLACE "&quot;" "\"" text "${text}")
  string(REPLACE "&apos;" "'" text "${text}")
  string(REPLACE "&amp;" "&" text "${text}")
  string(REPLACE "<LB>" "[" text "${text}")
  string(REPLACE "<RB>" "]" text "${text}")
  string(REPLACE "<SEMI>" ";" text "${text}")
  set(${out_var}
      "${text}"
      PARENT_SCOPE
  )
endfunction ()

# Returns a CSV field, quoted when it holds a comma, a quote, a line break or an outer space
function (csv_field text out_var)
  if (text MATCHES "[,\"\n\r]"
      OR text MATCHES "^ "
      OR text MATCHES " $"
  )
    string(REPLACE "\"" "\"\"" text "${text}")
    set(text "\"${text}\"")
  endif ()
  set(${out_var}
      "${text}"
      PARENT_SCOPE
  )
endfunction ()

# Returns the value of one attribute of an element line, empty when the line has none
function (read_attribute line attribute out_var)
  if (line MATCHES " ${attribute}=\"([^\"]*)\"")
    set(${out_var}
        "${CMAKE_MATCH_1}"
        PARENT_SCOPE
    )
  else ()
    set(${out_var}
        ""
        PARENT_SCOPE
    )
  endif ()
endfunction ()

# A list holds the lines, so the characters CMake reads as list syntax are replaced for the loop
file(READ "${XML_FILE}" XML_TEXT)
string(REPLACE "\r" "" XML_TEXT "${XML_TEXT}")
string(REPLACE ";" "<SEMI>" XML_TEXT "${XML_TEXT}")
string(REPLACE "[" "<LB>" XML_TEXT "${XML_TEXT}")
string(REPLACE "]" "<RB>" XML_TEXT "${XML_TEXT}")
string(REPLACE "\n" ";" XML_LINES "${XML_TEXT}")

string(ASCII 10 NL)
set(CSV_TEXT "schema,test_case,benchmark,samples,iterations,mean_ns,mean_low_ns,mean_high_ns,")
string(APPEND CSV_TEXT "std_dev_ns,config,commit,sdk_version,compiler,os,os_version,arch,cpu,")
string(APPEND CSV_TEXT "logical_cores,memory_mb,run_utc${NL}")
csv_field("${COMMIT}" CSV_COMMIT)
csv_field("${SDK_VERSION}" CSV_SDK_VERSION)
csv_field("${COMPILER}" CSV_COMPILER)
csv_field("${HOST_OS}" CSV_OS)
csv_field("${HOST_OS_VERSION}" CSV_OS_VERSION)
csv_field("${HOST_ARCH}" CSV_ARCH)
csv_field("${HOST_CPU}" CSV_CPU)
set(HOST_COLUMNS "${CONFIG},${CSV_COMMIT},${CSV_SDK_VERSION},${CSV_COMPILER},${CSV_OS},")
string(APPEND HOST_COLUMNS "${CSV_OS_VERSION},${CSV_ARCH},${CSV_CPU},")
string(APPEND HOST_COLUMNS "${HOST_LOGICAL_CORES},${HOST_MEMORY_MB},${RUN_UTC}")
set(ROW_COUNT 0)
set(TEST_CASE "")
set(IN_BENCHMARK FALSE)
foreach (line IN LISTS XML_LINES)
  if (line MATCHES "^ *<TestCase ")
    read_attribute("${line}" name TEST_CASE)
    decode_xml("${TEST_CASE}" TEST_CASE)
  elseif (line MATCHES "^ *<BenchmarkResults ")
    set(IN_BENCHMARK TRUE)
    read_attribute("${line}" name BENCH_NAME)
    read_attribute("${line}" samples BENCH_SAMPLES)
    read_attribute("${line}" iterations BENCH_ITERATIONS)
    decode_xml("${BENCH_NAME}" BENCH_NAME)
    set(BENCH_MEAN "")
    set(BENCH_MEAN_LOW "")
    set(BENCH_MEAN_HIGH "")
    set(BENCH_STD_DEV "")
  elseif (IN_BENCHMARK AND line MATCHES "^ *<failed ")
    message(FATAL_ERROR "benchmark '${BENCH_NAME}' in '${TEST_CASE}' failed")
  elseif (IN_BENCHMARK AND line MATCHES "^ *<mean ")
    read_attribute("${line}" value BENCH_MEAN)
    read_attribute("${line}" lowerBound BENCH_MEAN_LOW)
    read_attribute("${line}" upperBound BENCH_MEAN_HIGH)
  elseif (IN_BENCHMARK AND line MATCHES "^ *<standardDeviation ")
    read_attribute("${line}" value BENCH_STD_DEV)
  elseif (IN_BENCHMARK AND line MATCHES "^ *</BenchmarkResults>")
    set(IN_BENCHMARK FALSE)
    csv_field("${TEST_CASE}" _case)
    csv_field("${BENCH_NAME}" _name)
    string(APPEND CSV_TEXT "1,${_case},${_name},${BENCH_SAMPLES},${BENCH_ITERATIONS},")
    string(APPEND CSV_TEXT "${BENCH_MEAN},${BENCH_MEAN_LOW},${BENCH_MEAN_HIGH},${BENCH_STD_DEV},")
    string(APPEND CSV_TEXT "${HOST_COLUMNS}${NL}")
    math(EXPR ROW_COUNT "${ROW_COUNT} + 1")
  endif ()
endforeach ()

if (ROW_COUNT EQUAL 0)
  message(FATAL_ERROR "no benchmark results in ${XML_FILE} for the filter '${FILTER}'")
endif ()

file(WRITE "${CSV_FILE}" "${CSV_TEXT}")

message(STATUS "csv: ${CSV_FILE}")
message(STATUS "rows: ${ROW_COUNT}")
