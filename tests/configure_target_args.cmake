# Checks the arguments rexglue_configure_target refuses. Run with
# cmake -DREXGLUE_ROOT=<root> -DARGS=<arguments, as a list> -P.
include(${REXGLUE_ROOT}/cmake/rexglue_target.cmake)

rexglue_configure_target(unused ${ARGS})
