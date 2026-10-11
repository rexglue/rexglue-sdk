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
#include <rex/math.h>

namespace rex::gpu {
using namespace xenos::ucode;

void DXBCShaderTranslator::ExportToMemory(u8 export_eM) {
  if (!export_eM) {
    return;
  }

  REX_ASSERT((export_eM & ~current_shader().memexport_eM_written()) == 0);

  a_.OpIf(true, dxbc::Src::R(system_temp_memexport_enabled_and_eM_written_, dxbc::Src::kXXXX));

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

  u8 eM_remaining;
  u32 eM_index;

  {
    u32 red_blue_swap_temp = PushSystemTemp();
    a_.OpIBFE(dxbc::Dest::R(red_blue_swap_temp, 0b0001), dxbc::Src::LU(1), dxbc::Src::LU(19),
              dxbc::Src::R(system_temp_memexport_address_, dxbc::Src::kZZZZ));
    a_.OpIf(true, dxbc::Src::R(red_blue_swap_temp, dxbc::Src::kXXXX));
    PopSystemTemp();

    eM_remaining = export_eM;
    while (rex::bit_scan_forward(eM_remaining, &eM_index)) {
      eM_remaining &= ~(static_cast<u8>(1) << eM_index);
      a_.OpMov(dxbc::Dest::R(system_temps_memexport_data_[eM_index], 0b0101),
               dxbc::Src::R(system_temps_memexport_data_[eM_index], 0b11000110));
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

    auto flush_nan = [this, export_eM](u32 components) {
      u8 eM_remaining = export_eM;
      u32 eM_index;
      u32 is_nan_temp = PushSystemTemp();
      while (rex::bit_scan_forward(eM_remaining, &eM_index)) {
        eM_remaining &= ~(static_cast<u8>(1) << eM_index);
        u32 eM = system_temps_memexport_data_[eM_index];
        a_.OpNE(dxbc::Dest::R(is_nan_temp, components), dxbc::Src::R(eM), dxbc::Src::R(eM));
        a_.OpMovC(dxbc::Dest::R(eM, components), dxbc::Src::R(is_nan_temp), dxbc::Src::LF(0.0f),
                  dxbc::Src::R(eM));
      }
      PopSystemTemp();
    };

    // Widths must have no holes and sum to the texel size (8, 16 or 32 bits), upper bits are junk.
    auto pack_8_16_32 = [&](std::array<u32, 4> widths) {
      u8 eM_remaining;
      u32 eM_index;

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
          eM_remaining = export_eM;
          while (rex::bit_scan_forward(eM_remaining, &eM_index)) {
            eM_remaining &= ~(static_cast<u8>(1) << eM_index);
            u32 eM = system_temps_memexport_data_[eM_index];
            a_.OpMax(dxbc::Dest::R(eM, components), min_value_src, dxbc::Src::R(eM));
            a_.OpMin(dxbc::Dest::R(eM, components), max_value_src, dxbc::Src::R(eM));
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
          eM_remaining = export_eM;
          while (rex::bit_scan_forward(eM_remaining, &eM_index)) {
            eM_remaining &= ~(static_cast<u8>(1) << eM_index);
            u32 eM = system_temps_memexport_data_[eM_index];
            a_.OpMax(dxbc::Dest::R(eM, components), dxbc::Src::LF(-1.0f), dxbc::Src::R(eM));
            a_.OpMin(dxbc::Dest::R(eM, components), dxbc::Src::LF(1.0f), dxbc::Src::R(eM));
            if (scale_components) {
              a_.OpMul(dxbc::Dest::R(eM, scale_components), dxbc::Src::R(eM), scale_src);
            }
          }
        }
        a_.OpEndIf();

        u32 round_bias_temp = PushSystemTemp();
        eM_remaining = export_eM;
        while (rex::bit_scan_forward(eM_remaining, &eM_index)) {
          eM_remaining &= ~(static_cast<u8>(1) << eM_index);
          u32 eM = system_temps_memexport_data_[eM_index];
          a_.OpBFI(dxbc::Dest::R(eM, components), dxbc::Src::LU(31), dxbc::Src::LU(0),
                   dxbc::Src::LF(0.5f), dxbc::Src::R(eM));
          a_.OpAdd(dxbc::Dest::R(eM, components), dxbc::Src::R(eM), dxbc::Src::R(round_bias_temp));
          a_.OpFToI(dxbc::Dest::R(eM, components), dxbc::Src::R(eM));
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
          eM_remaining = export_eM;
          while (rex::bit_scan_forward(eM_remaining, &eM_index)) {
            eM_remaining &= ~(static_cast<u8>(1) << eM_index);
            u32 eM = system_temps_memexport_data_[eM_index];
            a_.OpMax(dxbc::Dest::R(eM, components), dxbc::Src::LF(0.0f), dxbc::Src::R(eM));
            a_.OpMin(dxbc::Dest::R(eM, components), max_value_src, dxbc::Src::R(eM));
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
          eM_remaining = export_eM;
          while (rex::bit_scan_forward(eM_remaining, &eM_index)) {
            eM_remaining &= ~(static_cast<u8>(1) << eM_index);
            u32 eM = system_temps_memexport_data_[eM_index];
            a_.OpMov(dxbc::Dest::R(eM, components), dxbc::Src::R(eM), true);
            if (scale_components) {
              a_.OpMul(dxbc::Dest::R(eM, scale_components), dxbc::Src::R(eM), scale_src);
            }
          }
        }
        a_.OpEndIf();

        eM_remaining = export_eM;
        while (rex::bit_scan_forward(eM_remaining, &eM_index)) {
          eM_remaining &= ~(static_cast<u8>(1) << eM_index);
          u32 eM = system_temps_memexport_data_[eM_index];
          a_.OpAdd(dxbc::Dest::R(eM, components), dxbc::Src::R(eM), dxbc::Src::LF(0.5f));
          a_.OpFToU(dxbc::Dest::R(eM, components), dxbc::Src::R(eM));
        }
      }
      a_.OpEndIf();

      for (u32 i = 0; i < 4; ++i) {
        if (!widths[i]) {
          continue;
        }
        dxbc::Src width_src(dxbc::Src::LU(widths[i]));
        dxbc::Src offset_src(dxbc::Src::LU(offsets[i]));
        eM_remaining = export_eM;
        while (rex::bit_scan_forward(eM_remaining, &eM_index)) {
          eM_remaining &= ~(static_cast<u8>(1) << eM_index);
          u32 eM = system_temps_memexport_data_[eM_index];
          a_.OpBFI(dxbc::Dest::R(eM, 0b0001), width_src, offset_src, dxbc::Src::R(eM).Select(i),
                   dxbc::Src::R(eM, dxbc::Src::kXXXX));
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
          eM_remaining = export_eM;
          while (rex::bit_scan_forward(eM_remaining, &eM_index)) {
            eM_remaining &= ~(static_cast<u8>(1) << eM_index);
            u32 eM = system_temps_memexport_data_[eM_index];
            a_.OpMax(dxbc::Dest::R(eM), dxbc::Src::LF(static_cast<f32>(INT16_MIN)),
                     dxbc::Src::R(eM));
            a_.OpMin(dxbc::Dest::R(eM), dxbc::Src::LF(static_cast<f32>(INT16_MAX)),
                     dxbc::Src::R(eM));
          }
        }
        a_.OpElse();
        {
          eM_remaining = export_eM;
          while (rex::bit_scan_forward(eM_remaining, &eM_index)) {
            eM_remaining &= ~(static_cast<u8>(1) << eM_index);
            u32 eM = system_temps_memexport_data_[eM_index];
            a_.OpMax(dxbc::Dest::R(eM), dxbc::Src::LF(-1.0f), dxbc::Src::R(eM));
            a_.OpMin(dxbc::Dest::R(eM), dxbc::Src::LF(1.0f), dxbc::Src::R(eM));
            a_.OpMul(dxbc::Dest::R(eM), dxbc::Src::R(eM),
                     dxbc::Src::LF(static_cast<f32>(INT16_MAX)));
          }
        }
        a_.OpEndIf();

        u32 round_bias_temp = PushSystemTemp();
        eM_remaining = export_eM;
        while (rex::bit_scan_forward(eM_remaining, &eM_index)) {
          eM_remaining &= ~(static_cast<u8>(1) << eM_index);
          u32 eM = system_temps_memexport_data_[eM_index];
          a_.OpBFI(dxbc::Dest::R(eM), dxbc::Src::LU(31), dxbc::Src::LU(0), dxbc::Src::LF(0.5f),
                   dxbc::Src::R(eM));
          a_.OpAdd(dxbc::Dest::R(eM), dxbc::Src::R(eM), dxbc::Src::R(round_bias_temp));
          a_.OpFToI(dxbc::Dest::R(eM), dxbc::Src::R(eM));
        }
        PopSystemTemp();
      }
      a_.OpElse();
      {
        a_.OpIf(true, num_format_integer);
        {
          eM_remaining = export_eM;
          while (rex::bit_scan_forward(eM_remaining, &eM_index)) {
            eM_remaining &= ~(static_cast<u8>(1) << eM_index);
            u32 eM = system_temps_memexport_data_[eM_index];
            a_.OpMax(dxbc::Dest::R(eM), dxbc::Src::LF(0.0f), dxbc::Src::R(eM));
            a_.OpMin(dxbc::Dest::R(eM), dxbc::Src::LF(static_cast<f32>(UINT16_MAX)),
                     dxbc::Src::R(eM));
          }
        }
        a_.OpElse();
        {
          eM_remaining = export_eM;
          while (rex::bit_scan_forward(eM_remaining, &eM_index)) {
            eM_remaining &= ~(static_cast<u8>(1) << eM_index);
            u32 eM = system_temps_memexport_data_[eM_index];
            a_.OpMov(dxbc::Dest::R(eM), dxbc::Src::R(eM), true);
            a_.OpMul(dxbc::Dest::R(eM), dxbc::Src::R(eM),
                     dxbc::Src::LF(static_cast<f32>(UINT16_MAX)));
          }
        }
        a_.OpEndIf();

        eM_remaining = export_eM;
        while (rex::bit_scan_forward(eM_remaining, &eM_index)) {
          eM_remaining &= ~(static_cast<u8>(1) << eM_index);
          u32 eM = system_temps_memexport_data_[eM_index];
          a_.OpAdd(dxbc::Dest::R(eM), dxbc::Src::R(eM), dxbc::Src::LF(0.5f));
          a_.OpFToU(dxbc::Dest::R(eM), dxbc::Src::R(eM));
        }
      }
      a_.OpEndIf();

      eM_remaining = export_eM;
      while (rex::bit_scan_forward(eM_remaining, &eM_index)) {
        eM_remaining &= ~(static_cast<u8>(1) << eM_index);
        u32 eM = system_temps_memexport_data_[eM_index];
        a_.OpBFI(dxbc::Dest::R(eM, 0b0011), dxbc::Src::LU(16), dxbc::Src::LU(16),
                 dxbc::Src::R(eM, 0b1101), dxbc::Src::R(eM, 0b1000));
      }

      a_.OpMov(element_size_dest, dxbc::Src::LU(3));
    }
    a_.OpBreak();

    a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::ColorFormat::k_16_FLOAT)));
    {
      eM_remaining = export_eM;
      while (rex::bit_scan_forward(eM_remaining, &eM_index)) {
        eM_remaining &= ~(static_cast<u8>(1) << eM_index);
        u32 eM = system_temps_memexport_data_[eM_index];
        a_.OpF32ToF16(dxbc::Dest::R(eM, 0b0001), dxbc::Src::R(eM, dxbc::Src::kXXXX));
      }
      a_.OpMov(element_size_dest, dxbc::Src::LU(1));
    }
    a_.OpBreak();

    a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::ColorFormat::k_16_16_FLOAT)));
    {
      eM_remaining = export_eM;
      while (rex::bit_scan_forward(eM_remaining, &eM_index)) {
        eM_remaining &= ~(static_cast<u8>(1) << eM_index);
        u32 eM = system_temps_memexport_data_[eM_index];
        a_.OpF32ToF16(dxbc::Dest::R(eM, 0b0011), dxbc::Src::R(eM));
        a_.OpBFI(dxbc::Dest::R(eM, 0b0001), dxbc::Src::LU(16), dxbc::Src::LU(16),
                 dxbc::Src::R(eM, dxbc::Src::kYYYY), dxbc::Src::R(eM, dxbc::Src::kXXXX));
      }
      a_.OpMov(element_size_dest, dxbc::Src::LU(2));
    }
    a_.OpBreak();

    a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::ColorFormat::k_16_16_16_16_FLOAT)));
    {
      eM_remaining = export_eM;
      while (rex::bit_scan_forward(eM_remaining, &eM_index)) {
        eM_remaining &= ~(static_cast<u8>(1) << eM_index);
        u32 eM = system_temps_memexport_data_[eM_index];
        a_.OpF32ToF16(dxbc::Dest::R(eM), dxbc::Src::R(eM));
        a_.OpBFI(dxbc::Dest::R(eM, 0b0011), dxbc::Src::LU(16), dxbc::Src::LU(16),
                 dxbc::Src::R(eM, 0b1101), dxbc::Src::R(eM, 0b1000));
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
      eM_remaining = export_eM;
      while (rex::bit_scan_forward(eM_remaining, &eM_index)) {
        eM_remaining &= ~(static_cast<u8>(1) << eM_index);
        u32 eM = system_temps_memexport_data_[eM_index];
        a_.OpMovC(dxbc::Dest::R(eM), dxbc::Src::R(temp, dxbc::Src::kZZZZ),
                  dxbc::Src::R(eM, i ? 0b00011011 : 0b10110001), dxbc::Src::R(eM));
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
    eM_remaining = export_eM;
    while (rex::bit_scan_forward(eM_remaining, &eM_index)) {
      eM_remaining &= ~(static_cast<u8>(1) << eM_index);
      u32 eM = system_temps_memexport_data_[eM_index];
      dxbc::Dest eM_dest(dxbc::Dest::R(eM));
      dxbc::Src eM_src(dxbc::Src::R(eM));
      a_.OpAnd(swap_temp_dest, eM_src, dxbc::Src::LU(0x00FF00FF));
      a_.OpUShR(eM_dest, eM_src, dxbc::Src::LU(8));
      a_.OpAnd(eM_dest, eM_src, dxbc::Src::LU(0x00FF00FF));
      a_.OpUMAd(eM_dest, swap_temp_src, dxbc::Src::LU(256), eM_src);
    }
    a_.OpBreak();
    a_.OpEndSwitch();

    a_.OpSwitch(endian_src);
    a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::Endian128::k8in32)));
    a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::Endian128::k16in32)));
    eM_remaining = export_eM;
    while (rex::bit_scan_forward(eM_remaining, &eM_index)) {
      eM_remaining &= ~(static_cast<u8>(1) << eM_index);
      u32 eM = system_temps_memexport_data_[eM_index];
      dxbc::Dest eM_dest(dxbc::Dest::R(eM));
      dxbc::Src eM_src(dxbc::Src::R(eM));
      a_.OpUShR(swap_temp_dest, eM_src, dxbc::Src::LU(16));
      a_.OpBFI(eM_dest, dxbc::Src::LU(16), dxbc::Src::LU(16), eM_src, swap_temp_src);
    }
    a_.OpBreak();
    a_.OpEndSwitch();

    PopSystemTemp();
  }

  a_.OpUBFE(dxbc::Dest::R(temp, 0b0110), dxbc::Src::LU(23), dxbc::Src::LU(0),
            dxbc::Src::R(system_temp_memexport_address_, 0b1101 << 2));
  dxbc::Dest eM0_address_dest(dxbc::Dest::R(temp, 0b0010));
  dxbc::Src eM0_address_src(dxbc::Src::R(temp, dxbc::Src::kYYYY));
  dxbc::Src index_count_src(dxbc::Src::R(temp, dxbc::Src::kZZZZ));

  a_.OpILT(dxbc::Dest::R(temp, 0b1000), eM0_address_src, index_count_src);
  a_.OpIf(true, dxbc::Src::R(temp, dxbc::Src::kWWWW));

  a_.OpIShL(dxbc::Dest::R(temp, 0b1000),
            dxbc::Src::R(system_temp_memexport_address_, dxbc::Src::kXXXX), dxbc::Src::LU(2));
  dxbc::Src base_address_src(dxbc::Src::R(temp, dxbc::Src::kWWWW));

  u8 export_eM14 = export_eM >> 1;
  REX_ASSERT((export_eM14 >> 4) == 0);
  u32 eM14_address_temp = UINT32_MAX, store_eM14_temp = UINT32_MAX;
  if (export_eM14) {
    eM14_address_temp = PushSystemTemp();
    dxbc::Dest eM14_address_dest(dxbc::Dest::R(eM14_address_temp, export_eM14));
    dxbc::Src eM14_address_src(dxbc::Src::R(eM14_address_temp));
    store_eM14_temp = PushSystemTemp();
    dxbc::Dest store_eM14_dest(dxbc::Dest::R(store_eM14_temp, export_eM14));
    dxbc::Src store_eM14_src(dxbc::Src::R(store_eM14_temp));
    a_.OpIAdd(eM14_address_dest, eM0_address_src, dxbc::Src::LU(1, 2, 3, 4));
    a_.OpILT(store_eM14_dest, eM14_address_src, index_count_src);
    u32 eM14_written_temp = PushSystemTemp();
    a_.OpIBFE(dxbc::Dest::R(eM14_written_temp, export_eM14), dxbc::Src::LU(1),
              dxbc::Src::LU(1, 2, 3, 4),
              dxbc::Src::R(system_temp_memexport_enabled_and_eM_written_, dxbc::Src::kYYYY));
    a_.OpAnd(store_eM14_dest, store_eM14_src, dxbc::Src::R(eM14_written_temp));
    PopSystemTemp();
    a_.OpIShL(eM14_address_dest, eM14_address_src, element_size_src);
    a_.OpIAdd(eM14_address_dest, base_address_src, eM14_address_src);
  }
  if (export_eM & 0b1) {
    a_.OpIShL(eM0_address_dest, eM0_address_src, element_size_src);
    a_.OpIAdd(eM0_address_dest, base_address_src, eM0_address_src);
    a_.OpIBFE(dxbc::Dest::R(temp, 0b0100), dxbc::Src::LU(1), dxbc::Src::LU(0),
              dxbc::Src::R(system_temp_memexport_enabled_and_eM_written_, dxbc::Src::kYYYY));
  }
  dxbc::Src eM0_written_src(dxbc::Src::R(temp, dxbc::Src::kZZZZ));

  // An unknown format (UINT32_MAX size) enters no case, so nothing is written.
  if (uav_index_shared_memory_ == kBindingIndexUnallocated) {
    uav_index_shared_memory_ = uav_count_++;
  }
  u8 eM14_remaining;
  u32 eM14_index;
  a_.OpSwitch(element_size_src);

  dxbc::Dest atomic_dest(
      dxbc::Dest::U(uav_index_shared_memory_, std::to_underlying(UAVRegister::kSharedMemory), 0));
  for (u32 i = 0; i <= 1; ++i) {
    a_.OpCase(dxbc::Src::LU(i));
    dxbc::Src width_src(dxbc::Src::LU(8 << i));
    u32 sub_dword_temp = PushSystemTemp();
    if (export_eM & 0b1) {
      a_.OpIf(true, eM0_written_src);
      a_.OpBFI(dxbc::Dest::R(sub_dword_temp, 0b0001), dxbc::Src::LU(2), dxbc::Src::LU(3),
               eM0_address_src, dxbc::Src::LU(0));
      a_.OpAnd(eM0_address_dest, eM0_address_src, dxbc::Src::LU(~static_cast<u32>(3)));
      a_.OpBFI(dxbc::Dest::R(sub_dword_temp, 0b0010), width_src,
               dxbc::Src::R(sub_dword_temp, dxbc::Src::kXXXX), dxbc::Src::LU(0),
               dxbc::Src::LU(UINT32_MAX));
      a_.OpAtomicAnd(atomic_dest, eM0_address_src, 0b0001,
                     dxbc::Src::R(sub_dword_temp, dxbc::Src::kYYYY));
      a_.OpBFI(dxbc::Dest::R(sub_dword_temp, 0b0010), width_src,
               dxbc::Src::R(sub_dword_temp, dxbc::Src::kXXXX),
               dxbc::Src::R(system_temps_memexport_data_[0], dxbc::Src::kXXXX), dxbc::Src::LU(0));
      a_.OpAtomicOr(atomic_dest, eM0_address_src, 0b0001,
                    dxbc::Src::R(sub_dword_temp, dxbc::Src::kYYYY));
      a_.OpEndIf();
    }
    if (export_eM14) {
      a_.OpBFI(dxbc::Dest::R(sub_dword_temp, export_eM14), dxbc::Src::LU(2), dxbc::Src::LU(3),
               dxbc::Src::R(eM14_address_temp), dxbc::Src::LU(0));
      a_.OpAnd(dxbc::Dest::R(eM14_address_temp, export_eM14), dxbc::Src::R(eM14_address_temp),
               dxbc::Src::LU(~static_cast<u32>(3)));
      u32 sub_dword_data_temp = PushSystemTemp();
      eM14_remaining = export_eM14;
      while (rex::bit_scan_forward(eM14_remaining, &eM14_index)) {
        eM14_remaining &= ~(static_cast<u8>(1) << eM14_index);
        a_.OpIf(true, dxbc::Src::R(store_eM14_temp).Select(eM14_index));
        a_.OpBFI(dxbc::Dest::R(sub_dword_data_temp, 0b0001), width_src,
                 dxbc::Src::R(sub_dword_temp).Select(eM14_index), dxbc::Src::LU(0),
                 dxbc::Src::LU(UINT32_MAX));
        a_.OpAtomicAnd(atomic_dest, dxbc::Src::R(eM14_address_temp).Select(eM14_index), 0b0001,
                       dxbc::Src::R(sub_dword_data_temp, dxbc::Src::kXXXX));
        a_.OpBFI(dxbc::Dest::R(sub_dword_data_temp, 0b0001), width_src,
                 dxbc::Src::R(sub_dword_temp).Select(eM14_index),
                 dxbc::Src::R(system_temps_memexport_data_[1 + eM14_index], dxbc::Src::kXXXX),
                 dxbc::Src::LU(0));
        a_.OpAtomicOr(atomic_dest, dxbc::Src::R(eM14_address_temp).Select(eM14_index), 0b0001,
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
    if (export_eM & 0b1) {
      a_.OpIf(true, eM0_written_src);
      a_.OpStoreRaw(store_dest, eM0_address_src, dxbc::Src::R(system_temps_memexport_data_[0]));
      a_.OpEndIf();
    }
    eM14_remaining = export_eM14;
    while (rex::bit_scan_forward(eM14_remaining, &eM14_index)) {
      eM14_remaining &= ~(static_cast<u8>(1) << eM14_index);
      a_.OpIf(true, dxbc::Src::R(store_eM14_temp).Select(eM14_index));
      a_.OpStoreRaw(store_dest, dxbc::Src::R(eM14_address_temp).Select(eM14_index),
                    dxbc::Src::R(system_temps_memexport_data_[1 + eM14_index]));
      a_.OpEndIf();
    }
    a_.OpBreak();
  }

  a_.OpEndSwitch();

  if (export_eM14) {
    PopSystemTemp(2);
  }

  a_.OpEndIf();

  PopSystemTemp();

  a_.OpEndIf();

  a_.OpEndIf();
}

}  // namespace rex::gpu
