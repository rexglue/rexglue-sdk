/**
 * @file        runtime/guest/memory.h
 * @brief       PPC memory load/store operations, SIMD intrinsics, and byte swapping
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Based on XenonRecomp/UnleashedRecomp memory access patterns
 *              and simde implementation
 */

#pragma once

#include <array>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <simde/x86/avx.h>
#include <simde/x86/sse.h>
#include <simde/x86/sse4.1.h>

#include <rex/runtime/mmio_handler.h>

//=============================================================================
// Physical Heap Offset (Windows Granularity Workaround)
//=============================================================================
// On Windows, allocation granularity is 64KB, so the 0x1000 file offset for
// the 0xE0 physical heap gets masked away. We compensate by adding 0x1000
// to host addresses when the guest address is >= 0xE0000000.
//=============================================================================
// TODO(tomc): there has to be a better way to handle this than shlittering PPC_PHYS_HOST_OFFSET()
// everywhere and adding it to every single memory access.
// Maybe a separate base pointer for the 0xE0 heap?
//=============================================================================
#ifdef _WIN32
#define PPC_PHYS_HOST_OFFSET(addr) (((uint32_t)(addr) >= 0xE0000000u) ? 0x1000u : 0u)
#else
#define PPC_PHYS_HOST_OFFSET(addr) 0u  // Linux has 4KB granularity, file offset works
#endif

// Raw address calculation with physical offset (for operations that don't use PPC_LOAD/PPC_STORE)
#define PPC_RAW_ADDR(x) (base + (uint32_t)(x) + PPC_PHYS_HOST_OFFSET(x))

//=============================================================================
// Load Macros (Big-Endian to Host)
//=============================================================================

#ifndef PPC_LOAD_U8
#define PPC_LOAD_U8(x)  (*(volatile uint8_t*)(base + (uint32_t)(x) + PPC_PHYS_HOST_OFFSET(x)))
#endif

#ifndef PPC_LOAD_U16
#define PPC_LOAD_U16(x) __builtin_bswap16(*(volatile uint16_t*)(base + (uint32_t)(x) + PPC_PHYS_HOST_OFFSET(x)))
#endif

#ifndef PPC_LOAD_U32
#define PPC_LOAD_U32(x) __builtin_bswap32(*(volatile uint32_t*)(base + (uint32_t)(x) + PPC_PHYS_HOST_OFFSET(x)))
#endif

#ifndef PPC_LOAD_U64
#define PPC_LOAD_U64(x) __builtin_bswap64(*(volatile uint64_t*)(base + (uint32_t)(x) + PPC_PHYS_HOST_OFFSET(x)))
#endif

#ifndef PPC_LOAD_STRING
#define PPC_LOAD_STRING(x, len) std::string_view(reinterpret_cast<const char*>(base + (uint32_t)(x) + PPC_PHYS_HOST_OFFSET(x)), (len))
#endif

//=============================================================================
// Store Macros (Host to Big-Endian)
//=============================================================================

#ifndef PPC_STORE_U8
#define PPC_STORE_U8(x, y)  (*(volatile uint8_t*)(base + (uint32_t)(x) + PPC_PHYS_HOST_OFFSET(x)) = (y))
#endif

#ifndef PPC_STORE_U16
#define PPC_STORE_U16(x, y) (*(volatile uint16_t*)(base + (uint32_t)(x) + PPC_PHYS_HOST_OFFSET(x)) = __builtin_bswap16(y))
#endif

#ifndef PPC_STORE_U32
#define PPC_STORE_U32(x, y) (*(volatile uint32_t*)(base + (uint32_t)(x) + PPC_PHYS_HOST_OFFSET(x)) = __builtin_bswap32(y))
#endif

#ifndef PPC_STORE_U64
#define PPC_STORE_U64(x, y) (*(volatile uint64_t*)(base + (uint32_t)(x) + PPC_PHYS_HOST_OFFSET(x)) = __builtin_bswap64(y))
#endif

//=============================================================================
// Memory Size Constant
//=============================================================================

#define PPC_MEMORY_SIZE 0x100000000ull

//=============================================================================
// MMIO Address Range (per Xenia memory.cc)
//=============================================================================
// Xbox 360 memory map:
//   0x7F000000 - 0x7FFFFFFF - MMIO (GPU registers at 0x7FC80000, audio, etc.)
//   0xA0000000 - 0xBFFFFFFF - physical 64k pages
//   0xC0000000 - 0xDFFFFFFF - physical 16mb pages
//   0xE0000000 - 0xFFFFFFFF - physical 4k pages
// NOTE: 0xC0000000+ is PHYSICAL MEMORY, not MMIO!

#define PPC_IS_MMIO_ADDR(addr) ((addr) >= 0x7F000000u && (addr) < 0x80000000u)

//=============================================================================
// MMIO Store Macros
//=============================================================================
// If the address is in MMIO range, dispatch to MMIOHandler.
// Otherwise, perform a regular memory store with physical offset compensation.

#define PPC_MM_STORE_U8(addr, val) do { \
    uint32_t _mmio_addr = (addr); \
    if (PPC_IS_MMIO_ADDR(_mmio_addr)) { \
        rex::runtime::MMIOHandler::global_handler()->CheckStore(_mmio_addr, static_cast<uint32_t>(val)); \
    } else { \
        *(volatile uint8_t*)(base + _mmio_addr + PPC_PHYS_HOST_OFFSET(_mmio_addr)) = (val); \
    } \
} while(0)

#define PPC_MM_STORE_U16(addr, val) do { \
    uint32_t _mmio_addr = (addr); \
    if (PPC_IS_MMIO_ADDR(_mmio_addr)) { \
        rex::runtime::MMIOHandler::global_handler()->CheckStore(_mmio_addr, static_cast<uint32_t>(val)); \
    } else { \
        *(volatile uint16_t*)(base + _mmio_addr + PPC_PHYS_HOST_OFFSET(_mmio_addr)) = __builtin_bswap16(val); \
    } \
} while(0)

#define PPC_MM_STORE_U32(addr, val) do { \
    uint32_t _mmio_addr = (addr); \
    if (PPC_IS_MMIO_ADDR(_mmio_addr)) { \
        rex::runtime::MMIOHandler::global_handler()->CheckStore(_mmio_addr, static_cast<uint32_t>(val)); \
    } else { \
        *(volatile uint32_t*)(base + _mmio_addr + PPC_PHYS_HOST_OFFSET(_mmio_addr)) = __builtin_bswap32(val); \
    } \
} while(0)

#define PPC_MM_STORE_U64(addr, val) do { \
    uint32_t _mmio_addr = (addr); \
    if (PPC_IS_MMIO_ADDR(_mmio_addr)) { \
        uint64_t _v64 = static_cast<uint64_t>(val); \
        rex::runtime::MMIOHandler::global_handler()->CheckStore(_mmio_addr, static_cast<uint32_t>(_v64 >> 32)); \
        rex::runtime::MMIOHandler::global_handler()->CheckStore(_mmio_addr + 4, static_cast<uint32_t>(_v64)); \
    } else { \
        *(volatile uint64_t*)(base + _mmio_addr + PPC_PHYS_HOST_OFFSET(_mmio_addr)) = __builtin_bswap64(val); \
    } \
} while(0)

//=============================================================================
// MMIO Load Macros
//=============================================================================
// If the address is in MMIO range, dispatch to MMIOHandler.
// Otherwise, perform a regular memory load with physical offset compensation.

#define PPC_MM_LOAD_U8(addr) \
    (PPC_IS_MMIO_ADDR(addr) ? \
        ({ uint32_t _v; rex::runtime::MMIOHandler::global_handler()->CheckLoad(addr, &_v); static_cast<uint8_t>(_v); }) : \
        *(volatile uint8_t*)(base + (addr) + PPC_PHYS_HOST_OFFSET(addr)))

#define PPC_MM_LOAD_U16(addr) \
    (PPC_IS_MMIO_ADDR(addr) ? \
        ({ uint32_t _v; rex::runtime::MMIOHandler::global_handler()->CheckLoad(addr, &_v); static_cast<uint16_t>(_v); }) : \
        __builtin_bswap16(*(volatile uint16_t*)(base + (addr) + PPC_PHYS_HOST_OFFSET(addr))))

#define PPC_MM_LOAD_U32(addr) \
    (PPC_IS_MMIO_ADDR(addr) ? \
        ({ uint32_t _v; rex::runtime::MMIOHandler::global_handler()->CheckLoad(addr, &_v); _v; }) : \
        __builtin_bswap32(*(volatile uint32_t*)(base + (addr) + PPC_PHYS_HOST_OFFSET(addr))))

#define PPC_MM_LOAD_U64(addr) \
    (PPC_IS_MMIO_ADDR(addr) ? \
        ({ uint32_t _hi, _lo; \
           rex::runtime::MMIOHandler::global_handler()->CheckLoad(addr, &_hi); \
           rex::runtime::MMIOHandler::global_handler()->CheckLoad((addr) + 4, &_lo); \
           (static_cast<uint64_t>(_hi) << 32) | _lo; }) : \
        __builtin_bswap64(*(volatile uint64_t*)(base + (addr) + PPC_PHYS_HOST_OFFSET(addr))))

namespace rex::runtime::guest {

//=============================================================================
// Vector Load/Store Mask Tables
//=============================================================================
// These tables are used for lvlx/lvrx (load vector left/right) and
// stvlx/stvrx (store vector left/right) instructions.

inline uint8_t VectorMaskL[] = {
    0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00,
    0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
    0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02,
    0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03,
    0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F,
};

inline uint8_t VectorMaskR[] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
    0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF,
    0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF,
    0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF,
};

inline uint8_t VectorShiftTableL[] = {
    0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00,
    0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
    0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02,
    0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03,
    0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04,
    0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05,
    0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06,
    0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07,
    0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08,
    0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09,
    0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A,
    0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B,
    0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C,
    0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D,
    0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E,
    0x1E, 0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F,
};

inline uint8_t VectorShiftTableR[] = {
    0x1F, 0x1E, 0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10,
    0x1E, 0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F,
    0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E,
    0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D,
    0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C,
    0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B,
    0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A,
    0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09,
    0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08,
    0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07,
    0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06,
    0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05,
    0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04,
    0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03,
    0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02,
    0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
};

//=============================================================================
// FRSQRTE Lookup Table
//=============================================================================
// Credit: RPCS3 - https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/PPUInterpreter.cpp

constexpr uint32_t ppu_frsqrte_mantissas[16] = {
    0x000f1000u, 0x000d8000u, 0x000c0000u, 0x000a8000u,
    0x00098000u, 0x00088000u, 0x00080000u, 0x00070000u,
    0x00060000u, 0x0004c000u, 0x0003c000u, 0x00030000u,
    0x00020000u, 0x00018000u, 0x00010000u, 0x00008000u,
};

struct FrsqrteLUT {
    std::array<uint32_t, 0x8000> data{};

    constexpr FrsqrteLUT() {
        for (uint32_t i = 0; i < 0x8000; i++) {
            const uint32_t sign = (i >> 14) & 1;
            const uint32_t expv = i & 0x3FFF;

            if (sign) {
                data[i] = 0x7FF80000u;
            } else if (expv == 0) {
                data[i] = 0x7FF00000u;
            } else if (expv >= 0x3FE0) {
                data[i] = 0;
            } else {
                const uint32_t exp = 0x3FE00000u - (((expv + 0x1C01) >> 1) << 20);
                const uint32_t idx = 8 ^ (i & 0xF);
                data[i] = exp | ppu_frsqrte_mantissas[idx];
            }
        }
    }
};

inline constexpr FrsqrteLUT ppu_frsqrte_lut{};

//=============================================================================
// SIMD Helper Functions
//=============================================================================

// Unsigned 32-bit saturating add
inline simde__m128i simde_mm_adds_epu32(simde__m128i a, simde__m128i b) {
    return simde_mm_add_epi32(a, simde_mm_min_epu32(simde_mm_xor_si128(a, simde_mm_cmpeq_epi32(a, a)), b));
}

// Signed 8-bit average (rounds towards zero)
inline simde__m128i simde_mm_avg_epi8(simde__m128i a, simde__m128i b) {
    simde__m128i c = simde_mm_set1_epi8(char(128));
    return simde_mm_xor_si128(c, simde_mm_avg_epu8(simde_mm_xor_si128(c, a), simde_mm_xor_si128(c, b)));
}

// Signed 16-bit average
inline simde__m128i simde_mm_avg_epi16(simde__m128i a, simde__m128i b) {
    simde__m128i c = simde_mm_set1_epi16(short(32768));
    return simde_mm_xor_si128(c, simde_mm_avg_epu16(simde_mm_xor_si128(c, a), simde_mm_xor_si128(c, b)));
}

// Convert unsigned 32-bit integers to floats
inline simde__m128 simde_mm_cvtepu32_ps_(simde__m128i src1) {
    simde__m128i xmm1 = simde_mm_add_epi32(src1, simde_mm_set1_epi32(127));
    simde__m128i xmm0 = simde_mm_slli_epi32(src1, 31 - 8);
    xmm0 = simde_mm_srli_epi32(xmm0, 31);
    xmm0 = simde_mm_add_epi32(xmm0, xmm1);
    xmm0 = simde_mm_srai_epi32(xmm0, 8);
    xmm0 = simde_mm_add_epi32(xmm0, simde_mm_set1_epi32(0x4F800000));
    simde__m128 xmm2 = simde_mm_cvtepi32_ps(src1);
    return simde_mm_blendv_ps(xmm2, simde_mm_castsi128_ps(xmm0), simde_mm_castsi128_ps(src1));
}

// Permute bytes from two vectors based on control vector
inline simde__m128i simde_mm_perm_epi8_(simde__m128i a, simde__m128i b, simde__m128i c) {
    simde__m128i d = simde_mm_set1_epi8(0xF);
    simde__m128i e = simde_mm_sub_epi8(d, simde_mm_and_si128(c, d));
    return simde_mm_blendv_epi8(simde_mm_shuffle_epi8(a, e), simde_mm_shuffle_epi8(b, e), simde_mm_slli_epi32(c, 3));
}

// Unsigned 8-bit compare greater than
inline simde__m128i simde_mm_cmpgt_epu8(simde__m128i a, simde__m128i b) {
    simde__m128i c = simde_mm_set1_epi8(char(128));
    return simde_mm_cmpgt_epi8(simde_mm_xor_si128(a, c), simde_mm_xor_si128(b, c));
}

// Unsigned 16-bit compare greater than
inline simde__m128i simde_mm_cmpgt_epu16(simde__m128i a, simde__m128i b) {
    simde__m128i c = simde_mm_set1_epi16(short(32768));
    return simde_mm_cmpgt_epi16(simde_mm_xor_si128(a, c), simde_mm_xor_si128(b, c));
}

// Vector Convert To Signed Fixed-Point Word Saturate
inline simde__m128i simde_mm_vctsxs(simde__m128 src1) {
    simde__m128 xmm2 = simde_mm_cmpunord_ps(src1, src1);
    simde__m128i xmm0 = simde_mm_cvttps_epi32(src1);
    simde__m128i xmm1 = simde_mm_cmpeq_epi32(xmm0, simde_mm_set1_epi32(INT_MIN));
    xmm1 = simde_mm_andnot_si128(simde_mm_castps_si128(src1), xmm1);
    simde__m128 dest = simde_mm_blendv_ps(simde_mm_castsi128_ps(xmm0),
        simde_mm_castsi128_ps(simde_mm_set1_epi32(INT_MAX)), simde_mm_castsi128_ps(xmm1));
    return simde_mm_andnot_si128(simde_mm_castps_si128(xmm2), simde_mm_castps_si128(dest));
}

// Vector Convert To Unsigned Fixed-Point Word Saturate
// Convert float to unsigned int with saturation to [0, UINT_MAX]
// NaN -> 0, negative -> 0, > UINT_MAX -> UINT_MAX
inline simde__m128i simde_mm_vctuxs(simde__m128 src1) {
    simde__m128 nan_mask = simde_mm_cmpunord_ps(src1, src1);
    simde__m128 neg_mask = simde_mm_cmplt_ps(src1, simde_mm_setzero_ps());
    simde__m128 max_val = simde_mm_set1_ps(4294967295.0f);  // UINT_MAX as float
    simde__m128 overflow_mask = simde_mm_cmpge_ps(src1, max_val);

    // Clamp to [0, UINT_MAX]
    simde__m128 clamped = simde_mm_max_ps(src1, simde_mm_setzero_ps());
    clamped = simde_mm_min_ps(clamped, max_val);

    // Convert to signed int first (will handle values up to INT_MAX correctly)
    // For values > INT_MAX, we need special handling
    simde__m128 half_range = simde_mm_set1_ps(2147483648.0f);  // 2^31
    simde__m128 high_bit_mask = simde_mm_cmpge_ps(clamped, half_range);

    // For values >= 2^31, subtract 2^31 before conversion and add it back after
    simde__m128 adjusted = simde_mm_sub_ps(clamped, simde_mm_and_ps(high_bit_mask, half_range));
    simde__m128i low_bits = simde_mm_cvttps_epi32(adjusted);
    simde__m128i high_bit = simde_mm_and_si128(simde_mm_castps_si128(high_bit_mask),
        simde_mm_set1_epi32(int(0x80000000u)));
    simde__m128i result = simde_mm_or_si128(low_bits, high_bit);

    // Apply saturation: NaN -> 0, overflow -> UINT_MAX
    result = simde_mm_andnot_si128(simde_mm_castps_si128(nan_mask), result);
    result = simde_mm_andnot_si128(simde_mm_castps_si128(neg_mask), result);
    result = simde_mm_or_si128(simde_mm_andnot_si128(simde_mm_castps_si128(overflow_mask), result),
        simde_mm_and_si128(simde_mm_castps_si128(overflow_mask), simde_mm_set1_epi32(-1)));

    return result;
}

// Vector Shift Right
inline simde__m128i simde_mm_vsr(simde__m128i a, simde__m128i b) {
    b = simde_mm_srli_epi64(simde_mm_slli_epi64(b, 61), 61);
    return simde_mm_castps_si128(simde_mm_insert_ps(
        simde_mm_castsi128_ps(simde_mm_srl_epi64(a, b)),
        simde_mm_castsi128_ps(simde_mm_srl_epi64(simde_mm_srli_si128(a, 4), b)), 0x10));
}

// Vector Shift Left - shift entire 128-bit vector left by bits in low 3 bits of b
inline simde__m128i simde_mm_vsl(simde__m128i a, simde__m128i b) {
    int shift = simde_mm_extract_epi8(b, 15) & 0x7;  // Get low 3 bits from byte 15 (BE: byte 0)
    if (shift == 0) return a;
    // Split into high and low 64-bit parts
    simde__m128i low_shifted = simde_mm_slli_epi64(a, shift);
    simde__m128i high_carry = simde_mm_srli_epi64(a, 64 - shift);
    // Shift the carry from low qword to high qword position
    high_carry = simde_mm_slli_si128(high_carry, 8);
    return simde_mm_or_si128(low_shifted, high_carry);
}

// Vector Shift Left by Octet - shift entire vector left by bytes in bits [121:124] of vB
// In PPC big-endian byte 15 is at LSB position, which in x86 LE is at index 0
// Bits 121:124 within the byte are extracted as (byte >> 3) & 0xF
// PPC left shift = shift towards MSB (lower PPC addresses) = shift towards higher x86 addresses
inline simde__m128i simde_mm_vslo(simde__m128i a, simde__m128i b) {
    int shift_bytes = (simde_mm_extract_epi8(b, 0) >> 3) & 0xF;
    if (shift_bytes == 0) return a;
    if (shift_bytes >= 16) return simde_mm_setzero_si128();
    alignas(16) uint8_t src[16], dst[16];
    simde_mm_store_si128((simde__m128i*)src, a);
    memset(dst, 0, sizeof(dst));
    memcpy(dst + shift_bytes, src, 16 - shift_bytes);
    return simde_mm_load_si128((simde__m128i*)dst);
}

// Vector Shift Right by Octet - shift entire vector right by bytes in bits [121:124] of vB
// In PPC big-endian byte 15 is at LSB position, which in x86 LE is at index 0
// Bits 121:124 within the byte are extracted as (byte >> 3) & 0xF
// PPC right shift = shift towards LSB (higher PPC addresses) = shift towards lower x86 addresses
inline simde__m128i simde_mm_vsro(simde__m128i a, simde__m128i b) {
    int shift_bytes = (simde_mm_extract_epi8(b, 0) >> 3) & 0xF;
    if (shift_bytes == 0) return a;
    if (shift_bytes >= 16) return simde_mm_setzero_si128();
    alignas(16) uint8_t src[16], dst[16];
    simde_mm_store_si128((simde__m128i*)src, a);
    memset(dst, 0, sizeof(dst));
    memcpy(dst, src + shift_bytes, 16 - shift_bytes);
    return simde_mm_load_si128((simde__m128i*)dst);
}

//=============================================================================
// Platform-Specific Intrinsics
//=============================================================================

#if defined(__aarch64__) || defined(_M_ARM64)
inline uint64_t __rdtsc() {
    uint64_t ret;
    asm volatile("mrs %0, cntvct_el0\n\t" : "=r"(ret)::"memory");
    return ret;
}
#elif defined(__x86_64__) || defined(_M_X64)
// On x86_64, __rdtsc is available via x86intrin.h or immintrin.h
#if defined(__GNUC__) && !defined(__clang__)
#include <x86intrin.h>
#endif
#else
#   error "Missing implementation for __rdtsc()"
#endif

}  // namespace rex::runtime::guest

//=============================================================================
// Global Aliases for Generated Code
//=============================================================================
// Vector mask tables accessible from global scope for generated code
using rex::runtime::guest::VectorMaskL;
using rex::runtime::guest::VectorMaskR;
using rex::runtime::guest::VectorShiftTableL;
using rex::runtime::guest::VectorShiftTableR;
