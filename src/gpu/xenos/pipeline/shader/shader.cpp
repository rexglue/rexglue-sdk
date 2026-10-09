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
#include <fstream>
#include <utility>

#include <format>

#include <rex/xenos/ucode.h>
#include "gpu/xenos/pipeline/shader/shader.h"
#include <rex/math.h>
#include <rex/memory.h>
#include <rex/string.h>

namespace rex::gpu {
using namespace xenos::ucode;

Shader::Shader(xenos::ShaderType shader_type, u64 ucode_data_hash, const u32* ucode_dwords,
               size_t ucode_dword_count, std::endian ucode_source_endian)
    : shader_type_(shader_type), ucode_data_hash_(ucode_data_hash) {
  ucode_data_.resize(ucode_dword_count);
  if (std::endian::native != ucode_source_endian) {
    memory::copy_and_swap(ucode_data_.data(), ucode_dwords, ucode_dword_count);
  } else {
    std::memcpy(ucode_data_.data(), ucode_dwords, sizeof(u32) * ucode_dword_count);
  }
}

Shader::~Shader() = default;

std::pair<std::filesystem::path, std::filesystem::path> Shader::Translation::Dump(
    const std::filesystem::path& base_path, const char* path_prefix) const {
  if (!is_valid()) {
    return std::make_pair(std::filesystem::path(), std::filesystem::path());
  }

  std::filesystem::path path = base_path;
  std::filesystem::path target_path = base_path;
  if (!target_path.empty()) {
    target_path = std::filesystem::absolute(target_path);
    std::filesystem::create_directories(target_path);
  }

  const char* type_extension = shader().type() == xenos::ShaderType::kVertex ? "vert" : "frag";

  std::filesystem::path binary_path =
      target_path / std::format("shader_{:016X}_{:016X}.{}.bin.{}", shader().ucode_data_hash(),
                                modification(), path_prefix, type_extension);
  std::ofstream binary_file(binary_path, std::ios::binary | std::ios::trunc);
  binary_file.write(
      reinterpret_cast<const char*>(translated_binary_.data()),
      static_cast<std::streamsize>(translated_binary_.size() * sizeof(*translated_binary_.data())));

  std::filesystem::path disasm_path;
  if (!host_disassembly_.empty()) {
    disasm_path =
        target_path / std::format("shader_{:016X}_{:016X}.{}.{}", shader().ucode_data_hash(),
                                  modification(), path_prefix, type_extension);
    std::ofstream disasm_file(disasm_path, std::ios::trunc);
    disasm_file.write(host_disassembly_.data(),
                      static_cast<std::streamsize>(host_disassembly_.size()));
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

  const char* type_extension = type() == xenos::ShaderType::kVertex ? "vert" : "frag";

  std::filesystem::path binary_path =
      target_path / std::format("shader_{:016X}.ucode.bin.{}", ucode_data_hash(), type_extension);
  std::ofstream binary_file(binary_path, std::ios::binary | std::ios::trunc);
  binary_file.write(
      reinterpret_cast<const char*>(ucode_data().data()),
      static_cast<std::streamsize>(ucode_data().size() * sizeof(*ucode_data().data())));

  std::filesystem::path disasm_path;
  if (is_ucode_analyzed()) {
    disasm_path =
        target_path / std::format("shader_{:016X}.ucode.{}", ucode_data_hash(), type_extension);
    std::ofstream disasm_file(disasm_path, std::ios::trunc);
    disasm_file.write(ucode_disassembly().data(),
                      static_cast<std::streamsize>(ucode_disassembly().size()));
  }

  return std::make_pair(std::move(binary_path), std::move(disasm_path));
}

std::unique_ptr<Shader::Translation> Shader::CreateTranslationInstance(u64 modification) {
  return std::make_unique<Translation>(*this, modification);
}

}  // namespace rex::gpu
