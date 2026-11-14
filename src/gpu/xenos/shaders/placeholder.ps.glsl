/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#version 460

// Minimal placeholder pixel shader for async pipeline hot-swap.
// Discarding avoids transient black flashes while real shaders are compiling.
layout(location = 0) out vec4 oC0;

void main() {
  discard;
}
