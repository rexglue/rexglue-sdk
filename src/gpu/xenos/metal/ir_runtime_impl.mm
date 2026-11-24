/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Metal Shader Converter ships its runtime helpers (IRRuntimeDraw*,
// IRDescriptorTableSet*, ...) as a single-header library: exactly one
// translation unit must define IR_PRIVATE_IMPLEMENTATION before including the
// header to emit the implementations. This is that TU. ARC is required here (it
// is on for all OBJCXX in this build) and the ObjC (id<MTL...>) type path is
// used (IR_RUNTIME_METALCPP is intentionally left undefined).

#import <Metal/Metal.h>

#define IR_PRIVATE_IMPLEMENTATION
#include "metal_irconverter_runtime.h"
