# Checks that rexglue_configure_target refuses the removed staging arguments. Run with
# cmake -DREXGLUE_ROOT=<root> -P.
include(${REXGLUE_ROOT}/cmake/rexglue_target.cmake)

rexglue_configure_target(unused GPU_PLUGINS xenos)
