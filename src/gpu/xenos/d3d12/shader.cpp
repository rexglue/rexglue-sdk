/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2018 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cstring>
#include <utility>

#include <rex/assert.h>
#include "gpu/xenos/d3d12/shader.h"
#include "gpu/xenos/pipeline/shader/dxbc.h"
#include <rex/logging/log.h>
#include "gpu/xenos/host/d3d12/d3d12_api.h"

namespace rex::gpu::d3d12 {

D3D12Shader::D3D12Shader(xenos::ShaderType shader_type, u64 ucode_data_hash,
                         const u32* ucode_dwords, size_t ucode_dword_count,
                         std::endian ucode_source_endian)
    : DXBCShader(shader_type, ucode_data_hash, ucode_dwords, ucode_dword_count,
                 ucode_source_endian) {}

void D3D12Shader::D3D12Translation::DisassembleDXBCAndDXIL(
    const gpu::d3d12::D3D12Provider& provider, bool disassemble_dxbc,
    IDxbcConverter* dxbc_converter, IDxcUtils* dxc_utils, IDxcCompiler* dxc_compiler) {
  std::string disassembly;
  bool is_first_disassembly = true;
  if (disassemble_dxbc) {
    ID3DBlob* dxbc_disassembly;
    if (SUCCEEDED(provider.Disassemble(
            GetTranslatedBinary().data(), GetTranslatedBinary().size(),
            D3D_DISASM_ENABLE_INSTRUCTION_NUMBERING | D3D_DISASM_ENABLE_INSTRUCTION_OFFSET, nullptr,
            &dxbc_disassembly))) {
      REX_ASSERT(is_first_disassembly);
      is_first_disassembly = false;
      disassembly.append(reinterpret_cast<const char*>(dxbc_disassembly->GetBufferPointer()));
      dxbc_disassembly->Release();
    } else {
      REXLOG_ERROR(gpu, "Failed to disassemble DXBC shader {:016X}",
                   GetShader().GetUcodeDataHash());
    }
  }
  if (dxbc_converter && dxc_utils && dxc_compiler) {
    void* dxil;
    UINT32 dxil_size;
    if (SUCCEEDED(dxbc_converter->Convert(GetTranslatedBinary().data(),
                                          static_cast<UINT32>(GetTranslatedBinary().size()),
                                          nullptr, &dxil, &dxil_size, nullptr)) &&
        dxil != nullptr) {
      IDxcBlobEncoding* dxil_blob;
      if (SUCCEEDED(dxc_utils->CreateBlobFromPinned(dxil, dxil_size, DXC_CP_ACP, &dxil_blob))) {
        IDxcBlobEncoding* dxil_disassembly;
        bool dxil_disassembled = SUCCEEDED(dxc_compiler->Disassemble(dxil_blob, &dxil_disassembly));
        dxil_blob->Release();
        CoTaskMemFree(dxil);
        if (dxil_disassembled) {
          IDxcBlobUtf8* dxil_disassembly_utf8;
          bool dxil_disassembly_got_utf8 =
              SUCCEEDED(dxc_utils->GetBlobAsUtf8(dxil_disassembly, &dxil_disassembly_utf8));
          dxil_disassembly->Release();
          if (dxil_disassembly_got_utf8) {
            if (!is_first_disassembly) {
              disassembly.append("\n\n");
            }
            is_first_disassembly = false;
            disassembly.append(
                reinterpret_cast<const char*>(dxil_disassembly_utf8->GetStringPointer()));
            dxil_disassembly_utf8->Release();
          } else {
            REXLOG_ERROR(gpu, "Failed to get DXIL shader {:016X} disassembly as UTF-8",
                         GetShader().GetUcodeDataHash());
          }
        } else {
          REXLOG_ERROR(gpu, "Failed to disassemble DXIL shader {:016X}",
                       GetShader().GetUcodeDataHash());
        }
      } else {
        REXLOG_ERROR(gpu, "Failed to create a blob with DXIL shader {:016X}",
                     GetShader().GetUcodeDataHash());
        CoTaskMemFree(dxil);
      }
    } else {
      REXLOG_ERROR(gpu, "Failed to convert shader {:016X} to DXIL", GetShader().GetUcodeDataHash());
    }
  }
  SetHostDisassembly(std::move(disassembly));
}

std::unique_ptr<Shader::Translation> D3D12Shader::CreateTranslationInstance(u64 modification) {
  return std::make_unique<D3D12Translation>(*this, modification);
}

}  // namespace rex::gpu::d3d12
