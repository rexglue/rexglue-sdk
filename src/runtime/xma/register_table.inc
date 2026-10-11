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

#ifndef REX_XMA_REGISTER
#define REX_XMA_REGISTER(index, name)
#define REX_XMA_REGISTER_UNSET
#endif

#ifndef REX_XMA_REGISTER_CONTEXT_GROUP
/**
 * Registers the same register for each of the 10 contexts, at consecutive indices.
 *
 * @param index The index of the Context0 register.
 * @param suffix The register name part appended after ContextN.
 *
 * @code
 * REX_XMA_REGISTER_CONTEXT_GROUP(0x0650, Kick)
 * @endcode
 */
#define REX_XMA_REGISTER_CONTEXT_GROUP(index, suffix) \
  REX_XMA_REGISTER((index) + 0, Context0##suffix)     \
  REX_XMA_REGISTER((index) + 1, Context1##suffix)     \
  REX_XMA_REGISTER((index) + 2, Context2##suffix)     \
  REX_XMA_REGISTER((index) + 3, Context3##suffix)     \
  REX_XMA_REGISTER((index) + 4, Context4##suffix)     \
  REX_XMA_REGISTER((index) + 5, Context5##suffix)     \
  REX_XMA_REGISTER((index) + 6, Context6##suffix)     \
  REX_XMA_REGISTER((index) + 7, Context7##suffix)     \
  REX_XMA_REGISTER((index) + 8, Context8##suffix)     \
  REX_XMA_REGISTER((index) + 9, Context9##suffix)
#endif

REX_XMA_REGISTER(0x0600, ContextArrayAddress)
REX_XMA_REGISTER(0x0606, CurrentContextIndex)
REX_XMA_REGISTER(0x0607, NextContextIndex)
REX_XMA_REGISTER_CONTEXT_GROUP(0x0610, Unknown610)
REX_XMA_REGISTER_CONTEXT_GROUP(0x0620, Unknown620)
REX_XMA_REGISTER_CONTEXT_GROUP(0x0650, Kick)
REX_XMA_REGISTER_CONTEXT_GROUP(0x0660, Unknown660)
REX_XMA_REGISTER_CONTEXT_GROUP(0x0690, Lock)
REX_XMA_REGISTER_CONTEXT_GROUP(0x06A0, Clear)

#ifdef REX_XMA_REGISTER_UNSET
#undef REX_XMA_REGISTER_UNSET
#undef REX_XMA_REGISTER
#endif
