cmake_minimum_required(VERSION 3.25)
project(build_nonce_probe NONE)

include(${REXGLUE_ROOT}/cmake/rexglue_build_nonce.cmake)
rexglue_get_build_nonce(nonce)
file(WRITE ${CMAKE_BINARY_DIR}/nonce.txt "${nonce}")
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/identity.h.in ${CMAKE_BINARY_DIR}/identity.h @ONLY)
