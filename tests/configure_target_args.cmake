# Checks the arguments rexglue_configure_target refuses. Run with
# cmake -DREXGLUE_ROOT=<root> -DARGS=<arguments, as a list> -P.
cmake_minimum_required(VERSION 3.25)

include(${REXGLUE_ROOT}/cmake/rexglue_target.cmake)

rexglue_configure_target(unused ${ARGS})
