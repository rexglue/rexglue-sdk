cmake_minimum_required(VERSION 3.25)
project(probe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (PROBE_SCOPED)
  add_subdirectory(scope)
else ()
  include(${REXGLUE_ROOT}/cmake/rexglue_module.cmake)
endif ()

add_library(probe_module SHARED probe.cpp)
target_include_directories(probe_module PRIVATE ${REXGLUE_ROOT}/include)
rexglue_configure_module_target(probe_module)

add_executable(probe_host main.cpp)
rexglue_set_rpath(probe_host "$ORIGIN")
