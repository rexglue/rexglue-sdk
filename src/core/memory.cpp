/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

/*
 * Byte swap routines are based on GNU Radio volk.
 * https://github.com/gnuradio/volk/blob/master/kernels/volk/volk_16u_byteswap.h
 * https://github.com/gnuradio/volk/blob/master/kernels/volk/volk_32u_byteswap.h
 * https://github.com/gnuradio/volk/blob/master/kernels/volk/volk_64u_byteswap.h
 * Original links
 * https://gnuradio.org/redmine/projects/gnuradio/repository/revisions/cb32b70b79f430456208a2cd521d028e0ece5d5b/entry/volk/kernels/volk/volk_16u_byteswap.h
 * https://gnuradio.org/redmine/projects/gnuradio/repository/revisions/f2bc76cc65ffba51a141950f98e75364e49df874/entry/volk/kernels/volk/volk_32u_byteswap.h
 * https://gnuradio.org/redmine/projects/gnuradio/repository/revisions/2c4c371885c31222362f70a1cd714415d1398021/entry/volk/kernels/volk/volk_64u_byteswap.h
 */

#include <rex/memory/utils.h>
#include <rex/platform.h>

#if REX_ARCH_AMD64
#include <simde/x86/ssse3.h>
#elif REX_ARCH_ARM64
#include <arm_neon.h>
#endif

#include <algorithm>

namespace rex {
namespace memory {

void copy_128_aligned(void* dest, const void* src, size_t count) {
  std::memcpy(dest, src, count * 16);
}

#if REX_ARCH_AMD64

/**
 * Returns early when x is a compile time true constant, working around GCC bug 100801.
 *
 * @param x The condition.
 *
 * @code
 * REX_WORKAROUND_CONSTANT_RETURN_IF(count % 8 == 0);
 * @endcode
 */
#if REX_COMPILER_GNUC
#define REX_WORKAROUND_CONSTANT_RETURN_IF(x) \
  if (__builtin_constant_p(x) && (x))        \
    return;
#else
#define REX_WORKAROUND_CONSTANT_RETURN_IF(x)
#endif
void copy_and_swap_16_aligned(void* dest_ptr, const void* src_ptr, size_t count) {
  REX_ASSERT((reinterpret_cast<uintptr_t>(dest_ptr) & 0xF) == 0);
  REX_ASSERT((reinterpret_cast<uintptr_t>(src_ptr) & 0xF) == 0);

  auto dest = reinterpret_cast<u16*>(dest_ptr);
  auto src = reinterpret_cast<const u16*>(src_ptr);
  simde__m128i shufmask = simde_mm_set_epi8(0x0E, 0x0F, 0x0C, 0x0D, 0x0A, 0x0B, 0x08, 0x09, 0x06,
                                            0x07, 0x04, 0x05, 0x02, 0x03, 0x00, 0x01);

  size_t i = 0;
  for (i = 0; i + 8 <= count; i += 8) {
    simde__m128i input = simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(&src[i]));
    simde__m128i output = simde_mm_shuffle_epi8(input, shufmask);
    simde_mm_store_si128(reinterpret_cast<simde__m128i*>(&dest[i]), output);
  }
  REX_WORKAROUND_CONSTANT_RETURN_IF(count % 8 == 0);
  for (; i < count; ++i) {
    dest[i] = byte_swap(src[i]);
  }
}

void copy_and_swap_16_unaligned(void* dest_ptr, const void* src_ptr, size_t count) {
  auto dest = reinterpret_cast<u16*>(dest_ptr);
  auto src = reinterpret_cast<const u16*>(src_ptr);
  simde__m128i shufmask = simde_mm_set_epi8(0x0E, 0x0F, 0x0C, 0x0D, 0x0A, 0x0B, 0x08, 0x09, 0x06,
                                            0x07, 0x04, 0x05, 0x02, 0x03, 0x00, 0x01);

  size_t i;
  for (i = 0; i + 8 <= count; i += 8) {
    simde__m128i input = simde_mm_loadu_si128(reinterpret_cast<const simde__m128i*>(&src[i]));
    simde__m128i output = simde_mm_shuffle_epi8(input, shufmask);
    simde_mm_storeu_si128(reinterpret_cast<simde__m128i*>(&dest[i]), output);
  }
  REX_WORKAROUND_CONSTANT_RETURN_IF(count % 8 == 0);
  for (; i < count; ++i) {
    dest[i] = byte_swap(src[i]);
  }
}

void copy_and_swap_32_aligned(void* dest_ptr, const void* src_ptr, size_t count) {
  REX_ASSERT((reinterpret_cast<uintptr_t>(dest_ptr) & 0xF) == 0);
  REX_ASSERT((reinterpret_cast<uintptr_t>(src_ptr) & 0xF) == 0);

  auto dest = reinterpret_cast<u32*>(dest_ptr);
  auto src = reinterpret_cast<const u32*>(src_ptr);
  simde__m128i shufmask = simde_mm_set_epi8(0x0C, 0x0D, 0x0E, 0x0F, 0x08, 0x09, 0x0A, 0x0B, 0x04,
                                            0x05, 0x06, 0x07, 0x00, 0x01, 0x02, 0x03);

  size_t i;
  for (i = 0; i + 4 <= count; i += 4) {
    simde__m128i input = simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(&src[i]));
    simde__m128i output = simde_mm_shuffle_epi8(input, shufmask);
    simde_mm_store_si128(reinterpret_cast<simde__m128i*>(&dest[i]), output);
  }
  REX_WORKAROUND_CONSTANT_RETURN_IF(count % 4 == 0);
  for (; i < count; ++i) {
    dest[i] = byte_swap(src[i]);
  }
}

void copy_and_swap_32_unaligned(void* dest_ptr, const void* src_ptr, size_t count) {
  auto dest = reinterpret_cast<u32*>(dest_ptr);
  auto src = reinterpret_cast<const u32*>(src_ptr);
  simde__m128i shufmask = simde_mm_set_epi8(0x0C, 0x0D, 0x0E, 0x0F, 0x08, 0x09, 0x0A, 0x0B, 0x04,
                                            0x05, 0x06, 0x07, 0x00, 0x01, 0x02, 0x03);

  size_t i;
  for (i = 0; i + 4 <= count; i += 4) {
    simde__m128i input = simde_mm_loadu_si128(reinterpret_cast<const simde__m128i*>(&src[i]));
    simde__m128i output = simde_mm_shuffle_epi8(input, shufmask);
    simde_mm_storeu_si128(reinterpret_cast<simde__m128i*>(&dest[i]), output);
  }
  REX_WORKAROUND_CONSTANT_RETURN_IF(count % 4 == 0);
  for (; i < count; ++i) {
    dest[i] = byte_swap(src[i]);
  }
}

void copy_and_swap_64_aligned(void* dest_ptr, const void* src_ptr, size_t count) {
  REX_ASSERT((reinterpret_cast<uintptr_t>(dest_ptr) & 0xF) == 0);
  REX_ASSERT((reinterpret_cast<uintptr_t>(src_ptr) & 0xF) == 0);

  auto dest = reinterpret_cast<u64*>(dest_ptr);
  auto src = reinterpret_cast<const u64*>(src_ptr);
  simde__m128i shufmask = simde_mm_set_epi8(0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x00,
                                            0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07);

  size_t i;
  for (i = 0; i + 2 <= count; i += 2) {
    simde__m128i input = simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(&src[i]));
    simde__m128i output = simde_mm_shuffle_epi8(input, shufmask);
    simde_mm_store_si128(reinterpret_cast<simde__m128i*>(&dest[i]), output);
  }
  REX_WORKAROUND_CONSTANT_RETURN_IF(count % 2 == 0);
  for (; i < count; ++i) {
    dest[i] = byte_swap(src[i]);
  }
}

void copy_and_swap_64_unaligned(void* dest_ptr, const void* src_ptr, size_t count) {
  auto dest = reinterpret_cast<u64*>(dest_ptr);
  auto src = reinterpret_cast<const u64*>(src_ptr);
  simde__m128i shufmask = simde_mm_set_epi8(0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x00,
                                            0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07);

  size_t i;
  for (i = 0; i + 2 <= count; i += 2) {
    simde__m128i input = simde_mm_loadu_si128(reinterpret_cast<const simde__m128i*>(&src[i]));
    simde__m128i output = simde_mm_shuffle_epi8(input, shufmask);
    simde_mm_storeu_si128(reinterpret_cast<simde__m128i*>(&dest[i]), output);
  }
  REX_WORKAROUND_CONSTANT_RETURN_IF(count % 2 == 0);
  for (; i < count; ++i) {
    dest[i] = byte_swap(src[i]);
  }
}

void copy_and_swap_16_in_32_aligned(void* dest_ptr, const void* src_ptr, size_t count) {
  auto dest = reinterpret_cast<u32*>(dest_ptr);
  auto src = reinterpret_cast<const u32*>(src_ptr);
  size_t i;
  for (i = 0; i + 4 <= count; i += 4) {
    simde__m128i input = simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(&src[i]));
    simde__m128i output =
        simde_mm_or_si128(simde_mm_slli_epi32(input, 16), simde_mm_srli_epi32(input, 16));
    simde_mm_store_si128(reinterpret_cast<simde__m128i*>(&dest[i]), output);
  }
  REX_WORKAROUND_CONSTANT_RETURN_IF(count % 4 == 0);
  for (; i < count; ++i) {
    dest[i] = (src[i] >> 16) | (src[i] << 16);
  }
}

void copy_and_swap_16_in_32_unaligned(void* dest_ptr, const void* src_ptr, size_t count) {
  auto dest = reinterpret_cast<u32*>(dest_ptr);
  auto src = reinterpret_cast<const u32*>(src_ptr);
  size_t i;
  for (i = 0; i + 4 <= count; i += 4) {
    simde__m128i input = simde_mm_loadu_si128(reinterpret_cast<const simde__m128i*>(&src[i]));
    simde__m128i output =
        simde_mm_or_si128(simde_mm_slli_epi32(input, 16), simde_mm_srli_epi32(input, 16));
    simde_mm_storeu_si128(reinterpret_cast<simde__m128i*>(&dest[i]), output);
  }
  REX_WORKAROUND_CONSTANT_RETURN_IF(count % 4 == 0);
  for (; i < count; ++i) {
    dest[i] = (src[i] >> 16) | (src[i] << 16);
  }
}

#elif REX_ARCH_ARM64

// NEON vrev is slower in benchmarks and uint8x16xN_t is no faster, so 1 SIMD register is used.

void copy_and_swap_16_aligned(void* dst_ptr, const void* src_ptr, size_t count) {
  copy_and_swap_16_unaligned(dst_ptr, src_ptr, count);
}

void copy_and_swap_16_unaligned(void* dst_ptr, const void* src_ptr, size_t count) {
  auto dst = reinterpret_cast<u8*>(dst_ptr);
  auto src = reinterpret_cast<const u8*>(src_ptr);

  const uint8x16_t tbl_idx = vcombine_u8(vcreate_u8(UINT64_C(0x0607040502030001)),
                                         vcreate_u8(UINT64_C(0x0E0F0C0D0A0B0809)));

  while (count >= 8) {
    uint8x16_t data = vld1q_u8(src);
    data = vqtbl1q_u8(data, tbl_idx);
    vst1q_u8(dst, data);

    count -= 8;
    dst += 16;
    src += 16;
  }

  while (count > 0) {
    store_and_swap<u16>(dst, load<u16>(src));

    count--;
    dst += 2;
    src += 2;
  }
}

void copy_and_swap_32_aligned(void* dst, const void* src, size_t count) {
  copy_and_swap_32_unaligned(dst, src, count);
}

void copy_and_swap_32_unaligned(void* dst_ptr, const void* src_ptr, size_t count) {
  auto dst = reinterpret_cast<u8*>(dst_ptr);
  auto src = reinterpret_cast<const u8*>(src_ptr);

  const uint8x16_t tbl_idx = vcombine_u8(vcreate_u8(UINT64_C(0x405060700010203)),
                                         vcreate_u8(UINT64_C(0x0C0D0E0F08090A0B)));

  while (count >= 4) {
    uint8x16_t data = vld1q_u8(src);
    data = vqtbl1q_u8(data, tbl_idx);
    vst1q_u8(dst, data);

    count -= 4;
    dst += 16;
    src += 16;
  }

  while (count > 0) {
    store_and_swap<u32>(dst, load<u32>(src));

    count--;
    dst += 4;
    src += 4;
  }
}

void copy_and_swap_64_aligned(void* dst, const void* src, size_t count) {
  copy_and_swap_64_unaligned(dst, src, count);
}

void copy_and_swap_64_unaligned(void* dst_ptr, const void* src_ptr, size_t count) {
  auto dst = reinterpret_cast<u8*>(dst_ptr);
  auto src = reinterpret_cast<const u8*>(src_ptr);

  const uint8x16_t tbl_idx = vcombine_u8(vcreate_u8(UINT64_C(0x0001020304050607)),
                                         vcreate_u8(UINT64_C(0x08090A0B0C0D0E0F)));

  while (count >= 2) {
    uint8x16_t data = vld1q_u8(src);
    data = vqtbl1q_u8(data, tbl_idx);
    vst1q_u8(dst, data);

    count -= 2;
    dst += 16;
    src += 16;
  }

  while (count > 0) {
    store_and_swap<u64>(dst, load<u64>(src));

    count--;
    dst += 8;
    src += 8;
  }
}

void copy_and_swap_16_in_32_aligned(void* dst, const void* src, size_t count) {
  return copy_and_swap_16_in_32_unaligned(dst, src, count);
}

void copy_and_swap_16_in_32_unaligned(void* dst_ptr, const void* src_ptr, size_t count) {
  auto dst = reinterpret_cast<u16*>(dst_ptr);
  auto src = reinterpret_cast<const u16*>(src_ptr);
  while (count > 0) {
    u16 word0 = *src++;
    u16 word1 = *src++;
    *dst++ = word1;
    *dst++ = word0;

    count--;
  }
}

#else

void copy_and_swap_16_aligned(void* dest, const void* src, size_t count) {
  return copy_and_swap_16_unaligned(dest, src, count);
}

void copy_and_swap_16_unaligned(void* dest_ptr, const void* src_ptr, size_t count) {
  auto dest = reinterpret_cast<u16*>(dest_ptr);
  auto src = reinterpret_cast<const u16*>(src_ptr);
  for (size_t i = 0; i < count; ++i) {
    dest[i] = byte_swap(src[i]);
  }
}

void copy_and_swap_32_aligned(void* dest, const void* src, size_t count) {
  return copy_and_swap_32_unaligned(dest, src, count);
}

void copy_and_swap_32_unaligned(void* dest_ptr, const void* src_ptr, size_t count) {
  auto dest = reinterpret_cast<u32*>(dest_ptr);
  auto src = reinterpret_cast<const u32*>(src_ptr);
  for (size_t i = 0; i < count; ++i) {
    dest[i] = byte_swap(src[i]);
  }
}

void copy_and_swap_64_aligned(void* dest, const void* src, size_t count) {
  return copy_and_swap_64_unaligned(dest, src, count);
}

void copy_and_swap_64_unaligned(void* dest_ptr, const void* src_ptr, size_t count) {
  auto dest = reinterpret_cast<u64*>(dest_ptr);
  auto src = reinterpret_cast<const u64*>(src_ptr);
  for (size_t i = 0; i < count; ++i) {
    dest[i] = byte_swap(src[i]);
  }
}

void copy_and_swap_16_in_32_aligned(void* dest, const void* src, size_t count) {
  return copy_and_swap_16_in_32_unaligned(dest, src, count);
}

void copy_and_swap_16_in_32_unaligned(void* dst_ptr, const void* src_ptr, size_t count) {
  auto dst = reinterpret_cast<u16*>(dst_ptr);
  auto src = reinterpret_cast<const u16*>(src_ptr);
  while (count > 0) {
    u16 word0 = *src++;
    u16 word1 = *src++;
    *dst++ = word1;
    *dst++ = word0;

    count--;
  }
}

#endif

}  // namespace memory
}  // namespace rex
