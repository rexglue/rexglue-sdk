/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <cstddef>

// Enable inline implementations and advanced API (XXH3)
#ifndef XXH_INLINE_ALL
#define XXH_INLINE_ALL
#endif

// Can't use XXH_X86DISPATCH because XXH is calculated on multiple threads,
// while the dispatch writes the result (multiple pointers without any
// synchronization) to XXH_g_dispatch at the first call.

#include <xxhash.h>

namespace rex {

// For use in unordered_sets and unordered_maps (primarily multisets and
// multimaps, with manual collision resolution), where the hash is calculated
// externally (for instance, as XXH3), possibly requiring context data rather
// than a pure function to calculate the hash
template <typename Key>
struct IdentityHasher {
  size_t operator()(const Key& key) const { return static_cast<size_t>(key); }
};

template <typename Key>
struct XXHasher {
  size_t operator()(const Key& key) const {
    return static_cast<size_t>(XXH3_64bits(&key, sizeof(key)));
  }
};

}  // namespace rex
