/**
 * @file        ppc/fpu.h
 * @brief       Vector estimates, dot product and fused multiply add
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <simde/x86/sse4.1.h>
#include <simde/x86/fma.h>
#if defined(__AVX2__)
#include <simde/x86/avx2.h>
#endif

#include <rex/types.h>

namespace rex::ppc {

/** Result of the scalar reciprocal estimate indexed by the top 7 mantissa bits of the operand. */
inline constexpr u64 kScalarReciprocalTable[128] = {
    0x3FEFE00000000000, 0x3FEFA10000000000, 0x3FEF630000000000, 0x3FEF260000000000,
    0x3FEEEA0000000000, 0x3FEEAE0000000000, 0x3FEE740000000000, 0x3FEE3C0000000000,
    0x3FEE000000000000, 0x3FEDC80000000000, 0x3FED940000000000, 0x3FED5C0000000000,
    0x3FED280000000000, 0x3FECF00000000000, 0x3FECC00000000000, 0x3FEC8C0000000000,
    0x3FEC580000000000, 0x3FEC280000000000, 0x3FEBF80000000000, 0x3FEBC80000000000,
    0x3FEB980000000000, 0x3FEB680000000000, 0x3FEB380000000000, 0x3FEB080000000000,
    0x3FEAE00000000000, 0x3FEAB00000000000, 0x3FEA800000000000, 0x3FEA580000000000,
    0x3FEA300000000000, 0x3FEA000000000000, 0x3FE9D80000000000, 0x3FE9B00000000000,
    0x3FE9800000000000, 0x3FE9600000000000, 0x3FE9300000000000, 0x3FE9100000000000,
    0x3FE8E80000000000, 0x3FE8C00000000000, 0x3FE8980000000000, 0x3FE8700000000000,
    0x3FE8500000000000, 0x3FE8300000000000, 0x3FE8000000000000, 0x3FE7E00000000000,
    0x3FE7C00000000000, 0x3FE7A00000000000, 0x3FE7780000000000, 0x3FE7580000000000,
    0x3FE7300000000000, 0x3FE7100000000000, 0x3FE6F00000000000, 0x3FE6D00000000000,
    0x3FE6B00000000000, 0x3FE6900000000000, 0x3FE6700000000000, 0x3FE6500000000000,
    0x3FE6300000000000, 0x3FE6100000000000, 0x3FE5F00000000000, 0x3FE5D80000000000,
    0x3FE5C00000000000, 0x3FE5A00000000000, 0x3FE5800000000000, 0x3FE5600000000000,
    0x3FE5400000000000, 0x3FE5300000000000, 0x3FE5100000000000, 0x3FE4F00000000000,
    0x3FE4D80000000000, 0x3FE4C00000000000, 0x3FE4A00000000000, 0x3FE4880000000000,
    0x3FE4700000000000, 0x3FE4500000000000, 0x3FE4400000000000, 0x3FE4200000000000,
    0x3FE4000000000000, 0x3FE3F00000000000, 0x3FE3D00000000000, 0x3FE3C00000000000,
    0x3FE3A00000000000, 0x3FE3900000000000, 0x3FE3700000000000, 0x3FE3600000000000,
    0x3FE3400000000000, 0x3FE3300000000000, 0x3FE3200000000000, 0x3FE3000000000000,
    0x3FE2F00000000000, 0x3FE2D00000000000, 0x3FE2C00000000000, 0x3FE2B00000000000,
    0x3FE2900000000000, 0x3FE2800000000000, 0x3FE2700000000000, 0x3FE2500000000000,
    0x3FE2400000000000, 0x3FE2300000000000, 0x3FE2100000000000, 0x3FE2000000000000,
    0x3FE1F00000000000, 0x3FE1E00000000000, 0x3FE1C00000000000, 0x3FE1B00000000000,
    0x3FE1A00000000000, 0x3FE1900000000000, 0x3FE1700000000000, 0x3FE1600000000000,
    0x3FE1500000000000, 0x3FE1400000000000, 0x3FE1300000000000, 0x3FE1200000000000,
    0x3FE1000000000000, 0x3FE0F00000000000, 0x3FE0E00000000000, 0x3FE0D00000000000,
    0x3FE0C00000000000, 0x3FE0B00000000000, 0x3FE0A00000000000, 0x3FE0900000000000,
    0x3FE0800000000000, 0x3FE0700000000000, 0x3FE0600000000000, 0x3FE0500000000000,
    0x3FE0400000000000, 0x3FE0300000000000, 0x3FE0200000000000, 0x3FE0000000000000,
};

/** Scalar reciprocal square root estimate indexed by parity and the top 3 mantissa bits. */
inline constexpr u64 kScalarReciprocalSqrtTable[16] = {
    0x3FEF100000000000, 0x3FED800000000000, 0x3FEC000000000000, 0x3FEA800000000000,
    0x3FE9800000000000, 0x3FE8800000000000, 0x3FE8000000000000, 0x3FE7000000000000,
    0x3FE6000000000000, 0x3FE4C00000000000, 0x3FE3C00000000000, 0x3FE3000000000000,
    0x3FE2000000000000, 0x3FE1800000000000, 0x3FE1000000000000, 0x3FE0800000000000,
};

namespace simd {

/** Mantissa base of the vector reciprocal estimate indexed by the top 5 mantissa bits. */
inline constexpr u32 kReciprocalBase[32] = {
    0x7FF800, 0x783800, 0x70EA00, 0x6A0800, 0x638800, 0x5D6200, 0x579000, 0x520800,
    0x4CC800, 0x47CA00, 0x430800, 0x3E8000, 0x3A2C00, 0x360800, 0x321400, 0x2E4A00,
    0x2AA800, 0x272C00, 0x23D600, 0x209E00, 0x1D8800, 0x1A9000, 0x17AE00, 0x14F800,
    0x124400, 0x0FBE00, 0x0D3800, 0x0ADE00, 0x088400, 0x065000, 0x041C00, 0x020C00,
};

/** Mantissa slope of the vector reciprocal estimate indexed by the top 5 mantissa bits. */
inline constexpr i32 kReciprocalSlope[32] = {
    -3972, -3740, -3524, -3328, -3148, -2984, -2832, -2688, -2556, -2436, -2324,
    -2216, -2120, -2028, -1940, -1860, -1784, -1712, -1644, -1580, -1520, -1464,
    -1388, -1388, -1292, -1292, -1204, -1204, -1128, -1128, -1056, -1048,
};

/** Vector reciprocal square root mantissa base indexed by parity and the top 4 mantissa bits. */
inline constexpr u32 kReciprocalSqrtBase[32] = {
    0x7FF400, 0x785200, 0x715400, 0x6AE400, 0x64F200, 0x5F6E00, 0x5A4C00, 0x558000,
    0x510200, 0x4CCA00, 0x48D000, 0x450E00, 0x418200, 0x3E2400, 0x3AF200, 0x37E800,
    0x34FD00, 0x2F9700, 0x2AA500, 0x261800, 0x21E400, 0x1DFE00, 0x1A5C00, 0x16F800,
    0x13CA00, 0x10CE00, 0x0DFE00, 0x0B5700, 0x08D400, 0x067300, 0x043100, 0x020B00,
};

/** Vector reciprocal square root mantissa slope indexed by parity and the top 4 mantissa bits. */
inline constexpr i32 kReciprocalSqrtSlope[32] = {
    -1956, -1792, -1648, -1522, -1412, -1316, -1228, -1150, -1082, -1018, -962,
    -910,  -862,  -818,  -778,  -742,  -1384, -1267, -1165, -1077, -999,  -930,
    -869,  -814,  -764,  -720,  -680,  -643,  -609,  -579,  -550,  -523,
};

/** Fraction base of the vector log2 estimate indexed by the top 4 mantissa bits. */
inline constexpr i32 kLog2Base[16] = {
    0,    1536,  2816,  4096,  5248,  6400,  7552,  8576,
    9600, 10623, 11519, 12415, 13310, 14078, 14846, 15614,
};

/** Fraction slope of the vector log2 estimate indexed by the top 4 mantissa bits. */
inline constexpr i32 kLog2Slope[16] = {
    12, 10, 10, 9, 9, 9, 8, 8, 8, 7, 7, 7, 6, 6, 6, 6,
};

/** Top 11 mantissa bits of 2^x indexed by the 11 fraction bits of a positive operand. */
inline constexpr u16 kExp2Positive[2048] = {
    0x000, 0x001, 0x002, 0x003, 0x003, 0x004, 0x005, 0x006, 0x006, 0x007, 0x008, 0x009, 0x009,
    0x00A, 0x00B, 0x00C, 0x00C, 0x00D, 0x00E, 0x00F, 0x00F, 0x010, 0x011, 0x012, 0x012, 0x013,
    0x014, 0x015, 0x015, 0x016, 0x017, 0x018, 0x018, 0x019, 0x01A, 0x01B, 0x01B, 0x01C, 0x01D,
    0x01E, 0x01E, 0x01F, 0x020, 0x021, 0x021, 0x022, 0x023, 0x024, 0x024, 0x025, 0x026, 0x027,
    0x027, 0x028, 0x029, 0x02A, 0x02A, 0x02B, 0x02C, 0x02D, 0x02D, 0x02E, 0x02F, 0x030, 0x030,
    0x031, 0x032, 0x033, 0x033, 0x034, 0x035, 0x036, 0x036, 0x037, 0x038, 0x039, 0x039, 0x03A,
    0x03B, 0x03C, 0x03C, 0x03D, 0x03E, 0x03F, 0x03F, 0x040, 0x041, 0x042, 0x042, 0x043, 0x044,
    0x045, 0x045, 0x046, 0x047, 0x048, 0x048, 0x049, 0x04A, 0x04B, 0x04B, 0x04C, 0x04D, 0x04E,
    0x04E, 0x04F, 0x050, 0x051, 0x051, 0x052, 0x053, 0x054, 0x054, 0x055, 0x056, 0x057, 0x057,
    0x058, 0x059, 0x05A, 0x05A, 0x05B, 0x05C, 0x05D, 0x05D, 0x05E, 0x05F, 0x060, 0x060, 0x061,
    0x062, 0x063, 0x063, 0x064, 0x065, 0x066, 0x066, 0x067, 0x068, 0x069, 0x069, 0x06A, 0x06B,
    0x06C, 0x06C, 0x06D, 0x06E, 0x06F, 0x06F, 0x070, 0x071, 0x072, 0x072, 0x073, 0x074, 0x075,
    0x075, 0x076, 0x077, 0x078, 0x078, 0x079, 0x07A, 0x07B, 0x07B, 0x07C, 0x07D, 0x07E, 0x07E,
    0x07F, 0x080, 0x081, 0x081, 0x082, 0x083, 0x084, 0x084, 0x085, 0x086, 0x087, 0x087, 0x088,
    0x089, 0x08A, 0x08A, 0x08B, 0x08C, 0x08D, 0x08D, 0x08E, 0x08F, 0x090, 0x090, 0x091, 0x092,
    0x093, 0x093, 0x094, 0x095, 0x096, 0x096, 0x097, 0x098, 0x099, 0x099, 0x09A, 0x09B, 0x09C,
    0x09C, 0x09D, 0x09E, 0x09F, 0x09F, 0x0A0, 0x0A1, 0x0A2, 0x0A2, 0x0A3, 0x0A4, 0x0A5, 0x0A5,
    0x0A6, 0x0A7, 0x0A8, 0x0A8, 0x0A9, 0x0AA, 0x0AB, 0x0AB, 0x0AC, 0x0AD, 0x0AE, 0x0AE, 0x0AF,
    0x0B0, 0x0B1, 0x0B1, 0x0B2, 0x0B3, 0x0B4, 0x0B4, 0x0B5, 0x0B6, 0x0B7, 0x0B7, 0x0B8, 0x0B9,
    0x0BA, 0x0BA, 0x0BB, 0x0BC, 0x0BD, 0x0BD, 0x0BE, 0x0BF, 0x0C0, 0x0C0, 0x0C1, 0x0C2, 0x0C3,
    0x0C3, 0x0C4, 0x0C5, 0x0C6, 0x0C6, 0x0C7, 0x0C8, 0x0C9, 0x0C9, 0x0CA, 0x0CB, 0x0CC, 0x0CC,
    0x0CD, 0x0CE, 0x0CF, 0x0CF, 0x0D0, 0x0D1, 0x0D2, 0x0D2, 0x0D3, 0x0D4, 0x0D5, 0x0D5, 0x0D6,
    0x0D7, 0x0D8, 0x0D8, 0x0D9, 0x0DA, 0x0DB, 0x0DB, 0x0DC, 0x0DD, 0x0DE, 0x0DE, 0x0DF, 0x0E0,
    0x0E1, 0x0E1, 0x0E2, 0x0E3, 0x0E4, 0x0E4, 0x0E5, 0x0E6, 0x0E7, 0x0E7, 0x0E8, 0x0E9, 0x0EA,
    0x0EA, 0x0EB, 0x0EC, 0x0ED, 0x0ED, 0x0EE, 0x0EF, 0x0F0, 0x0F0, 0x0F1, 0x0F2, 0x0F3, 0x0F3,
    0x0F4, 0x0F5, 0x0F6, 0x0F6, 0x0F7, 0x0F8, 0x0F9, 0x0F9, 0x0FA, 0x0FB, 0x0FC, 0x0FC, 0x0FD,
    0x0FE, 0x0FF, 0x0FF, 0x100, 0x101, 0x102, 0x102, 0x103, 0x104, 0x105, 0x105, 0x106, 0x107,
    0x108, 0x108, 0x109, 0x10A, 0x10B, 0x10B, 0x10C, 0x10D, 0x10E, 0x10E, 0x10F, 0x110, 0x111,
    0x111, 0x112, 0x113, 0x114, 0x114, 0x115, 0x116, 0x117, 0x117, 0x118, 0x119, 0x11A, 0x11A,
    0x11B, 0x11C, 0x11D, 0x11D, 0x11E, 0x11F, 0x120, 0x120, 0x121, 0x122, 0x123, 0x123, 0x124,
    0x125, 0x126, 0x126, 0x127, 0x128, 0x129, 0x129, 0x12A, 0x12B, 0x12C, 0x12C, 0x12D, 0x12E,
    0x12F, 0x12F, 0x130, 0x131, 0x132, 0x132, 0x133, 0x134, 0x135, 0x135, 0x136, 0x137, 0x138,
    0x138, 0x139, 0x13A, 0x13B, 0x13B, 0x13C, 0x13D, 0x13E, 0x13E, 0x13F, 0x140, 0x141, 0x141,
    0x142, 0x143, 0x144, 0x144, 0x145, 0x146, 0x147, 0x147, 0x148, 0x149, 0x14A, 0x14A, 0x14B,
    0x14C, 0x14D, 0x14D, 0x14E, 0x14F, 0x150, 0x150, 0x151, 0x152, 0x153, 0x153, 0x154, 0x155,
    0x156, 0x156, 0x157, 0x158, 0x159, 0x159, 0x15A, 0x15B, 0x15C, 0x15C, 0x15D, 0x15E, 0x15F,
    0x15F, 0x160, 0x161, 0x162, 0x162, 0x163, 0x164, 0x165, 0x165, 0x166, 0x167, 0x168, 0x168,
    0x169, 0x16A, 0x16B, 0x16B, 0x16C, 0x16D, 0x16E, 0x16E, 0x16F, 0x170, 0x171, 0x171, 0x172,
    0x173, 0x174, 0x174, 0x175, 0x176, 0x177, 0x177, 0x178, 0x179, 0x17A, 0x17A, 0x17B, 0x17C,
    0x17D, 0x17D, 0x17E, 0x17F, 0x180, 0x180, 0x181, 0x182, 0x183, 0x184, 0x185, 0x186, 0x187,
    0x187, 0x188, 0x189, 0x18A, 0x18B, 0x18C, 0x18D, 0x18E, 0x18E, 0x18F, 0x190, 0x191, 0x192,
    0x193, 0x194, 0x195, 0x195, 0x196, 0x197, 0x198, 0x199, 0x19A, 0x19B, 0x19C, 0x19C, 0x19D,
    0x19E, 0x19F, 0x1A0, 0x1A1, 0x1A2, 0x1A3, 0x1A3, 0x1A4, 0x1A5, 0x1A6, 0x1A7, 0x1A8, 0x1A9,
    0x1AA, 0x1AA, 0x1AB, 0x1AC, 0x1AD, 0x1AE, 0x1AF, 0x1B0, 0x1B1, 0x1B1, 0x1B2, 0x1B3, 0x1B4,
    0x1B5, 0x1B6, 0x1B7, 0x1B8, 0x1B8, 0x1B9, 0x1BA, 0x1BB, 0x1BC, 0x1BD, 0x1BE, 0x1BF, 0x1BF,
    0x1C0, 0x1C1, 0x1C2, 0x1C3, 0x1C4, 0x1C5, 0x1C6, 0x1C6, 0x1C7, 0x1C8, 0x1C9, 0x1CA, 0x1CB,
    0x1CC, 0x1CD, 0x1CD, 0x1CE, 0x1CF, 0x1D0, 0x1D1, 0x1D2, 0x1D3, 0x1D4, 0x1D4, 0x1D5, 0x1D6,
    0x1D7, 0x1D8, 0x1D9, 0x1DA, 0x1DB, 0x1DB, 0x1DC, 0x1DD, 0x1DE, 0x1DF, 0x1E0, 0x1E1, 0x1E2,
    0x1E2, 0x1E3, 0x1E4, 0x1E5, 0x1E6, 0x1E7, 0x1E8, 0x1E9, 0x1E9, 0x1EA, 0x1EB, 0x1EC, 0x1ED,
    0x1EE, 0x1EF, 0x1F0, 0x1F0, 0x1F1, 0x1F2, 0x1F3, 0x1F4, 0x1F5, 0x1F6, 0x1F7, 0x1F7, 0x1F8,
    0x1F9, 0x1FA, 0x1FB, 0x1FC, 0x1FD, 0x1FE, 0x1FE, 0x1FF, 0x200, 0x201, 0x202, 0x203, 0x204,
    0x205, 0x205, 0x206, 0x207, 0x208, 0x209, 0x20A, 0x20B, 0x20C, 0x20C, 0x20D, 0x20E, 0x20F,
    0x210, 0x211, 0x212, 0x213, 0x213, 0x214, 0x215, 0x216, 0x217, 0x218, 0x219, 0x21A, 0x21A,
    0x21B, 0x21C, 0x21D, 0x21E, 0x21F, 0x220, 0x221, 0x221, 0x222, 0x223, 0x224, 0x225, 0x226,
    0x227, 0x228, 0x228, 0x229, 0x22A, 0x22B, 0x22C, 0x22D, 0x22E, 0x22F, 0x22F, 0x230, 0x231,
    0x232, 0x233, 0x234, 0x235, 0x236, 0x236, 0x237, 0x238, 0x239, 0x23A, 0x23B, 0x23C, 0x23D,
    0x23D, 0x23E, 0x23F, 0x240, 0x241, 0x242, 0x243, 0x244, 0x244, 0x245, 0x246, 0x247, 0x248,
    0x249, 0x24A, 0x24B, 0x24B, 0x24C, 0x24D, 0x24E, 0x24F, 0x250, 0x251, 0x252, 0x252, 0x253,
    0x254, 0x255, 0x256, 0x257, 0x258, 0x259, 0x259, 0x25A, 0x25B, 0x25C, 0x25D, 0x25E, 0x25F,
    0x260, 0x260, 0x261, 0x262, 0x263, 0x264, 0x265, 0x266, 0x267, 0x267, 0x268, 0x269, 0x26A,
    0x26B, 0x26C, 0x26D, 0x26E, 0x26E, 0x26F, 0x270, 0x271, 0x272, 0x273, 0x274, 0x275, 0x275,
    0x276, 0x277, 0x278, 0x279, 0x27A, 0x27B, 0x27C, 0x27C, 0x27D, 0x27E, 0x27F, 0x280, 0x281,
    0x282, 0x283, 0x283, 0x284, 0x285, 0x286, 0x287, 0x288, 0x289, 0x28A, 0x28A, 0x28B, 0x28C,
    0x28D, 0x28E, 0x28F, 0x290, 0x291, 0x291, 0x292, 0x293, 0x294, 0x295, 0x296, 0x297, 0x298,
    0x298, 0x299, 0x29A, 0x29B, 0x29C, 0x29D, 0x29E, 0x29F, 0x29F, 0x2A0, 0x2A1, 0x2A2, 0x2A3,
    0x2A4, 0x2A5, 0x2A6, 0x2A6, 0x2A7, 0x2A8, 0x2A9, 0x2AA, 0x2AB, 0x2AC, 0x2AD, 0x2AD, 0x2AE,
    0x2AF, 0x2B0, 0x2B1, 0x2B2, 0x2B3, 0x2B4, 0x2B4, 0x2B5, 0x2B6, 0x2B7, 0x2B8, 0x2B9, 0x2BA,
    0x2BB, 0x2BB, 0x2BC, 0x2BD, 0x2BE, 0x2BF, 0x2C0, 0x2C1, 0x2C2, 0x2C2, 0x2C3, 0x2C4, 0x2C5,
    0x2C6, 0x2C7, 0x2C8, 0x2C9, 0x2C9, 0x2CA, 0x2CB, 0x2CC, 0x2CD, 0x2CE, 0x2CF, 0x2D0, 0x2D0,
    0x2D1, 0x2D2, 0x2D3, 0x2D4, 0x2D5, 0x2D6, 0x2D7, 0x2D8, 0x2D9, 0x2DA, 0x2DB, 0x2DC, 0x2DD,
    0x2DE, 0x2DF, 0x2E0, 0x2E1, 0x2E2, 0x2E3, 0x2E4, 0x2E5, 0x2E6, 0x2E7, 0x2E8, 0x2E9, 0x2EA,
    0x2EB, 0x2EC, 0x2ED, 0x2EE, 0x2EF, 0x2F0, 0x2F1, 0x2F2, 0x2F3, 0x2F4, 0x2F5, 0x2F6, 0x2F7,
    0x2F8, 0x2F9, 0x2FA, 0x2FB, 0x2FC, 0x2FD, 0x2FE, 0x2FF, 0x300, 0x301, 0x302, 0x303, 0x304,
    0x305, 0x306, 0x307, 0x308, 0x309, 0x30A, 0x30B, 0x30C, 0x30D, 0x30E, 0x30F, 0x310, 0x311,
    0x312, 0x313, 0x314, 0x315, 0x316, 0x317, 0x318, 0x319, 0x31A, 0x31B, 0x31C, 0x31D, 0x31E,
    0x31F, 0x320, 0x321, 0x322, 0x323, 0x324, 0x325, 0x326, 0x327, 0x328, 0x329, 0x32A, 0x32B,
    0x32C, 0x32D, 0x32E, 0x32F, 0x330, 0x331, 0x332, 0x333, 0x334, 0x335, 0x336, 0x337, 0x338,
    0x339, 0x33A, 0x33B, 0x33C, 0x33D, 0x33E, 0x33F, 0x340, 0x341, 0x342, 0x343, 0x344, 0x345,
    0x346, 0x347, 0x348, 0x349, 0x34A, 0x34B, 0x34C, 0x34D, 0x34E, 0x34F, 0x350, 0x351, 0x352,
    0x353, 0x354, 0x355, 0x356, 0x357, 0x358, 0x359, 0x35A, 0x35B, 0x35C, 0x35D, 0x35E, 0x35F,
    0x360, 0x361, 0x362, 0x363, 0x364, 0x365, 0x366, 0x367, 0x368, 0x369, 0x36A, 0x36B, 0x36C,
    0x36D, 0x36E, 0x36F, 0x370, 0x371, 0x372, 0x373, 0x374, 0x375, 0x376, 0x377, 0x378, 0x379,
    0x37A, 0x37B, 0x37C, 0x37D, 0x37E, 0x37F, 0x380, 0x381, 0x382, 0x383, 0x384, 0x385, 0x386,
    0x387, 0x388, 0x389, 0x38A, 0x38B, 0x38C, 0x38D, 0x38E, 0x38F, 0x390, 0x391, 0x392, 0x393,
    0x394, 0x395, 0x396, 0x397, 0x398, 0x399, 0x39A, 0x39B, 0x39C, 0x39D, 0x39E, 0x39F, 0x3A0,
    0x3A1, 0x3A2, 0x3A3, 0x3A4, 0x3A5, 0x3A6, 0x3A7, 0x3A8, 0x3A9, 0x3AA, 0x3AB, 0x3AC, 0x3AD,
    0x3AE, 0x3AF, 0x3B0, 0x3B1, 0x3B2, 0x3B3, 0x3B4, 0x3B5, 0x3B6, 0x3B7, 0x3B8, 0x3B9, 0x3BA,
    0x3BB, 0x3BC, 0x3BD, 0x3BE, 0x3BF, 0x3C0, 0x3C1, 0x3C2, 0x3C3, 0x3C4, 0x3C5, 0x3C6, 0x3C7,
    0x3C8, 0x3C9, 0x3CA, 0x3CB, 0x3CC, 0x3CD, 0x3CE, 0x3CF, 0x3D0, 0x3D1, 0x3D2, 0x3D3, 0x3D4,
    0x3D5, 0x3D6, 0x3D7, 0x3D8, 0x3D9, 0x3DA, 0x3DB, 0x3DC, 0x3DD, 0x3DE, 0x3DF, 0x3E0, 0x3E1,
    0x3E2, 0x3E3, 0x3E4, 0x3E5, 0x3E6, 0x3E7, 0x3E8, 0x3E9, 0x3EA, 0x3EB, 0x3EC, 0x3ED, 0x3EE,
    0x3EF, 0x3F0, 0x3F1, 0x3F2, 0x3F3, 0x3F4, 0x3F5, 0x3F6, 0x3F7, 0x3F8, 0x3F9, 0x3FA, 0x3FB,
    0x3FC, 0x3FD, 0x3FE, 0x3FF, 0x400, 0x401, 0x402, 0x403, 0x404, 0x405, 0x406, 0x407, 0x408,
    0x409, 0x40A, 0x40B, 0x40C, 0x40D, 0x40E, 0x40F, 0x410, 0x411, 0x412, 0x413, 0x414, 0x415,
    0x416, 0x417, 0x418, 0x419, 0x41A, 0x41B, 0x41C, 0x41D, 0x41E, 0x41F, 0x420, 0x421, 0x422,
    0x423, 0x424, 0x425, 0x426, 0x427, 0x428, 0x429, 0x42A, 0x42B, 0x42C, 0x42D, 0x42E, 0x42F,
    0x430, 0x431, 0x432, 0x433, 0x434, 0x435, 0x436, 0x437, 0x438, 0x439, 0x43A, 0x43B, 0x43C,
    0x43D, 0x43E, 0x43F, 0x440, 0x441, 0x442, 0x443, 0x444, 0x445, 0x446, 0x447, 0x448, 0x449,
    0x44A, 0x44B, 0x44C, 0x44D, 0x44E, 0x44F, 0x451, 0x452, 0x453, 0x454, 0x455, 0x456, 0x457,
    0x458, 0x45A, 0x45B, 0x45C, 0x45D, 0x45E, 0x45F, 0x460, 0x461, 0x463, 0x464, 0x465, 0x466,
    0x467, 0x468, 0x469, 0x46A, 0x46C, 0x46D, 0x46E, 0x46F, 0x470, 0x471, 0x472, 0x473, 0x475,
    0x476, 0x477, 0x478, 0x479, 0x47A, 0x47B, 0x47C, 0x47E, 0x47F, 0x480, 0x481, 0x482, 0x483,
    0x484, 0x485, 0x487, 0x488, 0x489, 0x48A, 0x48B, 0x48C, 0x48D, 0x48E, 0x490, 0x491, 0x492,
    0x493, 0x494, 0x495, 0x496, 0x497, 0x499, 0x49A, 0x49B, 0x49C, 0x49D, 0x49E, 0x49F, 0x4A0,
    0x4A2, 0x4A3, 0x4A4, 0x4A5, 0x4A6, 0x4A7, 0x4A8, 0x4A9, 0x4AB, 0x4AC, 0x4AD, 0x4AE, 0x4AF,
    0x4B0, 0x4B1, 0x4B2, 0x4B4, 0x4B5, 0x4B6, 0x4B7, 0x4B8, 0x4B9, 0x4BA, 0x4BB, 0x4BD, 0x4BE,
    0x4BF, 0x4C0, 0x4C1, 0x4C2, 0x4C3, 0x4C4, 0x4C6, 0x4C7, 0x4C8, 0x4C9, 0x4CA, 0x4CB, 0x4CC,
    0x4CD, 0x4CF, 0x4D0, 0x4D1, 0x4D2, 0x4D3, 0x4D4, 0x4D5, 0x4D6, 0x4D8, 0x4D9, 0x4DA, 0x4DB,
    0x4DC, 0x4DD, 0x4DE, 0x4DF, 0x4E1, 0x4E2, 0x4E3, 0x4E4, 0x4E5, 0x4E6, 0x4E7, 0x4E8, 0x4EA,
    0x4EB, 0x4EC, 0x4ED, 0x4EE, 0x4EF, 0x4F0, 0x4F1, 0x4F3, 0x4F4, 0x4F5, 0x4F6, 0x4F7, 0x4F8,
    0x4F9, 0x4FA, 0x4FC, 0x4FD, 0x4FE, 0x4FF, 0x500, 0x501, 0x502, 0x503, 0x505, 0x506, 0x507,
    0x508, 0x509, 0x50A, 0x50B, 0x50C, 0x50E, 0x50F, 0x510, 0x511, 0x512, 0x513, 0x514, 0x515,
    0x517, 0x518, 0x519, 0x51A, 0x51B, 0x51C, 0x51D, 0x51E, 0x520, 0x521, 0x522, 0x523, 0x524,
    0x525, 0x526, 0x527, 0x529, 0x52A, 0x52B, 0x52C, 0x52D, 0x52E, 0x52F, 0x530, 0x532, 0x533,
    0x534, 0x535, 0x536, 0x537, 0x538, 0x539, 0x53B, 0x53C, 0x53D, 0x53E, 0x53F, 0x540, 0x541,
    0x542, 0x544, 0x545, 0x546, 0x547, 0x548, 0x549, 0x54A, 0x54B, 0x54D, 0x54E, 0x54F, 0x550,
    0x551, 0x552, 0x553, 0x554, 0x556, 0x557, 0x558, 0x559, 0x55A, 0x55B, 0x55C, 0x55D, 0x55F,
    0x560, 0x561, 0x562, 0x563, 0x564, 0x565, 0x566, 0x568, 0x569, 0x56A, 0x56B, 0x56C, 0x56D,
    0x56E, 0x56F, 0x571, 0x572, 0x573, 0x574, 0x575, 0x576, 0x577, 0x578, 0x57A, 0x57B, 0x57C,
    0x57D, 0x57E, 0x57F, 0x580, 0x581, 0x583, 0x584, 0x585, 0x586, 0x587, 0x588, 0x589, 0x58A,
    0x58C, 0x58D, 0x58E, 0x58F, 0x590, 0x591, 0x592, 0x593, 0x595, 0x596, 0x597, 0x598, 0x599,
    0x59A, 0x59B, 0x59C, 0x59E, 0x59F, 0x5A0, 0x5A1, 0x5A2, 0x5A3, 0x5A4, 0x5A5, 0x5A7, 0x5A8,
    0x5A9, 0x5AA, 0x5AB, 0x5AC, 0x5AD, 0x5AE, 0x5B0, 0x5B1, 0x5B2, 0x5B3, 0x5B4, 0x5B5, 0x5B6,
    0x5B7, 0x5B9, 0x5BA, 0x5BB, 0x5BC, 0x5BD, 0x5BE, 0x5BF, 0x5C0, 0x5C2, 0x5C3, 0x5C4, 0x5C5,
    0x5C6, 0x5C7, 0x5C8, 0x5C9, 0x5CB, 0x5CC, 0x5CD, 0x5CE, 0x5CF, 0x5D0, 0x5D1, 0x5D2, 0x5D4,
    0x5D5, 0x5D6, 0x5D7, 0x5D8, 0x5D9, 0x5DA, 0x5DB, 0x5DD, 0x5DE, 0x5DF, 0x5E0, 0x5E1, 0x5E2,
    0x5E3, 0x5E4, 0x5E6, 0x5E7, 0x5E8, 0x5E9, 0x5EA, 0x5EB, 0x5EC, 0x5ED, 0x5EF, 0x5F0, 0x5F1,
    0x5F2, 0x5F3, 0x5F4, 0x5F5, 0x5F6, 0x5F8, 0x5F9, 0x5FA, 0x5FB, 0x5FC, 0x5FD, 0x5FE, 0x5FF,
    0x601, 0x602, 0x603, 0x604, 0x606, 0x607, 0x608, 0x609, 0x60B, 0x60C, 0x60D, 0x60E, 0x610,
    0x611, 0x612, 0x613, 0x615, 0x616, 0x617, 0x618, 0x61A, 0x61B, 0x61C, 0x61D, 0x61F, 0x620,
    0x621, 0x622, 0x624, 0x625, 0x626, 0x627, 0x629, 0x62A, 0x62B, 0x62C, 0x62E, 0x62F, 0x630,
    0x631, 0x633, 0x634, 0x635, 0x636, 0x638, 0x639, 0x63A, 0x63B, 0x63D, 0x63E, 0x63F, 0x640,
    0x642, 0x643, 0x644, 0x645, 0x647, 0x648, 0x649, 0x64A, 0x64C, 0x64D, 0x64E, 0x64F, 0x651,
    0x652, 0x653, 0x654, 0x656, 0x657, 0x658, 0x659, 0x65B, 0x65C, 0x65D, 0x65E, 0x660, 0x661,
    0x662, 0x663, 0x665, 0x666, 0x667, 0x668, 0x66A, 0x66B, 0x66C, 0x66D, 0x66F, 0x670, 0x671,
    0x672, 0x674, 0x675, 0x676, 0x677, 0x679, 0x67A, 0x67B, 0x67C, 0x67E, 0x67F, 0x680, 0x681,
    0x683, 0x684, 0x685, 0x686, 0x688, 0x689, 0x68A, 0x68B, 0x68D, 0x68E, 0x68F, 0x690, 0x692,
    0x693, 0x694, 0x695, 0x697, 0x698, 0x699, 0x69A, 0x69C, 0x69D, 0x69E, 0x69F, 0x6A1, 0x6A2,
    0x6A3, 0x6A4, 0x6A6, 0x6A7, 0x6A8, 0x6A9, 0x6AB, 0x6AC, 0x6AD, 0x6AE, 0x6B0, 0x6B1, 0x6B2,
    0x6B3, 0x6B5, 0x6B6, 0x6B7, 0x6B8, 0x6BA, 0x6BB, 0x6BC, 0x6BD, 0x6BF, 0x6C0, 0x6C1, 0x6C2,
    0x6C4, 0x6C5, 0x6C6, 0x6C7, 0x6C9, 0x6CA, 0x6CB, 0x6CC, 0x6CE, 0x6CF, 0x6D0, 0x6D1, 0x6D3,
    0x6D4, 0x6D5, 0x6D6, 0x6D8, 0x6D9, 0x6DA, 0x6DB, 0x6DD, 0x6DE, 0x6DF, 0x6E0, 0x6E2, 0x6E3,
    0x6E4, 0x6E5, 0x6E7, 0x6E8, 0x6E9, 0x6EA, 0x6EC, 0x6ED, 0x6EE, 0x6EF, 0x6F1, 0x6F2, 0x6F3,
    0x6F4, 0x6F6, 0x6F7, 0x6F8, 0x6F9, 0x6FB, 0x6FC, 0x6FD, 0x6FE, 0x700, 0x701, 0x702, 0x703,
    0x705, 0x706, 0x707, 0x708, 0x70A, 0x70B, 0x70C, 0x70D, 0x70F, 0x710, 0x711, 0x712, 0x714,
    0x715, 0x716, 0x717, 0x719, 0x71A, 0x71B, 0x71C, 0x71E, 0x71F, 0x720, 0x721, 0x723, 0x724,
    0x725, 0x726, 0x728, 0x729, 0x72A, 0x72B, 0x72D, 0x72E, 0x72F, 0x730, 0x732, 0x733, 0x734,
    0x735, 0x737, 0x738, 0x739, 0x73A, 0x73C, 0x73D, 0x73E, 0x73F, 0x741, 0x742, 0x744, 0x745,
    0x747, 0x748, 0x74A, 0x74B, 0x74D, 0x74E, 0x750, 0x751, 0x753, 0x754, 0x756, 0x757, 0x759,
    0x75A, 0x75C, 0x75D, 0x75F, 0x760, 0x762, 0x763, 0x765, 0x766, 0x768, 0x769, 0x76B, 0x76C,
    0x76E, 0x76F, 0x771, 0x772, 0x774, 0x775, 0x777, 0x778, 0x77A, 0x77B, 0x77D, 0x77E, 0x780,
    0x781, 0x783, 0x784, 0x786, 0x787, 0x789, 0x78A, 0x78C, 0x78D, 0x78F, 0x790, 0x792, 0x793,
    0x795, 0x796, 0x798, 0x799, 0x79B, 0x79C, 0x79E, 0x79F, 0x7A1, 0x7A2, 0x7A4, 0x7A5, 0x7A7,
    0x7A8, 0x7AA, 0x7AB, 0x7AD, 0x7AE, 0x7B0, 0x7B1, 0x7B3, 0x7B4, 0x7B6, 0x7B7, 0x7B9, 0x7BA,
    0x7BC, 0x7BD, 0x7BF, 0x7C0, 0x7C2, 0x7C3, 0x7C5, 0x7C6, 0x7C8, 0x7C9, 0x7CB, 0x7CC, 0x7CE,
    0x7CF, 0x7D1, 0x7D2, 0x7D4, 0x7D5, 0x7D7, 0x7D8, 0x7DA, 0x7DB, 0x7DD, 0x7DE, 0x7E0, 0x7E1,
    0x7E3, 0x7E4, 0x7E6, 0x7E7, 0x7E9, 0x7EA, 0x7EC, 0x7ED, 0x7EF, 0x7F0, 0x7F2, 0x7F3, 0x7F5,
    0x7F6, 0x7F8, 0x7F9, 0x7FB, 0x7FC, 0x7FE, 0x7FF,
};

}  // namespace simd

}  // namespace rex::ppc

namespace rex::ppc {

inline constexpr u32 kSign32 = 0x80000000u;
inline constexpr u32 kQuiet32 = 0x00400000u;
inline constexpr u32 kInfinity32 = 0x7F800000u;
inline constexpr u32 kQNaN32 = 0x7FC00000u;
inline constexpr u64 kSign64 = 0x8000000000000000ull;
inline constexpr u64 kQuiet64 = 0x0008000000000000ull;
inline constexpr u64 kMantissa64 = 0x000FFFFFFFFFFFFFull;
inline constexpr u64 kInfinity64 = 0x7FF0000000000000ull;
inline constexpr u64 kQNaN64 = 0x7FF8000000000000ull;

/**
 * Computes the scalar reciprocal estimate of fres.
 *
 * Operands with a biased exponent below 895 give a signed infinity, above 1172 a signed zero. A NaN
 * comes back quiet.
 */
[[nodiscard]] inline f64 ReciprocalEstimate(f64 x) noexcept {
  const u64 a = std::bit_cast<u64>(x);
  const u64 sign = a & kSign64;
  const i64 e = static_cast<i64>((a >> 52) & 0x7FFu);
  const u64 m = a & kMantissa64;
  if (e == 0x7FF) {
    return std::bit_cast<f64>(m != 0 ? a | kQuiet64 : sign);
  }
  if (e < 895) {
    return std::bit_cast<f64>(sign | kInfinity64);
  }
  if (e >= 1173) {
    return std::bit_cast<f64>(sign);
  }
  const u64 base = kScalarReciprocalTable[m >> 45];
  const i64 exponent = static_cast<i64>(base >> 52) - (e - 1023);
  return std::bit_cast<f64>(sign | static_cast<u64>(exponent) << 52 | (base & kMantissa64));
}

/**
 * Computes the scalar reciprocal square root estimate of frsqrte.
 *
 * Zero gives a signed infinity. A negative operand or negative infinity gives the default quiet
 * NaN.
 */
[[nodiscard]] inline f64 ReciprocalSqrtEstimate(f64 x) noexcept {
  const u64 a = std::bit_cast<u64>(x);
  const u64 sign = a & kSign64;
  i64 e = static_cast<i64>((a >> 52) & 0x7FFu);
  u64 m = a & kMantissa64;
  if (e == 0x7FF) {
    return std::bit_cast<f64>(m != 0 ? a | kQuiet64 : (sign != 0 ? kQNaN64 : 0));
  }
  if (e == 0) {
    if (m == 0) {
      return std::bit_cast<f64>(sign | kInfinity64);
    }
    const i32 shift = std::countl_zero(m) - 11;
    m = (m << shift) & kMantissa64;
    e = 1 - shift;
  }
  if (sign != 0) {
    return std::bit_cast<f64>(kQNaN64);
  }
  const i64 unbiased = e - 1023;
  const u64 base = kScalarReciprocalSqrtTable[static_cast<u64>(unbiased & 1) << 3 | m >> 49];
  const i64 exponent = static_cast<i64>(base >> 52) - (unbiased >> 1);
  return std::bit_cast<f64>(static_cast<u64>(exponent) << 52 | (base & kMantissa64));
}

namespace simd {

namespace detail {

struct Plane32 {
  u8 lo[16];
  u8 hi[16];
};

struct Plane16 {
  u8 bytes[16];
};

/** Packs the base and the slope magnitude of a vrefp cell. */
constexpr u32 ReciprocalCell(u32 i) {
  return (kReciprocalBase[i] >> 9) << 10 | static_cast<u32>(-kReciprocalSlope[i] / 4);
}

/** Packs the base and the slope magnitude of a vrsqrtefp cell. */
constexpr u32 ReciprocalSqrtCell(u32 i) {
  return (kReciprocalSqrtBase[i] >> 8) << 11 | static_cast<u32>(-kReciprocalSqrtSlope[i]);
}

/** Packs the base and the slope of a vlogefp cell. */
constexpr u32 Log2Cell(u32 i) {
  return static_cast<u32>(kLog2Base[i]) << 4 | static_cast<u32>(kLog2Slope[i]);
}

/** Finds the base and the slope of the line that reproduces 128 entries of the exp2 table. */
constexpr u32 Exp2Cell(u32 i) {
  for (u32 slope = 0; slope < 16; ++slope) {
    i32 lowest = -0x7FFFFFFF;
    i32 highest = 0x7FFFFFFF;
    for (u32 u = 0; u < 128; ++u) {
      const i32 entry = 8 * static_cast<i32>(kExp2Positive[i * 128 + u]);
      const i32 offset = static_cast<i32>(slope * u);
      lowest = lowest > entry - offset ? lowest : entry - offset;
      highest = highest < entry + 7 - offset ? highest : entry + 7 - offset;
    }
    if (lowest <= highest) {
      return static_cast<u32>(lowest) << 4 | slope;
    }
  }
  return 0;
}

constexpr bool IsExp2TableExact() {
  for (u32 i = 0; i < 16; ++i) {
    const u32 cell = Exp2Cell(i);
    for (u32 u = 0; u < 128; ++u) {
      const u32 value = ((cell >> 4) + (cell & 15) * u) >> 3;
      if (value != kExp2Positive[i * 128 + u]) {
        return false;
      }
    }
  }
  return true;
}

static_assert(IsExp2TableExact(), "the exp2 table must follow 16 lines");

template <u32 (*Cell)(u32), u32 Byte>
inline constexpr Plane32 kPlane32 = [] {
  Plane32 plane{};
  for (u32 i = 0; i < 16; ++i) {
    plane.lo[i] = static_cast<u8>(Cell(i) >> (8 * Byte));
    plane.hi[i] = static_cast<u8>(Cell(i + 16) >> (8 * Byte));
  }
  return plane;
}();

template <u32 (*Cell)(u32), u32 Byte>
inline constexpr Plane16 kPlane16 = [] {
  Plane16 plane{};
  for (u32 i = 0; i < 16; ++i) {
    plane.bytes[i] = static_cast<u8>(Cell(i) >> (8 * Byte));
  }
  return plane;
}();

inline simde__m128i Set(u32 value) {
  return simde_mm_set1_epi32(static_cast<i32>(value));
}

inline simde__m128i Select(simde__m128i mask, simde__m128i yes, simde__m128i no) {
  return simde_mm_blendv_epi8(no, yes, mask);
}

inline simde__m128i Equal(simde__m128i a, u32 value) {
  return simde_mm_cmpeq_epi32(a, Set(value));
}

struct Controls {
  simde__m128i lo;
  simde__m128i hi;
};

inline Controls MakeControls(simde__m128i index) {
  const auto lo = simde_mm_or_si128(simde_mm_add_epi32(index, Set(0x70)), Set(0x80808000u));
  return {lo, simde_mm_xor_si128(lo, Set(0x80))};
}

/** Reads one plane of a 32 entry table into byte 0 of each lane. */
template <const Plane32& Plane>
inline simde__m128i Fetch32(const Controls& controls) {
  const auto lo = simde_mm_loadu_si128(reinterpret_cast<const simde__m128i*>(Plane.lo));
  const auto hi = simde_mm_loadu_si128(reinterpret_cast<const simde__m128i*>(Plane.hi));
  return simde_mm_or_si128(simde_mm_shuffle_epi8(lo, controls.lo),
                           simde_mm_shuffle_epi8(hi, controls.hi));
}

/** Reads one plane of a 16 entry table into byte 0 of each lane. */
template <const Plane16& Plane>
inline simde__m128i Fetch16(simde__m128i control) {
  const auto bytes = simde_mm_loadu_si128(reinterpret_cast<const simde__m128i*>(Plane.bytes));
  return simde_mm_shuffle_epi8(bytes, control);
}

/** Reads the cell of a 32 entry table for the index of each lane. */
template <u32 (*Cell)(u32), u32 Planes>
inline simde__m128i Lookup32(simde__m128i index) {
  const auto controls = MakeControls(index);
  const auto byte0 = Fetch32<kPlane32<Cell, 0>>(controls);
  const auto byte1 = Fetch32<kPlane32<Cell, 1>>(controls);
  const auto byte2 = Fetch32<kPlane32<Cell, 2>>(controls);
  auto cell = simde_mm_or_si128(byte0, simde_mm_slli_epi32(byte1, 8));
  cell = simde_mm_or_si128(cell, simde_mm_slli_epi32(byte2, 16));
  if constexpr (Planes > 3) {
    const auto byte3 = Fetch32<kPlane32<Cell, 3>>(controls);
    cell = simde_mm_or_si128(cell, simde_mm_slli_epi32(byte3, 24));
  }
  return cell;
}

/** Reads the cell of a 16 entry table for the index of each lane. */
template <u32 (*Cell)(u32)>
inline simde__m128i Lookup16(simde__m128i index) {
  const auto control = simde_mm_or_si128(index, Set(0x80808000u));
  const auto byte0 = Fetch16<kPlane16<Cell, 0>>(control);
  const auto byte1 = Fetch16<kPlane16<Cell, 1>>(control);
  const auto byte2 = Fetch16<kPlane16<Cell, 2>>(control);
  const auto cell = simde_mm_or_si128(byte0, simde_mm_slli_epi32(byte1, 8));
  return simde_mm_or_si128(cell, simde_mm_slli_epi32(byte2, 16));
}

/** Reports whether any lane of the value is below the lowest or above the highest bound. */
inline bool AnyOutside(simde__m128i value, u32 lowest, u32 highest) {
  const auto offset = simde_mm_add_epi32(value, Set(kSign32 - lowest));
  const auto outside = simde_mm_cmpgt_epi32(offset, Set(kSign32 + (highest - lowest)));
  return simde_mm_testz_si128(outside, outside) == 0;
}

}  // namespace detail

/** Computes vrefp and vrefp128. */
[[nodiscard]] inline simde__m128 ReciprocalEstimate(simde__m128 v) noexcept {
  using namespace detail;
  const auto a = simde_mm_castps_si128(v);
  const auto biased = simde_mm_and_si128(a, Set(0x7F800000u));
  const auto cell =
      Lookup32<ReciprocalCell, 3>(simde_mm_and_si128(simde_mm_srli_epi32(a, 18), Set(31)));
  const auto u = simde_mm_and_si128(simde_mm_srli_epi32(a, 9), Set(0x1FF));
  const auto base = simde_mm_and_si128(simde_mm_srli_epi32(cell, 1), Set(0x7FFE00));
  const auto mantissa =
      simde_mm_sub_epi32(base, simde_mm_madd_epi16(simde_mm_and_si128(cell, Set(0x3FF)), u));
  const auto top = simde_mm_and_si128(a, Set(0xFF800000u));
  auto result = simde_mm_or_si128(simde_mm_sub_epi32(Set(253u << 23), top), mantissa);
  if (!AnyOutside(biased, 1u << 23, 252u << 23)) [[likely]] {
    return simde_mm_castsi128_ps(result);
  }
  const auto sign = simde_mm_and_si128(a, Set(kSign32));
  const auto e = simde_mm_srli_epi32(biased, 23);
  const auto m = simde_mm_and_si128(a, Set(0x7FFFFF));
  const auto smallest = simde_mm_and_si128(Equal(e, 253), Equal(m, 0));
  const auto underflow = Select(smallest, simde_mm_or_si128(sign, Set(0x00800000u)), sign);
  result = Select(simde_mm_cmpgt_epi32(e, Set(252)), underflow, result);
  const auto nan = simde_mm_andnot_si128(Equal(m, 0), Equal(e, 0xFF));
  result = Select(nan, simde_mm_or_si128(a, Set(kQuiet32)), result);
  return simde_mm_castsi128_ps(
      Select(Equal(e, 0), simde_mm_or_si128(sign, Set(kInfinity32)), result));
}

/**
 * Computes vrsqrtefp and vrsqrtefp128.
 *
 * @return The reciprocal square root estimate of each lane.
 */
[[nodiscard]] inline simde__m128 ReciprocalSqrtEstimate(simde__m128 v) noexcept {
  using namespace detail;
  const auto a = simde_mm_castps_si128(v);
  const auto e = simde_mm_srli_epi32(simde_mm_and_si128(a, Set(kInfinity32)), 23);
  const auto index =
      simde_mm_and_si128(simde_mm_srli_epi32(simde_mm_xor_si128(a, Set(1u << 23)), 19), Set(31));
  const auto cell = Lookup32<ReciprocalSqrtCell, 4>(index);
  const auto u = simde_mm_and_si128(simde_mm_srli_epi32(a, 9), Set(0x3FF));
  const auto product = simde_mm_madd_epi16(simde_mm_and_si128(cell, Set(0x7FF)), u);
  const auto tie = simde_mm_and_si128(simde_mm_srli_epi32(product, 2), Set(1));
  const auto quotient =
      simde_mm_srli_epi32(simde_mm_add_epi32(simde_mm_add_epi32(product, Set(1)), tie), 2);
  const auto mantissa =
      simde_mm_sub_epi32(simde_mm_and_si128(simde_mm_srli_epi32(cell, 3), Set(0x7FFF00)), quotient);
  const auto half = simde_mm_srai_epi32(simde_mm_sub_epi32(e, Set(127)), 1);
  const auto field = simde_mm_slli_epi32(simde_mm_sub_epi32(Set(126), half), 23);
  auto result = simde_mm_or_si128(field, mantissa);
  if (!AnyOutside(a, 1u << 23, (255u << 23) - 1)) [[likely]] {
    return simde_mm_castsi128_ps(result);
  }
  const auto sign = simde_mm_and_si128(a, Set(kSign32));
  const auto negative = simde_mm_srai_epi32(a, 31);
  const auto m = simde_mm_and_si128(a, Set(0x7FFFFF));
  result = Select(negative, Set(kQNaN32), result);
  const auto nan = simde_mm_andnot_si128(Equal(m, 0), Equal(e, 0xFF));
  const auto infinity = simde_mm_and_si128(negative, Set(kQNaN32));
  result =
      Select(Equal(e, 0xFF), Select(nan, simde_mm_or_si128(a, Set(kQuiet32)), infinity), result);
  return simde_mm_castsi128_ps(
      Select(Equal(e, 0), simde_mm_or_si128(sign, Set(kInfinity32)), result));
}

/**
 * Computes vlogefp and vlogefp128.
 *
 * @return The log2 estimate of each lane.
 */
[[nodiscard]] inline simde__m128 Log2Estimate(simde__m128 v) noexcept {
  using namespace detail;
  const auto a = simde_mm_castps_si128(v);
  const auto shifted = simde_mm_srli_epi32(a, 12);
  const auto cell = Lookup16<Log2Cell>(simde_mm_and_si128(simde_mm_srli_epi32(a, 19), Set(15)));
  const auto u = simde_mm_and_si128(shifted, Set(0x7F));
  const auto fraction = simde_mm_srli_epi32(
      simde_mm_add_epi32(simde_mm_srli_epi32(cell, 4),
                         simde_mm_madd_epi16(simde_mm_and_si128(cell, Set(15)), u)),
      3);
  const auto exponent =
      simde_mm_sub_epi32(simde_mm_and_si128(shifted, Set(0x7F800)), Set(127u << 11));
  const auto fixed = simde_mm_add_epi32(exponent, fraction);
  auto result = simde_mm_castps_si128(
      simde_mm_mul_ps(simde_mm_cvtepi32_ps(fixed), simde_mm_set1_ps(1.0f / 2048.0f)));
  if (!AnyOutside(a, 1u << 23, (255u << 23) - 1)) [[likely]] {
    return simde_mm_castsi128_ps(result);
  }
  const auto negative = simde_mm_srai_epi32(a, 31);
  const auto e = simde_mm_and_si128(simde_mm_srli_epi32(a, 23), Set(0xFF));
  const auto m = simde_mm_and_si128(a, Set(0x7FFFFF));
  result = Select(negative, Set(kQNaN32), result);
  const auto nan = simde_mm_andnot_si128(Equal(m, 0), Equal(e, 0xFF));
  const auto infinity = Select(negative, Set(kQNaN32), a);
  result =
      Select(Equal(e, 0xFF), Select(nan, simde_mm_or_si128(a, Set(kQuiet32)), infinity), result);
  return simde_mm_castsi128_ps(Select(Equal(e, 0), Set(kSign32 | kInfinity32), result));
}

/**
 * Computes vexptefp and vexptefp128.
 *
 * @return The exp2 estimate of each lane.
 */
[[nodiscard]] inline simde__m128 Exp2Estimate(simde__m128 v) noexcept {
  using namespace detail;
  const auto a = simde_mm_castps_si128(v);
  const auto fixed =
      simde_mm_srai_epi32(simde_mm_cvttps_epi32(simde_mm_mul_ps(v, simde_mm_set1_ps(32768.0f))), 4);
  const auto fraction = simde_mm_and_si128(fixed, Set(0x7FF));
  const auto cell = Lookup16<Exp2Cell>(simde_mm_srli_epi32(fraction, 7));
  const auto u = simde_mm_and_si128(fraction, Set(0x7F));
  const auto sum = simde_mm_add_epi32(simde_mm_srli_epi32(cell, 4),
                                      simde_mm_madd_epi16(simde_mm_and_si128(cell, Set(15)), u));
  const auto exponent = simde_mm_add_epi32(simde_mm_srai_epi32(fixed, 11), Set(127));
  auto result = simde_mm_or_si128(simde_mm_slli_epi32(exponent, 23),
                                  simde_mm_and_si128(simde_mm_slli_epi32(sum, 9), Set(0x7FF000)));
  if (!AnyOutside(simde_mm_andnot_si128(Set(kSign32), a), 1u << 23, 0x42FBFFFFu)) [[likely]] {
    return simde_mm_castsi128_ps(result);
  }
  const auto negative = simde_mm_srai_epi32(a, 31);
  const auto e = simde_mm_and_si128(simde_mm_srli_epi32(a, 23), Set(0xFF));
  const auto m = simde_mm_and_si128(a, Set(0x7FFFFF));
  result = simde_mm_andnot_si128(simde_mm_cmpgt_epi32(Set(1), exponent), result);
  const auto large = simde_mm_cmpgt_epi32(e, Set(133));
  result = Select(large, simde_mm_andnot_si128(negative, Set(kInfinity32)), result);
  const auto nan = simde_mm_andnot_si128(Equal(m, 0), Equal(e, 0xFF));
  result = Select(nan, simde_mm_or_si128(a, Set(kQuiet32)), result);
  return simde_mm_castsi128_ps(Select(Equal(e, 0), Set(0x3F800000u), result));
}

namespace detail {

/**
 * Reads lane 0 of a vector estimate applied to an operand.
 *
 * @param bits The operand bits.
 * @return The estimate bits.
 */
template <simde__m128 (*Estimate)(simde__m128)>
inline u32 EstimateOne(u32 bits) {
  return static_cast<u32>(simde_mm_cvtsi128_si32(simde_mm_castps_si128(
      Estimate(simde_mm_castsi128_ps(simde_mm_set1_epi32(static_cast<i32>(bits)))))));
}

/**
 * Reports whether an operand is a denormal other than zero.
 *
 * @param bits The operand bits.
 * @return True when the magnitude is nonzero and below the smallest normal.
 */
inline bool IsDenormal(u32 bits) {
  return (bits & kInfinity32) == 0 && (bits & 0x7FFFFFu) != 0;
}

/**
 * Rewrites a denormal operand as the normal operand of the same value scaled by 2^64.
 *
 * @param bits The denormal operand bits.
 * @return The scaled normal operand bits.
 */
inline u32 ScaleDenormal(u32 bits) {
  const u32 mantissa = bits & 0x7FFFFFu;
  const i32 shift = std::countl_zero(mantissa) - 8;
  const u32 biased = static_cast<u32>(1 - shift + 64);
  return (bits & kSign32) | (biased << 23) | ((mantissa << shift) & 0x7FFFFFu);
}

/**
 * Applies a scalar rule to the lanes of a vector that the fast estimate cannot handle.
 *
 * @tparam Rule The scalar rule, taking an operand and giving the result bits.
 * @tparam Needs True for a lane that takes the rule.
 * @param fast The result of the fast estimate.
 * @return The results with the special lanes replaced.
 */
template <u32 (*Rule)(u32), bool (*Needs)(u32)>
inline simde__m128 ReplaceLanes(simde__m128 v, simde__m128 fast) {
  alignas(16) std::array<u32, 4> in;
  alignas(16) std::array<u32, 4> out;
  simde_mm_store_si128(reinterpret_cast<simde__m128i*>(in.data()), simde_mm_castps_si128(v));
  simde_mm_store_si128(reinterpret_cast<simde__m128i*>(out.data()), simde_mm_castps_si128(fast));
  for (u32 lane = 0; lane < 4; ++lane) {
    if (Needs(in[lane])) {
      out[lane] = Rule(in[lane]);
    }
  }
  return simde_mm_castsi128_ps(
      simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(out.data())));
}

inline bool ReciprocalNeedsRule(u32 bits) {
  const u32 biased = (bits >> 23) & 0xFFu;
  return IsDenormal(bits) || biased == 253 || biased == 254;
}

inline u32 ReciprocalRule(u32 bits) {
  const u32 sign = bits & kSign32;
  const u32 biased = (bits >> 23) & 0xFFu;
  if (IsDenormal(bits)) {
    const u32 estimate = EstimateOne<ReciprocalEstimate>(ScaleDenormal(bits));
    const u32 field = ((estimate >> 23) & 0xFFu) + 64;
    return field > 254 ? (sign | kInfinity32) : (sign | (field << 23) | (estimate & 0x7FFFFFu));
  }
  if (biased == 253 && (bits & 0x7FFFFFu) == 0) {
    return sign | 0x00800000u;
  }
  const u32 estimate = EstimateOne<ReciprocalEstimate>((bits & 0x807FFFFFu) | (252u << 23));
  const u32 shift = biased - 252;
  const u32 full = 0x800000u | (estimate & 0x7FFFFFu);
  const u32 dropped = full & ((1u << shift) - 1);
  const u32 half = 1u << (shift - 1);
  const u32 kept = full >> shift;
  return sign | (kept + ((dropped > half || (dropped == half && (kept & 1))) ? 1u : 0u));
}

inline u32 ReciprocalSqrtRule(u32 bits) {
  const u32 estimate = EstimateOne<ReciprocalSqrtEstimate>(ScaleDenormal(bits));
  if ((estimate & kInfinity32) == kInfinity32) {
    return estimate;
  }
  return estimate + (32u << 23);
}

inline u32 Log2Rule(u32 bits) {
  const u32 estimate = EstimateOne<Log2Estimate>(ScaleDenormal(bits));
  if ((estimate & kInfinity32) == kInfinity32) {
    return estimate;
  }
  return std::bit_cast<u32>(std::bit_cast<f32>(estimate) - 64.0f);
}

}  // namespace detail

/**
 * Computes vrefp and vrefp128 for a fixed VSCR NJ bit.
 *
 * @tparam NonJava The VSCR NJ bit.
 */
template <bool NonJava>
[[nodiscard]] inline simde__m128 ReciprocalEstimate(simde__m128 v) noexcept {
  const auto fast = ReciprocalEstimate(v);
  if constexpr (NonJava) {
    return fast;
  } else {
    return detail::ReplaceLanes<detail::ReciprocalRule, detail::ReciprocalNeedsRule>(v, fast);
  }
}

/**
 * Computes vrefp and vrefp128, honoring VSCR NJ.
 *
 * With NJ clear, denormal operands and results are kept, rounded to nearest even.
 *
 * @param non_java The VSCR NJ bit.
 */
[[nodiscard]] inline simde__m128 ReciprocalEstimate(simde__m128 v, bool non_java) noexcept {
  return non_java ? ReciprocalEstimate<true>(v) : ReciprocalEstimate<false>(v);
}

/**
 * Computes vrsqrtefp and vrsqrtefp128 for a fixed VSCR NJ bit.
 *
 * @tparam NonJava The VSCR NJ bit.
 */
template <bool NonJava>
[[nodiscard]] inline simde__m128 ReciprocalSqrtEstimate(simde__m128 v) noexcept {
  const auto fast = ReciprocalSqrtEstimate(v);
  if constexpr (NonJava) {
    return fast;
  } else {
    return detail::ReplaceLanes<detail::ReciprocalSqrtRule, detail::IsDenormal>(v, fast);
  }
}

/**
 * Computes vrsqrtefp and vrsqrtefp128, honoring VSCR NJ.
 *
 * With NJ clear a denormal operand is estimated at its value.
 *
 * @param non_java The VSCR NJ bit.
 * @return The reciprocal square root estimate of each lane.
 */
[[nodiscard]] inline simde__m128 ReciprocalSqrtEstimate(simde__m128 v, bool non_java) noexcept {
  return non_java ? ReciprocalSqrtEstimate<true>(v) : ReciprocalSqrtEstimate<false>(v);
}

/**
 * Computes vlogefp and vlogefp128 for a fixed VSCR NJ bit.
 *
 * @tparam NonJava The VSCR NJ bit.
 */
template <bool NonJava>
[[nodiscard]] inline simde__m128 Log2Estimate(simde__m128 v) noexcept {
  const auto fast = Log2Estimate(v);
  if constexpr (NonJava) {
    return fast;
  } else {
    return detail::ReplaceLanes<detail::Log2Rule, detail::IsDenormal>(v, fast);
  }
}

/**
 * Computes vlogefp and vlogefp128, honoring VSCR NJ.
 *
 * With NJ clear a denormal operand is estimated at its value.
 *
 * @param non_java The VSCR NJ bit.
 * @return The log2 estimate of each lane.
 */
[[nodiscard]] inline simde__m128 Log2Estimate(simde__m128 v, bool non_java) noexcept {
  return non_java ? Log2Estimate<true>(v) : Log2Estimate<false>(v);
}

/**
 * Computes vexptefp and vexptefp128. A denormal operand gives 1.0 for either NJ value.
 *
 * @param non_java The VSCR NJ bit.
 * @return The exp2 estimate of each lane.
 */
[[nodiscard]] inline simde__m128 Exp2Estimate(simde__m128 v, bool non_java) noexcept {
  static_cast<void>(non_java);
  return Exp2Estimate(v);
}

}  // namespace simd

}  // namespace rex::ppc

namespace rex::ppc::simd {

namespace detail {

/** The product of two operands as an aligned fixed point term. */
struct DotTerm {
  bool negative;
  u64 mantissa;
  i32 exponent;
};

/**
 * Splits two operands into the signed product term. A denormal operand is zero when flush is set.
 *
 * @param a The first operand bits.
 * @param b The second operand bits.
 * @param flush True to take a denormal operand as a zero.
 * @return The term, with a zero mantissa when either operand is zero.
 */
inline DotTerm MakeDotTerm(u32 a, u32 b, bool flush) {
  const auto split = [flush](u32 bits, u32& mantissa, i32& exponent) {
    const u32 biased = (bits >> 23) & 0xFFu;
    mantissa = biased == 0 ? 0 : ((bits & 0x7FFFFFu) | 0x800000u);
    exponent = static_cast<i32>(biased) - 127;
    if (biased == 0 && !flush && (bits & 0x7FFFFFu) != 0) {
      const i32 shift = std::countl_zero(bits & 0x7FFFFFu) - 8;
      mantissa = (bits & 0x7FFFFFu) << shift;
      exponent = -126 - shift;
    }
  };
  u32 mantissa_a;
  u32 mantissa_b;
  i32 exponent_a;
  i32 exponent_b;
  split(a, mantissa_a, exponent_a);
  split(b, mantissa_b, exponent_b);
  return {((a ^ b) & kSign32) != 0, u64{mantissa_a} * mantissa_b, exponent_a + exponent_b - 46};
}

/**
 * Normalizes the fixed point sum of a dot product to the result bits.
 *
 * @param sum The sum of the aligned terms.
 * @param majority_negative True when the majority sign is negative.
 * @param lowest The exponent of the lowest fixed point bit.
 * @return The result bits.
 */
inline u32 FinishDot(i64 sum, bool majority_negative, i32 lowest) {
  const i64 flip = sum >> 63;
  const u32 sign = (majority_negative ? kSign32 : 0u) ^ (static_cast<u32>(flip) & kSign32);
  const u64 magnitude = static_cast<u64>(sum ^ flip);
  if (magnitude == 0) {
    return 0;
  }
  const u32 width = std::bit_width(magnitude);
  const i32 exponent = lowest + static_cast<i32>(width) - 1;
  const u32 top = static_cast<u32>((magnitude << (64 - width)) >> 40);
  if (static_cast<u32>(exponent + 126) < 254u) [[likely]] {
    return sign | (static_cast<u32>(exponent + 127) << 23) | (top & 0x7FFFFFu);
  }
  if (exponent < -127) {
    return sign;
  }
  if (exponent == -127) {
    return sign | (top & 0x7FFFFFu);
  }
  if (exponent > 128) {
    return sign | kQNaN32;
  }
  return sign | (static_cast<u32>(exponent + 127) << 23) | (top & 0x7FFFFFu);
}

/**
 * Computes the dot product of the first terms of two operand arrays as the 128 bit dot product
 * instructions do. Terms add as truncated fixed point with 29 fractional bits below the largest.
 *
 * @tparam Terms The number of terms, 3 or 4.
 * @param a The first operand words.
 * @param b The second operand words.
 * @param flush True to flush a denormal operand to a zero, as VSCR NJ set does.
 * @return The result bits.
 */
template <u32 Terms>
inline u32 DotProductReference(const std::array<u32, Terms>& a, const std::array<u32, Terms>& b,
                               bool flush = true) {
  bool any_nan = false;
  bool any_invalid = false;
  u32 infinities = 0;
  u32 negative_infinities = 0;
  std::array<DotTerm, Terms> terms{};
  for (u32 i = 0; i < Terms; ++i) {
    const u32 x = flush && (a[i] & 0x7F800000u) == 0 ? (a[i] & kSign32) : a[i];
    const u32 y = flush && (b[i] & 0x7F800000u) == 0 ? (b[i] & kSign32) : b[i];
    const bool nan_x = (x & 0x7FFFFFFFu) > kInfinity32;
    const bool nan_y = (y & 0x7FFFFFFFu) > kInfinity32;
    const bool inf_x = (x & 0x7FFFFFFFu) == kInfinity32;
    const bool inf_y = (y & 0x7FFFFFFFu) == kInfinity32;
    const bool zero_x = (x & 0x7FFFFFFFu) == 0;
    const bool zero_y = (y & 0x7FFFFFFFu) == 0;
    any_nan |= nan_x || nan_y;
    any_invalid |= (inf_x && zero_y) || (inf_y && zero_x);
    if ((inf_x || inf_y) && !zero_x && !zero_y) {
      ++infinities;
      negative_infinities += ((x ^ y) & kSign32) != 0 ? 1 : 0;
    }
    terms[i] = MakeDotTerm(x, y, flush);
  }
  if (any_nan || any_invalid) {
    return kQNaN32;
  }
  if (infinities != 0) {
    if (negative_infinities == 0) {
      return kInfinity32;
    }
    return negative_infinities == infinities ? (kInfinity32 | kSign32) : kQNaN32;
  }
  u32 negatives = 0;
  bool any_live = false;
  i32 highest = 0;
  for (const DotTerm& term : terms) {
    negatives += term.negative ? 1 : 0;
    if (term.mantissa != 0 && (!any_live || term.exponent > highest)) {
      highest = term.exponent;
      any_live = true;
    }
  }
  if (!any_live) {
    return negatives == Terms ? kSign32 : 0u;
  }
  const bool majority_negative = 2 * negatives > Terms;
  const i32 lowest = highest + 47 - 29;
  i64 sum = 0;
  for (const DotTerm& term : terms) {
    u64 aligned = 0;
    if (term.mantissa != 0) {
      const i32 shift = lowest - term.exponent;
      aligned = shift >= 64 ? 0 : (term.mantissa >> shift);
    }
    const i64 value = static_cast<i64>(aligned);
    sum += term.negative == majority_negative ? value : ~value;
  }
  return FinishDot(sum, majority_negative, lowest);
}

/**
 * Computes the dot product of host lanes with no NaN or infinity, matching DotProductReference.
 *
 * @tparam Terms The number of terms, 3 or 4, taken from the highest host lanes.
 * @param words_a The first operand bits.
 * @param words_b The second operand bits.
 * @param biased_a The biased exponent fields of the first operand.
 * @param biased_b The biased exponent fields of the second operand.
 * @return The result bits.
 */
template <u32 Terms>
inline u32 DotProductFinite(simde__m128i words_a, simde__m128i words_b, simde__m128i biased_a,
                            simde__m128i biased_b) {
  constexpr i32 kNone = -0x40000000;
  const auto used = simde_mm_set_epi32(-1, -1, -1, Terms == 4 ? -1 : 0);
  const auto zero = simde_mm_setzero_si128();
  const auto live_a = simde_mm_and_si128(simde_mm_cmpgt_epi32(biased_a, zero), used);
  const auto live_b = simde_mm_and_si128(simde_mm_cmpgt_epi32(biased_b, zero), used);
  const auto mantissa_a = simde_mm_and_si128(
      simde_mm_or_si128(simde_mm_and_si128(words_a, Set(0x7FFFFFu)), Set(0x800000u)), live_a);
  const auto mantissa_b = simde_mm_and_si128(
      simde_mm_or_si128(simde_mm_and_si128(words_b, Set(0x7FFFFFu)), Set(0x800000u)), live_b);
  const auto exponent = simde_mm_sub_epi32(simde_mm_add_epi32(biased_a, biased_b), Set(300));
  const auto live = simde_mm_and_si128(live_a, live_b);
  const auto candidate = Select(live, exponent, Set(static_cast<u32>(kNone)));
  auto highest_lanes = simde_mm_max_epi32(candidate, simde_mm_shuffle_epi32(candidate, 0x4E));
  highest_lanes = simde_mm_max_epi32(highest_lanes, simde_mm_shuffle_epi32(highest_lanes, 0xB1));
  const i32 highest = simde_mm_cvtsi128_si32(highest_lanes);
  const auto negative = simde_mm_srai_epi32(simde_mm_xor_si128(words_a, words_b), 31);
  const u32 negative_bits = static_cast<u32>(
      simde_mm_movemask_ps(simde_mm_castsi128_ps(simde_mm_and_si128(negative, used))));
  const u32 negatives = static_cast<u32>((0x4332322132212110ull >> (negative_bits * 4)) & 0xFu);
  if (highest == kNone) {
    return negatives == Terms ? kSign32 : 0u;
  }
  const bool majority = 2 * negatives > Terms;
  const auto minority = simde_mm_and_si128(
      simde_mm_xor_si128(negative, simde_mm_set1_epi32(-static_cast<i32>(majority))), used);
  const i32 lowest = highest + 47 - 29;
#if defined(__AVX2__)
  const auto product = simde_mm256_mul_epu32(simde_mm256_cvtepu32_epi64(mantissa_a),
                                             simde_mm256_cvtepu32_epi64(mantissa_b));
  const auto shift =
      simde_mm256_cvtepi32_epi64(simde_mm_sub_epi32(simde_mm_set1_epi32(lowest), exponent));
  const auto aligned = simde_mm256_srlv_epi64(product, shift);
  const auto terms = simde_mm256_xor_si256(aligned, simde_mm256_cvtepi32_epi64(minority));
  const auto pair = simde_mm_add_epi64(simde_mm256_castsi256_si128(terms),
                                       simde_mm256_extracti128_si256(terms, 1));
  const i64 total = simde_mm_cvtsi128_si64(pair) + simde_mm_extract_epi64(pair, 1);
  return FinishDot(total, majority, lowest);
#else
  const auto field = simde_mm_add_epi32(simde_mm_sub_epi32(exponent, simde_mm_set1_epi32(lowest)),
                                        simde_mm_set1_epi32(1023));
  const auto upper = [](simde__m128i v) { return simde_mm_shuffle_epi32(v, 0xEE); };
  const auto scale_low =
      simde_mm_castsi128_pd(simde_mm_slli_epi64(simde_mm_cvtepu32_epi64(field), 52));
  const auto scale_high =
      simde_mm_castsi128_pd(simde_mm_slli_epi64(simde_mm_cvtepu32_epi64(upper(field)), 52));
  const auto product_low =
      simde_mm_mul_pd(simde_mm_cvtepi32_pd(mantissa_a), simde_mm_cvtepi32_pd(mantissa_b));
  const auto product_high = simde_mm_mul_pd(simde_mm_cvtepi32_pd(upper(mantissa_a)),
                                            simde_mm_cvtepi32_pd(upper(mantissa_b)));
  const auto aligned =
      simde_mm_unpacklo_epi64(simde_mm_cvttpd_epi32(simde_mm_mul_pd(product_low, scale_low)),
                              simde_mm_cvttpd_epi32(simde_mm_mul_pd(product_high, scale_high)));
  const auto terms = simde_mm_xor_si128(aligned, minority);
  const auto pair =
      simde_mm_add_epi64(simde_mm_cvtepi32_epi64(terms), simde_mm_cvtepi32_epi64(upper(terms)));
  const i64 total = simde_mm_cvtsi128_si64(pair) + simde_mm_extract_epi64(pair, 1);
  return FinishDot(total, majority, lowest);
#endif
}

/**
 * Computes the dot product of the first elements of two vectors and broadcasts it to all lanes.
 *
 * @tparam Terms The number of elements, 3 or 4.
 * @param flush True to flush a denormal operand to a zero, as VSCR NJ set does.
 * @return The result in all 4 lanes.
 */
template <u32 Terms>
inline simde__m128 DotProductVector(simde__m128 a, simde__m128 b, bool flush) {
  const auto words_a = simde_mm_castps_si128(a);
  const auto words_b = simde_mm_castps_si128(b);
  const auto used = simde_mm_set_epi32(-1, -1, -1, Terms == 4 ? -1 : 0);
  const auto biased_a = simde_mm_srli_epi32(simde_mm_and_si128(words_a, Set(kInfinity32)), 23);
  const auto biased_b = simde_mm_srli_epi32(simde_mm_and_si128(words_b, Set(kInfinity32)), 23);
  const auto infinite = simde_mm_set1_epi32(0xFF);
  auto special = simde_mm_or_si128(simde_mm_cmpeq_epi32(biased_a, infinite),
                                   simde_mm_cmpeq_epi32(biased_b, infinite));
  if (!flush) [[unlikely]] {
    const auto zero = simde_mm_setzero_si128();
    const auto denormal_a = simde_mm_andnot_si128(
        simde_mm_cmpeq_epi32(simde_mm_and_si128(words_a, Set(0x7FFFFFu)), zero),
        Equal(simde_mm_and_si128(words_a, Set(kInfinity32)), 0));
    const auto denormal_b = simde_mm_andnot_si128(
        simde_mm_cmpeq_epi32(simde_mm_and_si128(words_b, Set(0x7FFFFFu)), zero),
        Equal(simde_mm_and_si128(words_b, Set(kInfinity32)), 0));
    special = simde_mm_or_si128(special, simde_mm_or_si128(denormal_a, denormal_b));
  }
  special = simde_mm_and_si128(special, used);
  u32 result;
  if (simde_mm_movemask_epi8(special) == 0) {
    result = DotProductFinite<Terms>(words_a, words_b, biased_a, biased_b);
  } else {
    alignas(16) std::array<u32, 4> host_a;
    alignas(16) std::array<u32, 4> host_b;
    simde_mm_store_si128(reinterpret_cast<simde__m128i*>(host_a.data()), words_a);
    simde_mm_store_si128(reinterpret_cast<simde__m128i*>(host_b.data()), words_b);
    std::array<u32, Terms> terms_a;
    std::array<u32, Terms> terms_b;
    for (u32 i = 0; i < Terms; ++i) {
      terms_a[i] = host_a[3 - i];
      terms_b[i] = host_b[3 - i];
    }
    result = DotProductReference<Terms>(terms_a, terms_b, flush);
  }
  return simde_mm_castsi128_ps(simde_mm_set1_epi32(static_cast<i32>(result)));
}

}  // namespace detail

/**
 * Computes vmsum3fp128, the dot product of the first 3 vector elements.
 *
 * @return The dot product in all 4 lanes.
 */
[[nodiscard]] inline simde__m128 DotProduct3(simde__m128 a, simde__m128 b) noexcept {
  return detail::DotProductVector<3>(a, b, true);
}

/**
 * Computes vmsum3fp128, the dot product of the first 3 elements, honoring VSCR NJ.
 *
 * @param non_java The VSCR NJ bit. When clear a denormal operand keeps its value.
 * @return The dot product in all 4 lanes.
 */
[[nodiscard]] inline simde__m128 DotProduct3(simde__m128 a, simde__m128 b, bool non_java) noexcept {
  return detail::DotProductVector<3>(a, b, non_java);
}

/**
 * Computes vmsum4fp128, the dot product of all 4 vector elements.
 *
 * @return The dot product in all 4 lanes.
 */
[[nodiscard]] inline simde__m128 DotProduct4(simde__m128 a, simde__m128 b) noexcept {
  return detail::DotProductVector<4>(a, b, true);
}

/**
 * Computes vmsum4fp128, the dot product of all 4 elements, honoring VSCR NJ.
 *
 * @param non_java The VSCR NJ bit. When clear a denormal operand keeps its value.
 * @return The dot product in all 4 lanes.
 */
[[nodiscard]] inline simde__m128 DotProduct4(simde__m128 a, simde__m128 b, bool non_java) noexcept {
  return detail::DotProductVector<4>(a, b, non_java);
}

}  // namespace rex::ppc::simd

namespace rex::ppc::simd {

namespace detail {

/**
 * Replaces each denormal lane with a zero of the same sign.
 *
 * @return The lanes with the denormals flushed.
 */
inline simde__m128i FlushDenormals(simde__m128i v) {
  const auto denormal = Equal(simde_mm_and_si128(v, Set(kInfinity32)), 0);
  return Select(denormal, simde_mm_and_si128(v, Set(kSign32)), v);
}

/**
 * Adds two f64 lanes and rounds the sum to odd, so that a later narrowing rounds once.
 *
 * @return The sum with an inexact result forced to an odd last bit.
 */
inline simde__m128d SumToOdd(simde__m128d p, simde__m128d c) {
  const auto s = simde_mm_add_pd(p, c);
  const auto virtual_c = simde_mm_sub_pd(s, p);
  const auto err = simde_mm_add_pd(simde_mm_sub_pd(p, simde_mm_sub_pd(s, virtual_c)),
                                   simde_mm_sub_pd(c, virtual_c));
  const auto inexact =
      simde_mm_and_pd(simde_mm_cmpord_pd(err, err), simde_mm_cmpneq_pd(err, simde_mm_setzero_pd()));
  const auto bits = simde_mm_castpd_si128(s);
  const auto even = simde_mm_cmpeq_epi64(simde_mm_and_si128(bits, simde_mm_set1_epi64x(1)),
                                         simde_mm_setzero_si128());
  const auto differ = simde_mm_srli_epi64(simde_mm_xor_si128(bits, simde_mm_castpd_si128(err)), 63);
  const auto step = simde_mm_sub_epi64(simde_mm_set1_epi64x(1), simde_mm_slli_epi64(differ, 1));
  const auto adjust =
      simde_mm_and_si128(simde_mm_castpd_si128(inexact), simde_mm_and_si128(even, step));
  return simde_mm_castsi128_pd(simde_mm_add_epi64(bits, adjust));
}

/**
 * Replaces each f64 lane below the smallest f32 normal with a zero of the same sign.
 *
 * @return The lanes with the tiny magnitudes flushed.
 */
inline simde__m128d FlushTiny(simde__m128d s) {
  const auto sign = simde_mm_castsi128_pd(simde_mm_set1_epi64x(static_cast<i64>(kSign64)));
  const auto tiny = simde_mm_cmplt_pd(simde_mm_andnot_pd(sign, s), simde_mm_set1_pd(0x1p-126));
  return simde_mm_blendv_pd(s, simde_mm_and_pd(s, sign), tiny);
}

/**
 * Computes a * b + c with a single rounding on the two low lanes.
 *
 * @param flush True to flush a result below the smallest f32 normal to a zero.
 * @return The two rounded f32 results in the low half.
 */
inline simde__m128 MultiplyAddHalf(simde__m128 a, simde__m128 b, simde__m128 c, bool flush) {
  const auto product = simde_mm_mul_pd(simde_mm_cvtps_pd(a), simde_mm_cvtps_pd(b));
  const auto sum = SumToOdd(product, simde_mm_cvtps_pd(c));
  return simde_mm_cvtpd_ps(flush ? FlushTiny(sum) : sum);
}

/**
 * Computes the fused multiply add of 4 lanes, or its negated subtract form, for any operands.
 *
 * @tparam Negate True for -(a * b - c).
 * @param flush True for VSCR NJ set, which flushes denormal operands and results to a zero.
 */
template <bool Negate>
inline simde__m128 MultiplyAddFull(simde__m128 a, simde__m128 b, simde__m128 c, bool flush) {
  const auto bits_a = simde_mm_castps_si128(a);
  const auto bits_b = simde_mm_castps_si128(b);
  const auto bits_c = simde_mm_castps_si128(c);
  const auto flushed_a = flush ? simde_mm_castsi128_ps(FlushDenormals(bits_a)) : a;
  const auto flushed_b = flush ? simde_mm_castsi128_ps(FlushDenormals(bits_b)) : b;
  auto addend = flush ? FlushDenormals(bits_c) : bits_c;
  if constexpr (Negate) {
    addend = simde_mm_xor_si128(addend, Set(kSign32));
  }
  const auto addend_f = simde_mm_castsi128_ps(addend);
  const auto low = MultiplyAddHalf(flushed_a, flushed_b, addend_f, flush);
  const auto high = MultiplyAddHalf(simde_mm_movehl_ps(flushed_a, flushed_a),
                                    simde_mm_movehl_ps(flushed_b, flushed_b),
                                    simde_mm_movehl_ps(addend_f, addend_f), flush);
  auto result = simde_mm_castps_si128(simde_mm_movelh_ps(low, high));
  if constexpr (Negate) {
    result = simde_mm_xor_si128(result, Set(kSign32));
  }
  const auto invalid = simde_mm_castps_si128(
      simde_mm_cmpunord_ps(simde_mm_castsi128_ps(result), simde_mm_castsi128_ps(result)));
  result = Select(invalid, Set(kQNaN32), result);
  for (const auto operand : {bits_b, bits_c, bits_a}) {
    const auto nan = simde_mm_castps_si128(
        simde_mm_cmpunord_ps(simde_mm_castsi128_ps(operand), simde_mm_castsi128_ps(operand)));
    result = Select(nan, simde_mm_or_si128(operand, Set(kQuiet32)), result);
  }
  return simde_mm_castsi128_ps(result);
}

/**
 * Computes the fused multiply add of 4 lanes, or its negated subtract form, with f64 sums when VSCR
 * NJ is set and every lane sums exactly in f64. Every other case takes MultiplyAddFull.
 *
 * @tparam Negate True for -(a * b - c).
 * @param flush True for VSCR NJ set, which flushes denormal operands and results to a zero.
 */
template <bool Negate>
inline simde__m128 MultiplyAddWide(simde__m128 a, simde__m128 b, simde__m128 c, bool flush) {
  if (!flush) [[unlikely]] {
    return MultiplyAddFull<Negate>(a, b, c, flush);
  }
  const auto flushed_a = simde_mm_castsi128_ps(FlushDenormals(simde_mm_castps_si128(a)));
  const auto flushed_b = simde_mm_castsi128_ps(FlushDenormals(simde_mm_castps_si128(b)));
  const auto flushed_c = simde_mm_castsi128_ps(FlushDenormals(simde_mm_castps_si128(c)));
  const auto exponent_mask = Set(kInfinity32);
  const auto biased_a =
      simde_mm_srli_epi32(simde_mm_and_si128(simde_mm_castps_si128(flushed_a), exponent_mask), 23);
  const auto biased_b =
      simde_mm_srli_epi32(simde_mm_and_si128(simde_mm_castps_si128(flushed_b), exponent_mask), 23);
  const auto biased_c =
      simde_mm_srli_epi32(simde_mm_and_si128(simde_mm_castps_si128(flushed_c), exponent_mask), 23);
  const auto all_ones = simde_mm_set1_epi32(0xFF);
  const auto special =
      simde_mm_or_si128(simde_mm_or_si128(simde_mm_cmpeq_epi32(biased_a, all_ones),
                                          simde_mm_cmpeq_epi32(biased_b, all_ones)),
                        simde_mm_cmpeq_epi32(biased_c, all_ones));
  const auto zero = simde_mm_setzero_si128();
  const auto any_zero = simde_mm_or_si128(
      simde_mm_or_si128(simde_mm_cmpeq_epi32(biased_a, zero), simde_mm_cmpeq_epi32(biased_b, zero)),
      simde_mm_cmpeq_epi32(biased_c, zero));
  const auto gap = simde_mm_sub_epi32(simde_mm_add_epi32(biased_a, biased_b),
                                      simde_mm_add_epi32(biased_c, simde_mm_set1_epi32(127)));
  const auto close = simde_mm_and_si128(simde_mm_cmpgt_epi32(gap, simde_mm_set1_epi32(-5)),
                                        simde_mm_cmpgt_epi32(simde_mm_set1_epi32(28), gap));
  const auto exact = simde_mm_or_si128(any_zero, close);
  if (simde_mm_movemask_epi8(simde_mm_or_si128(
          special, simde_mm_andnot_si128(exact, simde_mm_cmpeq_epi32(zero, zero)))) != 0)
      [[unlikely]] {
    return MultiplyAddFull<Negate>(a, b, c, flush);
  }
  auto addend = simde_mm_castps_si128(flushed_c);
  if constexpr (Negate) {
    addend = simde_mm_xor_si128(addend, Set(kSign32));
  }
  const auto addend_f = simde_mm_castsi128_ps(addend);
  const auto sum_low =
      simde_mm_add_pd(simde_mm_mul_pd(simde_mm_cvtps_pd(flushed_a), simde_mm_cvtps_pd(flushed_b)),
                      simde_mm_cvtps_pd(addend_f));
  const auto sum_high =
      simde_mm_add_pd(simde_mm_mul_pd(simde_mm_cvtps_pd(simde_mm_movehl_ps(flushed_a, flushed_a)),
                                      simde_mm_cvtps_pd(simde_mm_movehl_ps(flushed_b, flushed_b))),
                      simde_mm_cvtps_pd(simde_mm_movehl_ps(addend_f, addend_f)));
  auto result = simde_mm_castps_si128(simde_mm_movelh_ps(simde_mm_cvtpd_ps(FlushTiny(sum_low)),
                                                         simde_mm_cvtpd_ps(FlushTiny(sum_high))));
  if constexpr (Negate) {
    result = simde_mm_xor_si128(result, Set(kSign32));
  }
  return simde_mm_castsi128_ps(result);
}

#if defined(__FMA__)
/**
 * Computes the fused multiply add of 4 lanes, or its negated subtract form, with the host fused
 * instruction. A NaN result lane and a result of exactly the smallest normal take MultiplyAddFull,
 * since the host detects a tiny result after rounding and the guest before it.
 *
 * @tparam Negate True for -(a * b - c).
 */
template <bool Negate>
inline simde__m128 MultiplyAddFused(simde__m128 a, simde__m128 b, simde__m128 c) {
  const auto flushed_a = simde_mm_castsi128_ps(FlushDenormals(simde_mm_castps_si128(a)));
  const auto flushed_b = simde_mm_castsi128_ps(FlushDenormals(simde_mm_castps_si128(b)));
  const auto flushed_c = simde_mm_castsi128_ps(FlushDenormals(simde_mm_castps_si128(c)));
  const auto raw = Negate ? simde_mm_fmsub_ps(flushed_a, flushed_b, flushed_c)
                          : simde_mm_fmadd_ps(flushed_a, flushed_b, flushed_c);
  const auto bits = simde_mm_castps_si128(raw);
  const auto magnitude = simde_mm_andnot_si128(Set(kSign32), bits);
  const auto unordered = simde_mm_castps_si128(simde_mm_cmpunord_ps(raw, raw));
  const auto smallest = simde_mm_cmpeq_epi32(magnitude, Set(0x00800000u));
  if (simde_mm_movemask_epi8(simde_mm_or_si128(unordered, smallest)) != 0) [[unlikely]] {
    return MultiplyAddFull<Negate>(a, b, c, true);
  }
  const auto tiny = simde_mm_cmpgt_epi32(Set(0x00800000u), magnitude);
  auto result = Select(tiny, simde_mm_and_si128(bits, Set(kSign32)), bits);
  if constexpr (Negate) {
    result = simde_mm_xor_si128(result, Set(kSign32));
  }
  return simde_mm_castsi128_ps(result);
}
#endif

/**
 * Computes the fused multiply add of 4 lanes, or its negated subtract form, for any operands.
 *
 * @tparam Negate True for -(a * b - c).
 * @param flush True for VSCR NJ set, which flushes denormal operands and results to a zero.
 */
template <bool Negate>
inline simde__m128 MultiplyAdd(simde__m128 a, simde__m128 b, simde__m128 c, bool flush) {
#if defined(__FMA__)
  if (flush) [[likely]] {
    return MultiplyAddFused<Negate>(a, b, c);
  }
  return MultiplyAddFull<Negate>(a, b, c, flush);
#else
  return MultiplyAddWide<Negate>(a, b, c, flush);
#endif
}

}  // namespace detail

/**
 * Computes vmaddfp, vmaddfp128 and vmaddcfp128 with a single rounding.
 *
 * @return a * b + c in each lane.
 */
[[nodiscard]] inline simde__m128 MultiplyAdd(simde__m128 a, simde__m128 b, simde__m128 c) noexcept {
  return detail::MultiplyAdd<false>(a, b, c, true);
}

/**
 * Computes vmaddfp, vmaddfp128 and vmaddcfp128 with a single rounding, honoring VSCR NJ.
 *
 * @param non_java The VSCR NJ bit. When clear a denormal operand and a denormal result are kept.
 * @return a * b + c in each lane.
 */
[[nodiscard]] inline simde__m128 MultiplyAdd(simde__m128 a, simde__m128 b, simde__m128 c,
                                             bool non_java) noexcept {
  return detail::MultiplyAdd<false>(a, b, c, non_java);
}

/**
 * Computes vnmsubfp and vnmsubfp128 with a single rounding.
 *
 * @param c The value subtracted from the product.
 * @return -(a * b - c) in each lane.
 */
[[nodiscard]] inline simde__m128 NegativeMultiplySubtract(simde__m128 a, simde__m128 b,
                                                          simde__m128 c) noexcept {
  return detail::MultiplyAdd<true>(a, b, c, true);
}

/**
 * Computes vnmsubfp and vnmsubfp128 with a single rounding, honoring VSCR NJ.
 *
 * @param c The value subtracted from the product.
 * @param non_java The VSCR NJ bit. When clear a denormal operand and a denormal result are kept.
 * @return -(a * b - c) in each lane.
 */
[[nodiscard]] inline simde__m128 NegativeMultiplySubtract(simde__m128 a, simde__m128 b,
                                                          simde__m128 c, bool non_java) noexcept {
  return detail::MultiplyAdd<true>(a, b, c, non_java);
}

}  // namespace rex::ppc::simd
