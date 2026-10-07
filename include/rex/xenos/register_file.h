/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

#include <rex/assert.h>
#include <rex/macros.h>
#include <rex/xenos/registers.h>
#include <rex/xenos/xenos.h>

namespace rex::xenos {

struct RegisterInfo {
  enum class Type {
    kDword,
    kFloat,
  };
  Type type;
  const char* name;
};

class REX_API RegisterFile {
 public:
  static const RegisterInfo* GetRegisterInfo(uint32_t index);

  static constexpr size_t kRegisterCount = 0x5003;
  u32 values[kRegisterCount]{};

  const uint32_t& operator[](uint32_t reg) const { return values[reg]; }
  uint32_t& operator[](uint32_t reg) { return values[reg]; }
  const uint32_t& operator[](Register reg) const { return values[std::to_underlying(reg)]; }
  uint32_t& operator[](Register reg) { return values[std::to_underlying(reg)]; }

  template <typename T>
  T Get(uint32_t reg) const {
    return std::bit_cast<T>(values[reg]);
  }
  template <typename T>
  T Get(Register reg) const {
    return Get<T>(std::to_underlying(reg));
  }
  template <typename T>
  T Get() const {
    return Get<T>(T::register_index);
  }

  xenos::VertexFetch GetVertexFetch(uint32_t index) const {
    REX_ASSERT(index < 96);
    xenos::VertexFetch fetch;
    std::memcpy(&fetch,
                &values[std::to_underlying(xenos::Register::SHADER_CONSTANT_FETCH_00_0) +
                        (sizeof(fetch) / sizeof(uint32_t)) * index],
                sizeof(fetch));
    return fetch;
  }

  xenos::TextureFetch GetTextureFetch(uint32_t index) const {
    REX_ASSERT(index < 32);
    xenos::TextureFetch fetch;
    std::memcpy(&fetch,
                &values[std::to_underlying(xenos::Register::SHADER_CONSTANT_FETCH_00_0) +
                        (sizeof(fetch) / sizeof(uint32_t)) * index],
                sizeof(fetch));
    return fetch;
  }

  xenos::MemExportStream GetMemExportStream(uint32_t float_constant_index) const {
    REX_ASSERT(float_constant_index < 512);
    xenos::MemExportStream stream;
    std::memcpy(&stream,
                &values[std::to_underlying(xenos::Register::SHADER_CONSTANT_000_X) +
                        4 * float_constant_index],
                sizeof(stream));
    return stream;
  }
};

}  // namespace rex::xenos
