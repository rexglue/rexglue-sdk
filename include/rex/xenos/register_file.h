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

/** Name and value type of a register in the register table. */
struct RegisterInfo {
  /** How a register value is interpreted. */
  enum class Type {
    kDword, /**< Raw 32 bit integer value. */
    kFloat, /**< 32 bit floating point value. */
  };
  Type type;        /**< How the register value is interpreted. */
  const char* name; /**< Register name from the register table. */
};

/** Storage for every GPU register, indexed by dword register index. */
class REX_API RegisterFile {
 public:
  /**
   * Looks up a register in the register table.
   *
   * @param index Dword register index.
   * @return Name and type of the register, or nullptr when the index is not in the table.
   */
  static const RegisterInfo* GetRegisterInfo(u32 index);

  /** Number of dword registers, 1 past the highest register index. */
  static constexpr size_t kRegisterCount = 0x5003;
  /** Register values indexed by dword register index. */
  u32 values[kRegisterCount]{};

  const u32& operator[](u32 reg) const { return values[reg]; }
  u32& operator[](u32 reg) { return values[reg]; }
  const u32& operator[](Register reg) const { return values[std::to_underlying(reg)]; }
  u32& operator[](Register reg) { return values[std::to_underlying(reg)]; }

  template <typename T>
  /**
   * Reinterprets a register value as a register layout or other 32 bit type.
   *
   * @tparam T Type with the size of a register.
   * @param reg Dword register index.
   * @return The register bits as T.
   */
  T Get(u32 reg) const {
    return std::bit_cast<T>(values[reg]);
  }
  template <typename T>
  /**
   * Reinterprets a register value as a register layout or other 32 bit type.
   *
   * @tparam T Type with the size of a register.
   * @param reg Register to read.
   * @return The register bits as T.
   */
  T Get(Register reg) const {
    return Get<T>(std::to_underlying(reg));
  }
  template <typename T>
  /**
   * Reads the register that a register layout declares as its own.
   *
   * @tparam T Register layout declaring `register_index`.
   * @return The register bits as T.
   */
  T Get() const {
    return Get<T>(T::register_index);
  }

  /**
   * Reads a vertex fetch constant from the shader fetch constant registers.
   *
   * @param index Fetch constant slot, below 96.
   * @return Copy of the vertex fetch constant.
   */
  xenos::VertexFetch GetVertexFetch(u32 index) const {
    REX_ASSERT(index < 96);
    xenos::VertexFetch fetch;
    std::memcpy(&fetch,
                &values[std::to_underlying(xenos::Register::SHADER_CONSTANT_FETCH_00_0) +
                        (sizeof(fetch) / sizeof(u32)) * index],
                sizeof(fetch));
    return fetch;
  }

  /**
   * Reads a texture fetch constant from the shader fetch constant registers.
   *
   * @param index Fetch constant slot, below 32.
   * @return Copy of the texture fetch constant.
   */
  xenos::TextureFetch GetTextureFetch(u32 index) const {
    REX_ASSERT(index < 32);
    xenos::TextureFetch fetch;
    std::memcpy(&fetch,
                &values[std::to_underlying(xenos::Register::SHADER_CONSTANT_FETCH_00_0) +
                        (sizeof(fetch) / sizeof(u32)) * index],
                sizeof(fetch));
    return fetch;
  }

  /**
   * Reads a memory export stream constant from a float shader constant.
   *
   * @param float_constant_index Float constant index, below 512.
   * @return Copy of the memory export stream constant.
   */
  xenos::MemExportStream GetMemExportStream(u32 float_constant_index) const {
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
