// Local shim replacing Mesa's util/macros.h for the vendored Direct3D 11
// reference tessellator (implement-metal-host-tessellation task 1.1). The
// tessellator uses exactly one symbol from that header: UNREACHABLE(), and
// only on CHLSLTessellator paths this project never reaches.
#pragma once
#include <cstdlib>
#ifndef UNREACHABLE
#define UNREACHABLE(msg) std::abort()
#endif
