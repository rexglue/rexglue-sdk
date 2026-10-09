/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

// Partial file included by other files to construct tables.

#ifndef XE_XMA_REGISTER
#define XE_XMA_REGISTER(index, name)
#define __XE_XMA_REGISTER_UNSET
#endif

#ifndef XE_XMA_REGISTER_CONTEXT_GROUP
/**
 * Registers the same register for each of the 10 contexts, at consecutive indices.
 *
 * @param index The index of the Context0 register.
 * @param suffix The register name part appended after ContextN.
 *
 * @code
 * XE_XMA_REGISTER_CONTEXT_GROUP(0x0650, Kick)
 * @endcode
 */
#define XE_XMA_REGISTER_CONTEXT_GROUP(index, suffix) \
  XE_XMA_REGISTER(index + 0, Context0##suffix)       \
  XE_XMA_REGISTER(index + 1, Context1##suffix)       \
  XE_XMA_REGISTER(index + 2, Context2##suffix)       \
  XE_XMA_REGISTER(index + 3, Context3##suffix)       \
  XE_XMA_REGISTER(index + 4, Context4##suffix)       \
  XE_XMA_REGISTER(index + 5, Context5##suffix)       \
  XE_XMA_REGISTER(index + 6, Context6##suffix)       \
  XE_XMA_REGISTER(index + 7, Context7##suffix)       \
  XE_XMA_REGISTER(index + 8, Context8##suffix)       \
  XE_XMA_REGISTER(index + 9, Context9##suffix)
#endif

XE_XMA_REGISTER(0x0600, ContextArrayAddress)
XE_XMA_REGISTER(0x0606, CurrentContextIndex)
XE_XMA_REGISTER(0x0607, NextContextIndex)
XE_XMA_REGISTER_CONTEXT_GROUP(0x0610, Unknown610)
XE_XMA_REGISTER_CONTEXT_GROUP(0x0620, Unknown620)
XE_XMA_REGISTER_CONTEXT_GROUP(0x0650, Kick)
XE_XMA_REGISTER_CONTEXT_GROUP(0x0660, Unknown660)
XE_XMA_REGISTER_CONTEXT_GROUP(0x0690, Lock)
XE_XMA_REGISTER_CONTEXT_GROUP(0x06A0, Clear)

#ifdef __XE_XMA_REGISTER_UNSET
#undef __XE_XMA_REGISTER_UNSET
#undef XE_XMA_REGISTER
#endif
