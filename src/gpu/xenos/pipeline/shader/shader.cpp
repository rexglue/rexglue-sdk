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

#include <cinttypes>
#include <cstring>
#include <utility>

#include <format>

#include <rex/logging/log.h>
#include <rex/platform/file.h>
#include <rex/xenos/ucode.h>
#include "gpu/xenos/pipeline/shader/shader.h"
#include <rex/math.h>
#include <rex/memory.h>

namespace rex::gpu {
using namespace xenos::ucode;

Shader::Shader(xenos::ShaderType shader_type, u64 ucode_hash, const u32* ucode_dwords,
               size_t ucode_dword_count, std::endian ucode_source_endian)
    : shader_type_(shader_type), ucode_data_hash_(ucode_hash) {
  ucode_dwords_.resize(ucode_dword_count);
  if (std::endian::native != ucode_source_endian) {
    memory::copy_and_swap(ucode_dwords_.data(), ucode_dwords, ucode_dword_count);
  } else {
    std::memcpy(ucode_dwords_.data(), ucode_dwords, sizeof(u32) * ucode_dword_count);
  }
}

Shader::~Shader() = default;

std::pair<std::filesystem::path, std::filesystem::path> Shader::Translation::DumpTranslation(
    const std::filesystem::path& base_path, const char* path_label) const {
  if (!IsValid()) {
    return std::make_pair(std::filesystem::path(), std::filesystem::path());
  }

  std::filesystem::path target_path = base_path;
  if (!target_path.empty()) {
    target_path = std::filesystem::absolute(target_path);
    std::filesystem::create_directories(target_path);
  }

  const char* type_extension =
      GetShader().GetType() == xenos::ShaderType::kVertex ? "vert" : "frag";

  std::filesystem::path binary_path =
      target_path / std::format("shader_{:016X}_{:016X}.{}.bin.{}", GetShader().GetUcodeDataHash(),
                                GetModification(), path_label, type_extension);
  if (auto written = platform::WriteHostFile(
          binary_path,
          std::span<const u8>(reinterpret_cast<const u8*>(translated_binary_.data()),
                              translated_binary_.size() * sizeof(*translated_binary_.data())));
      !written) {
    REXLOG_WARN(gpu, "write failed: {}", written.error().GetDescription());
  }

  std::filesystem::path disasm_path;
  if (!host_disassembly_.empty()) {
    disasm_path =
        target_path / std::format("shader_{:016X}_{:016X}.{}.{}", GetShader().GetUcodeDataHash(),
                                  GetModification(), path_label, type_extension);
    if (auto written = platform::WriteHostFile(disasm_path, host_disassembly_); !written) {
      REXLOG_WARN(gpu, "write failed: {}", written.error().GetDescription());
    }
  }

  return std::make_pair(std::move(binary_path), std::move(disasm_path));
}

Shader::Translation* Shader::GetOrCreateTranslation(u64 modification, bool* is_new) {
  auto it = translations_.find(modification);
  if (it != translations_.end()) {
    if (is_new) {
      *is_new = false;
    }
    return it->second.get();
  }
  auto inserted = translations_.emplace(modification, CreateTranslationInstance(modification));
  if (is_new) {
    *is_new = true;
  }
  return inserted.first->second.get();
}

void Shader::DestroyTranslation(u64 modification) {
  auto it = translations_.find(modification);
  if (it == translations_.end()) {
    return;
  }
  translations_.erase(it);
}

std::pair<std::filesystem::path, std::filesystem::path> Shader::DumpUcode(
    const std::filesystem::path& base_path) const {
  std::filesystem::path target_path = base_path;
  if (!target_path.empty()) {
    target_path = std::filesystem::absolute(target_path);
    std::filesystem::create_directories(target_path);
  }

  const char* type_extension = GetType() == xenos::ShaderType::kVertex ? "vert" : "frag";

  std::filesystem::path binary_path =
      target_path / std::format("shader_{:016X}.ucode.bin.{}", GetUcodeDataHash(), type_extension);
  if (auto written = platform::WriteHostFile(
          binary_path, std::span<const u8>(reinterpret_cast<const u8*>(GetUcodeData().data()),
                                           GetUcodeData().size() * sizeof(*GetUcodeData().data())));
      !written) {
    REXLOG_WARN(gpu, "write failed: {}", written.error().GetDescription());
  }

  std::filesystem::path disasm_path;
  if (IsUcodeAnalyzed()) {
    disasm_path =
        target_path / std::format("shader_{:016X}.ucode.{}", GetUcodeDataHash(), type_extension);
    if (auto written = platform::WriteHostFile(disasm_path, GetUcodeDisassembly()); !written) {
      REXLOG_WARN(gpu, "write failed: {}", written.error().GetDescription());
    }
  }

  return std::make_pair(std::move(binary_path), std::move(disasm_path));
}

std::unique_ptr<Shader::Translation> Shader::CreateTranslationInstance(u64 modification) {
  return std::make_unique<Translation>(*this, modification);
}

}  // namespace rex::gpu
