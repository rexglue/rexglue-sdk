cmake_minimum_required(VERSION 3.25)
project(sdl_consumer LANGUAGES NONE)

find_package(SDL3 CONFIG REQUIRED)

get_target_property(sdl_type SDL3::SDL3 TYPE)
get_target_property(sdl_aliased SDL3::SDL3 ALIASED_TARGET)
message(STATUS "SDL3::SDL3 type ${sdl_type}, aliased ${sdl_aliased}")
if (NOT sdl_type STREQUAL "SHARED_LIBRARY")
  message(FATAL_ERROR "SDL3::SDL3 is ${sdl_type}, expected SHARED_LIBRARY")
endif ()
