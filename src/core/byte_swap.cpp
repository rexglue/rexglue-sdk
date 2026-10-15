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

#include <cmath>

#include <rex/memory/byte_swap.h>
#include <rex/platform.h>

#include <simde/x86/ssse3.h>

#include <algorithm>

namespace rex {
namespace memory {

void Copy128Aligned(void* dest, const void* src, size_t count) {
  std::memcpy(dest, src, count * 16);
}

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

}  // namespace memory
}  // namespace rex
