/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2023 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <array>
#include <cstdint>
#include <functional>

#include <rex/assert.h>
#include "gpu/xenos/pipeline/shader/dxbc_translator.h"
#include <rex/bit.h>

namespace rex::gpu {
using namespace xenos::ucode;

void DXBCShaderTranslator::ExportToMemory(u8 export_em) {
  if (!export_em) {
    return;
  }

  REX_ASSERT((export_em & ~GetCurrentShader().GetMemExportEMWritten()) == 0);

  a_.OpIf(true, dxbc::Src::R(system_temp_memexport_enabled_and_em_written_, dxbc::Src::kXXXX));

  {
    u32 address_check_temp = PushSystemTemp();
    a_.OpUShR(dxbc::Dest::R(address_check_temp), dxbc::Src::R(system_temp_memexport_address_),
              dxbc::Src::LU(30, 23, 23, 23));
    a_.OpIEq(dxbc::Dest::R(address_check_temp), dxbc::Src::R(address_check_temp),
             dxbc::Src::LU(0x1, 0x96, 0x96, 0x96));
    a_.OpAnd(dxbc::Dest::R(address_check_temp, 0b0011), dxbc::Src::R(address_check_temp),
             dxbc::Src::R(address_check_temp, 0b1110));
    a_.OpAnd(dxbc::Dest::R(address_check_temp, 0b0001),
             dxbc::Src::R(address_check_temp, dxbc::Src::kXXXX),
             dxbc::Src::R(address_check_temp, dxbc::Src::kYYYY));
    a_.OpIf(true, dxbc::Src::R(address_check_temp, dxbc::Src::kXXXX));
    PopSystemTemp();
  }

  u8 em_remaining;
  u32 em_index;

  {
    u32 red_blue_swap_temp = PushSystemTemp();
    a_.OpIBFE(dxbc::Dest::R(red_blue_swap_temp, 0b0001), dxbc::Src::LU(1), dxbc::Src::LU(19),
              dxbc::Src::R(system_temp_memexport_address_, dxbc::Src::kZZZZ));
    a_.OpIf(true, dxbc::Src::R(red_blue_swap_temp, dxbc::Src::kXXXX));
    PopSystemTemp();

    em_remaining = export_em;
    while (rex::bit_scan_forward(em_remaining, &em_index)) {
      em_remaining &= ~(static_cast<u8>(1) << em_index);
      a_.OpMov(dxbc::Dest::R(system_temps_memexport_data_[em_index], 0b0101),
               dxbc::Src::R(system_temps_memexport_data_[em_index], 0b11000110));
    }

    a_.OpEndIf();
  }

  u32 temp = PushSystemTemp();

  a_.OpUBFE(dxbc::Dest::R(temp, 0b0111), dxbc::Src::LU(6, 1, 1, 0), dxbc::Src::LU(8, 16, 17, 0),
            dxbc::Src::R(system_temp_memexport_address_, dxbc::Src::kZZZZ));

  // temp.x must hold log2 of the element size in bytes after switch, or UINT32_MAX if unknown.
  a_.OpSwitch(dxbc::Src::R(temp, dxbc::Src::kXXXX));
  {
    dxbc::Dest element_size_dest(dxbc::Dest::R(temp, 0b0001));
    dxbc::Src num_format_signed(dxbc::Src::R(temp, dxbc::Src::kYYYY));
    dxbc::Src num_format_integer(dxbc::Src::R(temp, dxbc::Src::kZZZZ));

    auto flush_nan = [this, export_em](u32 components) {
      u8 em_remaining = export_em;
      u32 em_index;
      u32 is_nan_temp = PushSystemTemp();
      while (rex::bit_scan_forward(em_remaining, &em_index)) {
        em_remaining &= ~(static_cast<u8>(1) << em_index);
        u32 em = system_temps_memexport_data_[em_index];
        a_.OpNE(dxbc::Dest::R(is_nan_temp, components), dxbc::Src::R(em), dxbc::Src::R(em));
        a_.OpMovC(dxbc::Dest::R(em, components), dxbc::Src::R(is_nan_temp), dxbc::Src::LF(0.0f),
                  dxbc::Src::R(em));
      }
      PopSystemTemp();
    };

    // Widths must have no holes and sum to the texel size (8, 16 or 32 bits), upper bits are junk.
    auto pack_8_16_32 = [&](std::array<u32, 4> widths) {
      u8 em_remaining;
      u32 em_index;

      u32 components = 0;
      std::array<u32, 4> offsets = {};
      for (u32 i = 0; i < 4; ++i) {
        if (widths[i]) {
          // Only formats for which max + 0.5 is exactly representable.
          assert(widths[i] <= 23);
          components |= static_cast<u32>(1) << i;
        }
        if (i) {
          offsets[i] = offsets[i - 1] + widths[i - 1];
        }
      }
      REX_ASSERT((components & 0b1) != 0);

      flush_nan(components);

      a_.OpIf(true, num_format_signed);
      {
        a_.OpIf(true, num_format_integer);
        {
          f32 min_value[4] = {}, max_value[4] = {};
          for (u32 i = 0; i < 4; ++i) {
            if (widths[i]) {
              max_value[i] = static_cast<f32>((static_cast<u32>(1) << (widths[i] - 1)) - 1);
              min_value[i] = -1.0f - max_value[i];
            }
          }
          dxbc::Src min_value_src(dxbc::Src::LP(min_value));
          dxbc::Src max_value_src(dxbc::Src::LP(max_value));
          em_remaining = export_em;
          while (rex::bit_scan_forward(em_remaining, &em_index)) {
            em_remaining &= ~(static_cast<u8>(1) << em_index);
            u32 em = system_temps_memexport_data_[em_index];
            a_.OpMax(dxbc::Dest::R(em, components), min_value_src, dxbc::Src::R(em));
            a_.OpMin(dxbc::Dest::R(em, components), max_value_src, dxbc::Src::R(em));
          }
        }
        a_.OpElse();
        {
          u32 scale_components = 0;
          f32 scale[4] = {};
          for (u32 i = 0; i < 4; ++i) {
            if (widths[i] > 2) {
              scale_components |= static_cast<u32>(1) << i;
              scale[i] = static_cast<f32>((static_cast<u32>(1) << (widths[i] - 1)) - 1);
            }
          }
          dxbc::Src scale_src(dxbc::Src::LP(scale));
          em_remaining = export_em;
          while (rex::bit_scan_forward(em_remaining, &em_index)) {
            em_remaining &= ~(static_cast<u8>(1) << em_index);
            u32 em = system_temps_memexport_data_[em_index];
            a_.OpMax(dxbc::Dest::R(em, components), dxbc::Src::LF(-1.0f), dxbc::Src::R(em));
            a_.OpMin(dxbc::Dest::R(em, components), dxbc::Src::LF(1.0f), dxbc::Src::R(em));
            if (scale_components) {
              a_.OpMul(dxbc::Dest::R(em, scale_components), dxbc::Src::R(em), scale_src);
            }
          }
        }
        a_.OpEndIf();

        u32 round_bias_temp = PushSystemTemp();
        em_remaining = export_em;
        while (rex::bit_scan_forward(em_remaining, &em_index)) {
          em_remaining &= ~(static_cast<u8>(1) << em_index);
          u32 em = system_temps_memexport_data_[em_index];
          a_.OpBFI(dxbc::Dest::R(em, components), dxbc::Src::LU(31), dxbc::Src::LU(0),
                   dxbc::Src::LF(0.5f), dxbc::Src::R(em));
          a_.OpAdd(dxbc::Dest::R(em, components), dxbc::Src::R(em), dxbc::Src::R(round_bias_temp));
          a_.OpFToI(dxbc::Dest::R(em, components), dxbc::Src::R(em));
        }
        PopSystemTemp();
      }
      a_.OpElse();
      {
        a_.OpIf(true, num_format_integer);
        {
          f32 max_value[4];
          for (u32 i = 0; i < 4; ++i) {
            max_value[i] = static_cast<f32>((static_cast<u32>(1) << widths[i]) - 1);
          }
          dxbc::Src max_value_src(dxbc::Src::LP(max_value));
          em_remaining = export_em;
          while (rex::bit_scan_forward(em_remaining, &em_index)) {
            em_remaining &= ~(static_cast<u8>(1) << em_index);
            u32 em = system_temps_memexport_data_[em_index];
            a_.OpMax(dxbc::Dest::R(em, components), dxbc::Src::LF(0.0f), dxbc::Src::R(em));
            a_.OpMin(dxbc::Dest::R(em, components), max_value_src, dxbc::Src::R(em));
          }
        }
        a_.OpElse();
        {
          u32 scale_components = 0;
          f32 scale[4] = {};
          for (u32 i = 0; i < 4; ++i) {
            if (widths[i] > 1) {
              scale_components |= static_cast<u32>(1) << i;
              scale[i] = static_cast<f32>((static_cast<u32>(1) << widths[i]) - 1);
            }
          }
          dxbc::Src scale_src(dxbc::Src::LP(scale));
          em_remaining = export_em;
          while (rex::bit_scan_forward(em_remaining, &em_index)) {
            em_remaining &= ~(static_cast<u8>(1) << em_index);
            u32 em = system_temps_memexport_data_[em_index];
            a_.OpMov(dxbc::Dest::R(em, components), dxbc::Src::R(em), true);
            if (scale_components) {
              a_.OpMul(dxbc::Dest::R(em, scale_components), dxbc::Src::R(em), scale_src);
            }
          }
        }
        a_.OpEndIf();

        em_remaining = export_em;
        while (rex::bit_scan_forward(em_remaining, &em_index)) {
          em_remaining &= ~(static_cast<u8>(1) << em_index);
          u32 em = system_temps_memexport_data_[em_index];
          a_.OpAdd(dxbc::Dest::R(em, components), dxbc::Src::R(em), dxbc::Src::LF(0.5f));
          a_.OpFToU(dxbc::Dest::R(em, components), dxbc::Src::R(em));
        }
      }
      a_.OpEndIf();

      for (u32 i = 0; i < 4; ++i) {
        if (!widths[i]) {
          continue;
        }
        dxbc::Src width_src(dxbc::Src::LU(widths[i]));
        dxbc::Src offset_src(dxbc::Src::LU(offsets[i]));
        em_remaining = export_em;
        while (rex::bit_scan_forward(em_remaining, &em_index)) {
          em_remaining &= ~(static_cast<u8>(1) << em_index);
          u32 em = system_temps_memexport_data_[em_index];
          a_.OpBFI(dxbc::Dest::R(em, 0b0001), width_src, offset_src, dxbc::Src::R(em).Select(i),
                   dxbc::Src::R(em, dxbc::Src::kXXXX));
        }
      }
    };

    a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::ColorFormat::k_8)));
    a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::ColorFormat::k_8_A)));
    a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::ColorFormat::k_8_B)));
    {
      pack_8_16_32({8});
      a_.OpMov(element_size_dest, dxbc::Src::LU(0));
    }
    a_.OpBreak();

    a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::ColorFormat::k_1_5_5_5)));
    {
      pack_8_16_32({5, 5, 5, 1});
      a_.OpMov(element_size_dest, dxbc::Src::LU(1));
    }
    a_.OpBreak();

    a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::ColorFormat::k_5_6_5)));
    {
      pack_8_16_32({5, 6, 5});
      a_.OpMov(element_size_dest, dxbc::Src::LU(1));
    }
    a_.OpBreak();

    a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::ColorFormat::k_6_5_5)));
    {
      pack_8_16_32({5, 5, 6});
      a_.OpMov(element_size_dest, dxbc::Src::LU(1));
    }
    a_.OpBreak();

    a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::ColorFormat::k_8_8_8_8)));
    a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::ColorFormat::k_8_8_8_8_A)));
    a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::ColorFormat::k_8_8_8_8_AS_16_16_16_16)));
    {
      pack_8_16_32({8, 8, 8, 8});
      a_.OpMov(element_size_dest, dxbc::Src::LU(2));
    }
    a_.OpBreak();

    a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::ColorFormat::k_2_10_10_10)));
    a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::ColorFormat::k_2_10_10_10_AS_16_16_16_16)));
    {
      pack_8_16_32({10, 10, 10, 2});
      a_.OpMov(element_size_dest, dxbc::Src::LU(2));
    }
    a_.OpBreak();

    a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::ColorFormat::k_8_8)));
    {
      pack_8_16_32({8, 8});
      a_.OpMov(element_size_dest, dxbc::Src::LU(1));
    }
    a_.OpBreak();

    a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::ColorFormat::k_4_4_4_4)));
    {
      pack_8_16_32({4, 4, 4, 4});
      a_.OpMov(element_size_dest, dxbc::Src::LU(1));
    }
    a_.OpBreak();

    a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::ColorFormat::k_10_11_11)));
    a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::ColorFormat::k_10_11_11_AS_16_16_16_16)));
    {
      pack_8_16_32({11, 11, 10});
      a_.OpMov(element_size_dest, dxbc::Src::LU(2));
    }
    a_.OpBreak();

    a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::ColorFormat::k_11_11_10)));
    a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::ColorFormat::k_11_11_10_AS_16_16_16_16)));
    {
      pack_8_16_32({10, 11, 11});
      a_.OpMov(element_size_dest, dxbc::Src::LU(2));
    }
    a_.OpBreak();

    a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::ColorFormat::k_16)));
    {
      pack_8_16_32({16});
      a_.OpMov(element_size_dest, dxbc::Src::LU(1));
    }
    a_.OpBreak();

    a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::ColorFormat::k_16_16)));
    {
      pack_8_16_32({16, 16});
      a_.OpMov(element_size_dest, dxbc::Src::LU(2));
    }
    a_.OpBreak();

    a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::ColorFormat::k_16_16_16_16)));
    {
      flush_nan(0b1111);

      a_.OpIf(true, num_format_signed);
      {
        a_.OpIf(true, num_format_integer);
        {
          em_remaining = export_em;
          while (rex::bit_scan_forward(em_remaining, &em_index)) {
            em_remaining &= ~(static_cast<u8>(1) << em_index);
            u32 em = system_temps_memexport_data_[em_index];
            a_.OpMax(dxbc::Dest::R(em), dxbc::Src::LF(static_cast<f32>(INT16_MIN)),
                     dxbc::Src::R(em));
            a_.OpMin(dxbc::Dest::R(em), dxbc::Src::LF(static_cast<f32>(INT16_MAX)),
                     dxbc::Src::R(em));
          }
        }
        a_.OpElse();
        {
          em_remaining = export_em;
          while (rex::bit_scan_forward(em_remaining, &em_index)) {
            em_remaining &= ~(static_cast<u8>(1) << em_index);
            u32 em = system_temps_memexport_data_[em_index];
            a_.OpMax(dxbc::Dest::R(em), dxbc::Src::LF(-1.0f), dxbc::Src::R(em));
            a_.OpMin(dxbc::Dest::R(em), dxbc::Src::LF(1.0f), dxbc::Src::R(em));
            a_.OpMul(dxbc::Dest::R(em), dxbc::Src::R(em),
                     dxbc::Src::LF(static_cast<f32>(INT16_MAX)));
          }
        }
        a_.OpEndIf();

        u32 round_bias_temp = PushSystemTemp();
        em_remaining = export_em;
        while (rex::bit_scan_forward(em_remaining, &em_index)) {
          em_remaining &= ~(static_cast<u8>(1) << em_index);
          u32 em = system_temps_memexport_data_[em_index];
          a_.OpBFI(dxbc::Dest::R(em), dxbc::Src::LU(31), dxbc::Src::LU(0), dxbc::Src::LF(0.5f),
                   dxbc::Src::R(em));
          a_.OpAdd(dxbc::Dest::R(em), dxbc::Src::R(em), dxbc::Src::R(round_bias_temp));
          a_.OpFToI(dxbc::Dest::R(em), dxbc::Src::R(em));
        }
        PopSystemTemp();
      }
      a_.OpElse();
      {
        a_.OpIf(true, num_format_integer);
        {
          em_remaining = export_em;
          while (rex::bit_scan_forward(em_remaining, &em_index)) {
            em_remaining &= ~(static_cast<u8>(1) << em_index);
            u32 em = system_temps_memexport_data_[em_index];
            a_.OpMax(dxbc::Dest::R(em), dxbc::Src::LF(0.0f), dxbc::Src::R(em));
            a_.OpMin(dxbc::Dest::R(em), dxbc::Src::LF(static_cast<f32>(UINT16_MAX)),
                     dxbc::Src::R(em));
          }
        }
        a_.OpElse();
        {
          em_remaining = export_em;
          while (rex::bit_scan_forward(em_remaining, &em_index)) {
            em_remaining &= ~(static_cast<u8>(1) << em_index);
            u32 em = system_temps_memexport_data_[em_index];
            a_.OpMov(dxbc::Dest::R(em), dxbc::Src::R(em), true);
            a_.OpMul(dxbc::Dest::R(em), dxbc::Src::R(em),
                     dxbc::Src::LF(static_cast<f32>(UINT16_MAX)));
          }
        }
        a_.OpEndIf();

        em_remaining = export_em;
        while (rex::bit_scan_forward(em_remaining, &em_index)) {
          em_remaining &= ~(static_cast<u8>(1) << em_index);
          u32 em = system_temps_memexport_data_[em_index];
          a_.OpAdd(dxbc::Dest::R(em), dxbc::Src::R(em), dxbc::Src::LF(0.5f));
          a_.OpFToU(dxbc::Dest::R(em), dxbc::Src::R(em));
        }
      }
      a_.OpEndIf();

      em_remaining = export_em;
      while (rex::bit_scan_forward(em_remaining, &em_index)) {
        em_remaining &= ~(static_cast<u8>(1) << em_index);
        u32 em = system_temps_memexport_data_[em_index];
        a_.OpBFI(dxbc::Dest::R(em, 0b0011), dxbc::Src::LU(16), dxbc::Src::LU(16),
                 dxbc::Src::R(em, 0b1101), dxbc::Src::R(em, 0b1000));
      }

      a_.OpMov(element_size_dest, dxbc::Src::LU(3));
    }
    a_.OpBreak();

    a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::ColorFormat::k_16_FLOAT)));
    {
      em_remaining = export_em;
      while (rex::bit_scan_forward(em_remaining, &em_index)) {
        em_remaining &= ~(static_cast<u8>(1) << em_index);
        u32 em = system_temps_memexport_data_[em_index];
        a_.OpF32ToF16(dxbc::Dest::R(em, 0b0001), dxbc::Src::R(em, dxbc::Src::kXXXX));
      }
      a_.OpMov(element_size_dest, dxbc::Src::LU(1));
    }
    a_.OpBreak();

    a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::ColorFormat::k_16_16_FLOAT)));
    {
      em_remaining = export_em;
      while (rex::bit_scan_forward(em_remaining, &em_index)) {
        em_remaining &= ~(static_cast<u8>(1) << em_index);
        u32 em = system_temps_memexport_data_[em_index];
        a_.OpF32ToF16(dxbc::Dest::R(em, 0b0011), dxbc::Src::R(em));
        a_.OpBFI(dxbc::Dest::R(em, 0b0001), dxbc::Src::LU(16), dxbc::Src::LU(16),
                 dxbc::Src::R(em, dxbc::Src::kYYYY), dxbc::Src::R(em, dxbc::Src::kXXXX));
      }
      a_.OpMov(element_size_dest, dxbc::Src::LU(2));
    }
    a_.OpBreak();

    a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::ColorFormat::k_16_16_16_16_FLOAT)));
    {
      em_remaining = export_em;
      while (rex::bit_scan_forward(em_remaining, &em_index)) {
        em_remaining &= ~(static_cast<u8>(1) << em_index);
        u32 em = system_temps_memexport_data_[em_index];
        a_.OpF32ToF16(dxbc::Dest::R(em), dxbc::Src::R(em));
        a_.OpBFI(dxbc::Dest::R(em, 0b0011), dxbc::Src::LU(16), dxbc::Src::LU(16),
                 dxbc::Src::R(em, 0b1101), dxbc::Src::R(em, 0b1000));
      }
      a_.OpMov(element_size_dest, dxbc::Src::LU(3));
    }
    a_.OpBreak();

    a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::ColorFormat::k_32_FLOAT)));
    { a_.OpMov(element_size_dest, dxbc::Src::LU(2)); }
    a_.OpBreak();

    a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::ColorFormat::k_32_32_FLOAT)));
    { a_.OpMov(element_size_dest, dxbc::Src::LU(3)); }
    a_.OpBreak();

    a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::ColorFormat::k_32_32_32_32_FLOAT)));
    { a_.OpMov(element_size_dest, dxbc::Src::LU(4)); }
    a_.OpBreak();

    a_.OpDefault();
    a_.OpMov(element_size_dest, dxbc::Src::LU(UINT32_MAX));
    a_.OpBreak();
  }
  a_.OpEndSwitch();

  dxbc::Src element_size_src(dxbc::Src::R(temp, dxbc::Src::kXXXX));

  {
    dxbc::Dest endian_dest(dxbc::Dest::R(temp, 0b0010));
    dxbc::Src endian_src(dxbc::Src::R(temp, dxbc::Src::kYYYY));
    a_.OpUBFE(endian_dest, dxbc::Src::LU(3), dxbc::Src::LU(0),
              dxbc::Src::R(system_temp_memexport_address_, dxbc::Src::kZZZZ));

    for (u32 i = 0; i < 2; ++i) {
      a_.OpIEq(dxbc::Dest::R(temp, 0b0100), endian_src,
               dxbc::Src::LU(
                   static_cast<u32>(i ? xenos::Endian128::k8in128 : xenos::Endian128::k8in64)));
      em_remaining = export_em;
      while (rex::bit_scan_forward(em_remaining, &em_index)) {
        em_remaining &= ~(static_cast<u8>(1) << em_index);
        u32 em = system_temps_memexport_data_[em_index];
        a_.OpMovC(dxbc::Dest::R(em), dxbc::Src::R(temp, dxbc::Src::kZZZZ),
                  dxbc::Src::R(em, i ? 0b00011011 : 0b10110001), dxbc::Src::R(em));
      }
      a_.OpMovC(endian_dest, dxbc::Src::R(temp, dxbc::Src::kZZZZ),
                dxbc::Src::LU(std::to_underlying(xenos::Endian128::k8in32)), endian_src);
    }

    u32 swap_temp = PushSystemTemp();
    dxbc::Dest swap_temp_dest(dxbc::Dest::R(swap_temp));
    dxbc::Src swap_temp_src(dxbc::Src::R(swap_temp));

    a_.OpSwitch(endian_src);
    a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::Endian128::k8in16)));
    a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::Endian128::k8in32)));
    em_remaining = export_em;
    while (rex::bit_scan_forward(em_remaining, &em_index)) {
      em_remaining &= ~(static_cast<u8>(1) << em_index);
      u32 em = system_temps_memexport_data_[em_index];
      dxbc::Dest em_dest(dxbc::Dest::R(em));
      dxbc::Src em_src(dxbc::Src::R(em));
      a_.OpAnd(swap_temp_dest, em_src, dxbc::Src::LU(0x00FF00FF));
      a_.OpUShR(em_dest, em_src, dxbc::Src::LU(8));
      a_.OpAnd(em_dest, em_src, dxbc::Src::LU(0x00FF00FF));
      a_.OpUMAd(em_dest, swap_temp_src, dxbc::Src::LU(256), em_src);
    }
    a_.OpBreak();
    a_.OpEndSwitch();

    a_.OpSwitch(endian_src);
    a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::Endian128::k8in32)));
    a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::Endian128::k16in32)));
    em_remaining = export_em;
    while (rex::bit_scan_forward(em_remaining, &em_index)) {
      em_remaining &= ~(static_cast<u8>(1) << em_index);
      u32 em = system_temps_memexport_data_[em_index];
      dxbc::Dest em_dest(dxbc::Dest::R(em));
      dxbc::Src em_src(dxbc::Src::R(em));
      a_.OpUShR(swap_temp_dest, em_src, dxbc::Src::LU(16));
      a_.OpBFI(em_dest, dxbc::Src::LU(16), dxbc::Src::LU(16), em_src, swap_temp_src);
    }
    a_.OpBreak();
    a_.OpEndSwitch();

    PopSystemTemp();
  }

  a_.OpUBFE(dxbc::Dest::R(temp, 0b0110), dxbc::Src::LU(23), dxbc::Src::LU(0),
            dxbc::Src::R(system_temp_memexport_address_, 0b1101 << 2));
  dxbc::Dest em0_address_dest(dxbc::Dest::R(temp, 0b0010));
  dxbc::Src em0_address_src(dxbc::Src::R(temp, dxbc::Src::kYYYY));
  dxbc::Src index_count_src(dxbc::Src::R(temp, dxbc::Src::kZZZZ));

  a_.OpILT(dxbc::Dest::R(temp, 0b1000), em0_address_src, index_count_src);
  a_.OpIf(true, dxbc::Src::R(temp, dxbc::Src::kWWWW));

  a_.OpIShL(dxbc::Dest::R(temp, 0b1000),
            dxbc::Src::R(system_temp_memexport_address_, dxbc::Src::kXXXX), dxbc::Src::LU(2));
  dxbc::Src base_address_src(dxbc::Src::R(temp, dxbc::Src::kWWWW));

  u8 export_em14 = export_em >> 1;
  REX_ASSERT((export_em14 >> 4) == 0);
  u32 em14_address_temp = UINT32_MAX, store_em14_temp = UINT32_MAX;
  if (export_em14) {
    em14_address_temp = PushSystemTemp();
    dxbc::Dest em14_address_dest(dxbc::Dest::R(em14_address_temp, export_em14));
    dxbc::Src em14_address_src(dxbc::Src::R(em14_address_temp));
    store_em14_temp = PushSystemTemp();
    dxbc::Dest store_em14_dest(dxbc::Dest::R(store_em14_temp, export_em14));
    dxbc::Src store_em14_src(dxbc::Src::R(store_em14_temp));
    a_.OpIAdd(em14_address_dest, em0_address_src, dxbc::Src::LU(1, 2, 3, 4));
    a_.OpILT(store_em14_dest, em14_address_src, index_count_src);
    u32 em14_written_temp = PushSystemTemp();
    a_.OpIBFE(dxbc::Dest::R(em14_written_temp, export_em14), dxbc::Src::LU(1),
              dxbc::Src::LU(1, 2, 3, 4),
              dxbc::Src::R(system_temp_memexport_enabled_and_em_written_, dxbc::Src::kYYYY));
    a_.OpAnd(store_em14_dest, store_em14_src, dxbc::Src::R(em14_written_temp));
    PopSystemTemp();
    a_.OpIShL(em14_address_dest, em14_address_src, element_size_src);
    a_.OpIAdd(em14_address_dest, base_address_src, em14_address_src);
  }
  if (export_em & 0b1) {
    a_.OpIShL(em0_address_dest, em0_address_src, element_size_src);
    a_.OpIAdd(em0_address_dest, base_address_src, em0_address_src);
    a_.OpIBFE(dxbc::Dest::R(temp, 0b0100), dxbc::Src::LU(1), dxbc::Src::LU(0),
              dxbc::Src::R(system_temp_memexport_enabled_and_em_written_, dxbc::Src::kYYYY));
  }
  dxbc::Src em0_written_src(dxbc::Src::R(temp, dxbc::Src::kZZZZ));

  // An unknown format (UINT32_MAX size) enters no case, so nothing is written.
  if (uav_index_shared_memory_ == kBindingIndexUnallocated) {
    uav_index_shared_memory_ = uav_count_++;
  }
  u8 em14_remaining;
  u32 em14_index;
  a_.OpSwitch(element_size_src);

  dxbc::Dest atomic_dest(
      dxbc::Dest::U(uav_index_shared_memory_, std::to_underlying(UAVRegister::kSharedMemory), 0));
  for (u32 i = 0; i <= 1; ++i) {
    a_.OpCase(dxbc::Src::LU(i));
    dxbc::Src width_src(dxbc::Src::LU(8 << i));
    u32 sub_dword_temp = PushSystemTemp();
    if (export_em & 0b1) {
      a_.OpIf(true, em0_written_src);
      a_.OpBFI(dxbc::Dest::R(sub_dword_temp, 0b0001), dxbc::Src::LU(2), dxbc::Src::LU(3),
               em0_address_src, dxbc::Src::LU(0));
      a_.OpAnd(em0_address_dest, em0_address_src, dxbc::Src::LU(~static_cast<u32>(3)));
      a_.OpBFI(dxbc::Dest::R(sub_dword_temp, 0b0010), width_src,
               dxbc::Src::R(sub_dword_temp, dxbc::Src::kXXXX), dxbc::Src::LU(0),
               dxbc::Src::LU(UINT32_MAX));
      a_.OpAtomicAnd(atomic_dest, em0_address_src, 0b0001,
                     dxbc::Src::R(sub_dword_temp, dxbc::Src::kYYYY));
      a_.OpBFI(dxbc::Dest::R(sub_dword_temp, 0b0010), width_src,
               dxbc::Src::R(sub_dword_temp, dxbc::Src::kXXXX),
               dxbc::Src::R(system_temps_memexport_data_[0], dxbc::Src::kXXXX), dxbc::Src::LU(0));
      a_.OpAtomicOr(atomic_dest, em0_address_src, 0b0001,
                    dxbc::Src::R(sub_dword_temp, dxbc::Src::kYYYY));
      a_.OpEndIf();
    }
    if (export_em14) {
      a_.OpBFI(dxbc::Dest::R(sub_dword_temp, export_em14), dxbc::Src::LU(2), dxbc::Src::LU(3),
               dxbc::Src::R(em14_address_temp), dxbc::Src::LU(0));
      a_.OpAnd(dxbc::Dest::R(em14_address_temp, export_em14), dxbc::Src::R(em14_address_temp),
               dxbc::Src::LU(~static_cast<u32>(3)));
      u32 sub_dword_data_temp = PushSystemTemp();
      em14_remaining = export_em14;
      while (rex::bit_scan_forward(em14_remaining, &em14_index)) {
        em14_remaining &= ~(static_cast<u8>(1) << em14_index);
        a_.OpIf(true, dxbc::Src::R(store_em14_temp).Select(em14_index));
        a_.OpBFI(dxbc::Dest::R(sub_dword_data_temp, 0b0001), width_src,
                 dxbc::Src::R(sub_dword_temp).Select(em14_index), dxbc::Src::LU(0),
                 dxbc::Src::LU(UINT32_MAX));
        a_.OpAtomicAnd(atomic_dest, dxbc::Src::R(em14_address_temp).Select(em14_index), 0b0001,
                       dxbc::Src::R(sub_dword_data_temp, dxbc::Src::kXXXX));
        a_.OpBFI(dxbc::Dest::R(sub_dword_data_temp, 0b0001), width_src,
                 dxbc::Src::R(sub_dword_temp).Select(em14_index),
                 dxbc::Src::R(system_temps_memexport_data_[1 + em14_index], dxbc::Src::kXXXX),
                 dxbc::Src::LU(0));
        a_.OpAtomicOr(atomic_dest, dxbc::Src::R(em14_address_temp).Select(em14_index), 0b0001,
                      dxbc::Src::R(sub_dword_data_temp, dxbc::Src::kXXXX));
        a_.OpEndIf();
      }
      PopSystemTemp();
    }
    PopSystemTemp();
    a_.OpBreak();
  }

  for (u32 i = 2; i <= 4; ++i) {
    a_.OpCase(dxbc::Src::LU(i));
    u32 store_mask = (static_cast<u32>(1) << (static_cast<u32>(1) << (i - 2))) - 1;
    dxbc::Dest store_dest(dxbc::Dest::U(
        uav_index_shared_memory_, std::to_underlying(UAVRegister::kSharedMemory), store_mask));
    if (export_em & 0b1) {
      a_.OpIf(true, em0_written_src);
      a_.OpStoreRaw(store_dest, em0_address_src, dxbc::Src::R(system_temps_memexport_data_[0]));
      a_.OpEndIf();
    }
    em14_remaining = export_em14;
    while (rex::bit_scan_forward(em14_remaining, &em14_index)) {
      em14_remaining &= ~(static_cast<u8>(1) << em14_index);
      a_.OpIf(true, dxbc::Src::R(store_em14_temp).Select(em14_index));
      a_.OpStoreRaw(store_dest, dxbc::Src::R(em14_address_temp).Select(em14_index),
                    dxbc::Src::R(system_temps_memexport_data_[1 + em14_index]));
      a_.OpEndIf();
    }
    a_.OpBreak();
  }

  a_.OpEndSwitch();

  if (export_em14) {
    PopSystemTemp(2);
  }

  a_.OpEndIf();

  PopSystemTemp();

  a_.OpEndIf();

  a_.OpEndIf();
}

}  // namespace rex::gpu
