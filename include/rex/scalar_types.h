/**
 * @file        scalar_types.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>

namespace rex {

/** Unsigned 8 bit integer. */
using u8 = uint8_t;
/** Unsigned 16 bit integer. */
using u16 = uint16_t;
/** Unsigned 32 bit integer. */
using u32 = uint32_t;
/** Unsigned 64 bit integer. */
using u64 = uint64_t;

/** Signed 8 bit integer. */
using i8 = int8_t;
/** Signed 16 bit integer. */
using i16 = int16_t;
/** Signed 32 bit integer. */
using i32 = int32_t;
/** Signed 64 bit integer. */
using i64 = int64_t;

/** 32 bit IEEE 754 float. */
using f32 = float;
/** 64 bit IEEE 754 float. */
using f64 = double;

static_assert(sizeof(f32) == 4, "f32 must be 4 bytes");
static_assert(sizeof(f64) == 8, "f64 must be 8 bytes");

}  // namespace rex
