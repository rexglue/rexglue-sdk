/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include "crypto/TinySHA1.hpp"
#include "crypto/rijndael-alg-fst.c"
#include "crypto/rijndael-alg-fst.h"
#include "pe/pe_image.h"

#include <algorithm>
#include <unordered_map>

#include <fmt/format.h>

#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory.h>
#include <rex/runtime.h>
#include <rex/system/export_resolver.h>
#include <rex/system/flags.h>
#include <rex/system/function.h>
#include <rex/system/kernel_state.h>
#include <rex/system/lzx.h>
#include <rex/system/processor.h>
#include <rex/system/xex_module.h>
#include <rex/system/xmodule.h>
#include <rex/types.h>

static const uint8_t xe_xex2_retail_key[16] = {0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3,
                                               0x40, 0x58, 0x3F, 0xBB, 0x08, 0x96, 0xBF, 0x91};
static const uint8_t xe_xex2_devkit_key[16] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

void aes_decrypt_buffer(const uint8_t* session_key, const uint8_t* input_buffer,
                        const size_t input_size, uint8_t* output_buffer, const size_t output_size) {
  uint32_t rk[4 * (MAXNR + 1)];
  uint8_t ivec[16] = {0};
  int32_t Nr = rijndaelKeySetupDec(rk, session_key, 128);
  const uint8_t* ct = input_buffer;
  uint8_t* pt = output_buffer;
  for (size_t n = 0; n < input_size; n += 16, ct += 16, pt += 16) {
    // Decrypt 16 uint8_ts from input -> output.
    rijndaelDecrypt(rk, Nr, ct, pt);
    for (size_t i = 0; i < 16; i++) {
      // XOR with previous.
      pt[i] ^= ivec[i];
      // Set previous.
      ivec[i] = ct[i];
    }
  }
}

namespace rex::runtime {

using rex::system::KernelState;

XexModule::XexModule(Processor* processor, KernelState* kernel_state)
    : Module(processor), kernel_state_(kernel_state) {}

XexModule::~XexModule() {}

bool XexModule::GetOptHeader(const xex2_header* header, xex2_header_keys key, void** out_ptr) {
  assert_not_null(header);
  assert_not_null(out_ptr);

  for (uint32_t i = 0; i < header->header_count; i++) {
    const xex2_opt_header& opt_header = header->headers[i];
    if (opt_header.key == key) {
      // Match!
      switch (key & 0xFF) {
        case 0x00: {
          // We just return the value of the optional header.
          // Assume that the output pointer points to a uint32_t.
          *reinterpret_cast<uint32_t*>(out_ptr) = static_cast<uint32_t>(opt_header.value);
        } break;
        case 0x01: {
          // Pointer to the value on the optional header.
          *out_ptr = const_cast<void*>(reinterpret_cast<const void*>(&opt_header.value));
        } break;
        default: {
          // Pointer to the header.
          *out_ptr = reinterpret_cast<void*>(uintptr_t(header) + opt_header.offset);
        } break;
      }

      return true;
    }
  }

  return false;
}

bool XexModule::GetOptHeader(xex2_header_keys key, void** out_ptr) const {
  return XexModule::GetOptHeader(xex_header(), key, out_ptr);
}

const void* XexModule::GetSecurityInfo(const xex2_header* header) {
  return reinterpret_cast<const void*>(uintptr_t(header) + header->security_offset);
}

const PESection* XexModule::GetPESection(const char* name) {
  for (std::vector<PESection>::iterator it = pe_sections_.begin(); it != pe_sections_.end(); ++it) {
    if (!strcmp(it->name, name)) {
      return &(*it);
    }
  }
  return nullptr;
}

uint32_t XexModule::GetProcAddress(uint16_t ordinal) const {
  // First: Check the xex2 export table.
  if (xex_security_info()->export_table) {
    auto export_table =
        memory()->TranslateVirtual<const xex2_export_table*>(xex_security_info()->export_table);

    ordinal -= export_table->base;
    if (ordinal >= export_table->count) {
      REXLOG_ERROR("GetProcAddress({:03X}): ordinal out of bounds", ordinal);
      return 0;
    }

    uint32_t num = ordinal;
    uint32_t ordinal_offset = export_table->ordOffset[num];
    ordinal_offset += export_table->imagebaseaddr << 16;
    return ordinal_offset;
  }

  // Second: Check the PE exports.
  assert_not_zero(base_address_);

  xex2_opt_data_directory* pe_export_directory = 0;
  if (GetOptHeader(XEX_HEADER_EXPORTS_BY_NAME, &pe_export_directory)) {
    auto e = memory()->TranslateVirtual<const X_IMAGE_EXPORT_DIRECTORY*>(
        base_address_ + pe_export_directory->offset);
    assert_not_null(e);

    uint32_t* function_table = reinterpret_cast<uint32_t*>(uintptr_t(e) + e->AddressOfFunctions);

    if (ordinal < e->NumberOfFunctions) {
      return base_address_ + function_table[ordinal];
    }
  }

  return 0;
}

uint32_t XexModule::GetProcAddress(const std::string_view name) const {
  assert_not_zero(base_address_);

  xex2_opt_data_directory* pe_export_directory = 0;
  if (!GetOptHeader(XEX_HEADER_EXPORTS_BY_NAME, &pe_export_directory)) {
    // No exports by name.
    return 0;
  }

  auto e = memory()->TranslateVirtual<const X_IMAGE_EXPORT_DIRECTORY*>(base_address_ +
                                                                       pe_export_directory->offset);
  assert_not_null(e);

  // e->AddressOfX RVAs are relative to the IMAGE_EXPORT_DIRECTORY!
  uint32_t* function_table = reinterpret_cast<uint32_t*>(uintptr_t(e) + e->AddressOfFunctions);

  // Names relative to directory
  uint32_t* name_table = reinterpret_cast<uint32_t*>(uintptr_t(e) + e->AddressOfNames);

  // Table of ordinals (by name)
  uint16_t* ordinal_table = reinterpret_cast<uint16_t*>(uintptr_t(e) + e->AddressOfNameOrdinals);

  for (uint32_t i = 0; i < e->NumberOfNames; i++) {
    auto fn_name = reinterpret_cast<const char*>(uintptr_t(e) + name_table[i]);
    uint16_t ordinal = ordinal_table[i];
    uint32_t addr = base_address_ + function_table[ordinal];
    if (name == std::string_view(fn_name)) {
      // We have a match!
      return addr;
    }
  }

  // No match
  return 0;
}

int XexModule::ApplyPatch(XexModule* module) {
  if (!is_patch()) {
    // This isn't a XEX2 patch.
    return 1;
  }

  // Grab the delta descriptor and get to work.
  xex2_opt_delta_patch_descriptor* patch_header = nullptr;
  GetOptHeader(XEX_HEADER_DELTA_PATCH_DESCRIPTOR, reinterpret_cast<void**>(&patch_header));
  assert_not_null(patch_header);

  // Compare hash inside delta descriptor to base XEX signature
  uint8_t digest[0x14];
  sha1::SHA1 s;
  s.processBytes(module->xex_security_info()->rsa_signature, 0x100);
  s.finalize(digest);

  if (memcmp(digest, patch_header->digest_source, 0x14) != 0) {
    REXLOG_WARN(
        "XEX patch signature hash doesn't match base XEX signature hash, patch "
        "will likely fail!");
  }

  uint32_t size = module->xex_header()->header_size;
  if (patch_header->delta_headers_source_offset > size) {
    REXLOG_ERROR("XEX header patch source is outside base XEX header area");
    return 2;
  }

  uint32_t header_size_available = size - patch_header->delta_headers_source_offset;
  if (patch_header->delta_headers_source_size > header_size_available) {
    REXLOG_ERROR("XEX header patch source is too large");
    return 3;
  }

  if (patch_header->delta_headers_target_offset > patch_header->size_of_target_headers) {
    REXLOG_ERROR("XEX header patch target is outside base XEX header area");
    return 4;
  }

  uint32_t delta_target_size =
      patch_header->size_of_target_headers - patch_header->delta_headers_target_offset;
  if (patch_header->delta_headers_source_size > delta_target_size) {
    return 5;  // ? unsure what the point of this test is, kernel checks for it
               // though
  }

  // Patch base XEX header
  uint32_t original_image_size = module->image_size();
  uint32_t header_target_size = patch_header->size_of_target_headers;

  if (!header_target_size) {
    header_target_size =
        patch_header->delta_headers_target_offset + patch_header->delta_headers_source_size;
  }

  size_t mem_size = module->xex_header_mem_.size();

  // Increase xex header buffer length if needed
  if (header_target_size > module->xex_header_mem_.size()) {
    module->xex_header_mem_.resize(header_target_size);
  }

  auto header_ptr = (uint8_t*)module->xex_header();

  // If headers_source_offset is set, copy [source_offset:source_size] to
  // target_offset
  if (patch_header->delta_headers_source_offset) {
    memcpy(header_ptr + patch_header->delta_headers_target_offset,
           header_ptr + patch_header->delta_headers_source_offset,
           patch_header->delta_headers_source_size);
  }

  // If new size is smaller than original, null out the difference
  if (header_target_size < module->xex_header_mem_.size()) {
    memset(header_ptr + header_target_size, 0, module->xex_header_mem_.size() - header_target_size);
  }

  auto file_format_header = opt_file_format_info();
  assert_not_null(file_format_header);

  // Apply header patch...
  uint32_t headerpatch_size = patch_header->info.compressed_len + 0xC;

  int result_code =
      lzxdelta_apply_patch(&patch_header->info, headerpatch_size,
                           file_format_header->compression_info.normal.window_size, header_ptr);
  if (result_code) {
    REXLOG_ERROR("XEX header patch application failed, error code {}", result_code);
    return result_code;
  }

  // Decrease xex header buffer length if needed (but only after patching)
  if (module->xex_header_mem_.size() > header_target_size) {
    module->xex_header_mem_.resize(header_target_size);
  }

  // Update security info context with latest security info data
  module->ReadSecurityInfo();

  uint32_t new_image_size = module->image_size();

  // Check if we need to alloc new memory for the patched xex
  if (new_image_size > original_image_size) {
    uint32_t size_delta = new_image_size - original_image_size;
    uint32_t addr_new_mem = module->base_address_ + original_image_size;

    bool alloc_result =
        memory()
            ->LookupHeap(addr_new_mem)
            ->AllocFixed(
                addr_new_mem, size_delta, 4096,
                rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite);

    if (!alloc_result) {
      REXLOG_ERROR("Unable to allocate XEX memory at {:08X}-{:08X}.", addr_new_mem, size_delta);
      assert_always();
      return 6;
    }
  }

  uint8_t orig_session_key[0x10];
  memcpy(orig_session_key, module->session_key_, 0x10);

  // Header patch updated the base XEX key, need to redecrypt it
  aes_decrypt_buffer(module->is_dev_kit_ ? xe_xex2_devkit_key : xe_xex2_retail_key,
                     reinterpret_cast<const uint8_t*>(module->xex_security_info()->aes_key), 16,
                     module->session_key_, 16);

  // Decrypt the patch XEX's key using base XEX key
  aes_decrypt_buffer(module->session_key_,
                     reinterpret_cast<const uint8_t*>(xex_security_info()->aes_key), 16,
                     session_key_, 16);

  // Test delta key against our decrypted keys
  // (kernel doesn't seem to check this, but it's the one use for the
  // image_key_source field I can think of...)
  uint8_t test_delta_key[0x10];
  aes_decrypt_buffer(module->session_key_, patch_header->image_key_source, 0x10, test_delta_key,
                     0x10);

  if (memcmp(test_delta_key, orig_session_key, 0x10) != 0) {
    REXLOG_ERROR("XEX patch image key doesn't match original XEX!");
    return 7;
  }

  // Decrypt (if needed).
  bool free_input = false;
  const uint8_t* patch_buffer = xexp_data_mem_.data();
  const size_t patch_length = xexp_data_mem_.size();

  const uint8_t* input_buffer = patch_buffer;

  switch (file_format_header->encryption_type) {
    case XEX_ENCRYPTION_NONE:
      // No-op.
      break;
    case XEX_ENCRYPTION_NORMAL:
      // TODO: a way to do without a copy/alloc?
      free_input = true;
      input_buffer = (const uint8_t*)calloc(1, patch_length);
      aes_decrypt_buffer(session_key_, patch_buffer, patch_length, (uint8_t*)input_buffer,
                         patch_length);
      break;
    default:
      assert_always();
      return 8;
  }

  const xex2_compressed_block_info* cur_block =
      &file_format_header->compression_info.normal.first_block;

  const uint8_t* p = input_buffer;
  uint8_t* base_exe = memory()->TranslateVirtual(module->base_address_);

  // If image_source_offset is set, copy [source_offset:source_size] to
  // target_offset
  if (patch_header->delta_image_source_offset) {
    memcpy(base_exe + patch_header->delta_image_target_offset,
           base_exe + patch_header->delta_image_source_offset,
           patch_header->delta_image_source_size);
  }

  // TODO: should we use new_image_size here instead?
  uint32_t image_target_size =
      patch_header->delta_image_target_offset + patch_header->delta_image_source_size;

  // If new size is smaller than original, null out the difference
  if (image_target_size < original_image_size) {
    memset(base_exe + image_target_size, 0, original_image_size - image_target_size);
  }

  // Now loop through each block and apply the delta patches inside
  while (cur_block->block_size) {
    const auto* next_block = (const xex2_compressed_block_info*)p;

    // Compare block hash, if no match we probably used wrong decrypt key
    s.reset();
    s.processBytes(p, cur_block->block_size);
    s.finalize(digest);

    if (memcmp(digest, cur_block->block_hash, 0x14) != 0) {
      result_code = 9;
      REXLOG_ERROR("XEX patch block hash doesn't match hash inside block info!");
      break;
    }

    // skip block info
    p += 20;
    p += 4;

    uint32_t block_data_size = cur_block->block_size - 20 - 4;

    // Apply delta patch
    result_code =
        lzxdelta_apply_patch((xex2_delta_patch*)p, block_data_size,
                             file_format_header->compression_info.normal.window_size, base_exe);
    if (result_code) {
      break;
    }

    p += block_data_size;
    cur_block = next_block;
  }

  if (!result_code) {
    // Decommit unused pages if new image size is smaller than original
    if (original_image_size > new_image_size) {
      uint32_t size_delta = original_image_size - new_image_size;
      uint32_t addr_free_mem = module->base_address_ + new_image_size;

      bool free_result = memory()->LookupHeap(addr_free_mem)->Decommit(addr_free_mem, size_delta);

      if (!free_result) {
        REXLOG_ERROR("Unable to decommit XEX memory at {:08X}-{:08X}.", addr_free_mem, size_delta);
        assert_always();
      }
    }

    xex2_version source_ver, target_ver;
    source_ver = patch_header->source_version();
    target_ver = patch_header->target_version();
    REXLOG_INFO(
        "XEX patch applied successfully: base version: {}.{}.{}.{}, new "
        "version: {}.{}.{}.{}",
        (uint32_t)source_ver.major, (uint32_t)source_ver.minor, (uint32_t)source_ver.build,
        (uint32_t)source_ver.qfe, (uint32_t)target_ver.major, (uint32_t)target_ver.minor,
        (uint32_t)target_ver.build, (uint32_t)target_ver.qfe);
  } else {
    REXLOG_ERROR("XEX patch application failed, error code {}", result_code);
  }

  if (free_input) {
    free((void*)input_buffer);
  }
  return result_code;
}

int XexModule::ReadImage(const void* xex_addr, size_t xex_length, bool use_dev_key) {
  if (!opt_file_format_info()) {
    return 1;
  }

  is_dev_kit_ = use_dev_key;

  if (is_patch()) {
    // Make a copy of patch data for other XEX's to use with ApplyPatch()
    const uint32_t data_len = static_cast<uint32_t>(xex_length - xex_header()->header_size);
    xexp_data_mem_.resize(data_len);
    std::memcpy(xexp_data_mem_.data(), (uint8_t*)xex_addr + xex_header()->header_size, data_len);
    return 0;
  }

  memory()->LookupHeap(base_address_)->Reset();

  aes_decrypt_buffer(use_dev_key ? xe_xex2_devkit_key : xe_xex2_retail_key,
                     reinterpret_cast<const uint8_t*>(xex_security_info()->aes_key), 16,
                     session_key_, 16);

  int result_code = 0;
  switch (opt_file_format_info()->compression_type) {
    case XEX_COMPRESSION_NONE:
      result_code = ReadImageUncompressed(xex_addr, xex_length);
      break;
    case XEX_COMPRESSION_BASIC:
      result_code = ReadImageBasicCompressed(xex_addr, xex_length);
      break;
    case XEX_COMPRESSION_NORMAL:
      result_code = ReadImageCompressed(xex_addr, xex_length);
      break;
    default:
      assert_always();
      return 2;
  }

  if (result_code) {
    return result_code;
  }

  if (is_patch() || is_valid_executable()) {
    return 0;
  }

  // Not a patch and image doesn't have proper PE header, return 3
  return 3;
}

int XexModule::ReadImageUncompressed(const void* xex_addr, size_t xex_length) {
  // Allocate in-place the XEX memory.
  const uint32_t exe_length = static_cast<uint32_t>(xex_length - xex_header()->header_size);

  uint32_t uncompressed_size = exe_length;
  bool alloc_result =
      memory()
          ->LookupHeap(base_address_)
          ->AllocFixed(base_address_, uncompressed_size, 4096,
                       rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                       rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite);
  if (!alloc_result) {
    REXLOG_ERROR("Unable to allocate XEX memory at {:08X}-{:08X}.", base_address_,
                 uncompressed_size);
    return 2;
  }
  uint8_t* buffer = memory()->TranslateVirtual(base_address_);
  std::memset(buffer, 0, uncompressed_size);

  const uint8_t* p = (const uint8_t*)xex_addr + xex_header()->header_size;

  switch (opt_file_format_info()->encryption_type) {
    case XEX_ENCRYPTION_NONE:
      if (exe_length > uncompressed_size) {
        return 1;
      }
      memcpy(buffer, p, exe_length);
      return 0;
    case XEX_ENCRYPTION_NORMAL:
      aes_decrypt_buffer(session_key_, p, exe_length, buffer, uncompressed_size);
      return 0;
    default:
      assert_always();
      return 1;
  }

  return 0;
}

int XexModule::ReadImageBasicCompressed(const void* xex_addr, size_t xex_length) {
  const uint32_t exe_length = static_cast<uint32_t>(xex_length - xex_header()->header_size);
  const uint8_t* source_buffer = (const uint8_t*)xex_addr + xex_header()->header_size;
  const uint8_t* p = source_buffer;

  auto heap = memory()->LookupHeap(base_address_);

  // Calculate uncompressed length.
  uint32_t uncompressed_size = 0;

  auto* file_info = opt_file_format_info();
  auto& comp_info = file_info->compression_info.basic;

  uint32_t block_count = (file_info->info_size - 8) / 8;
  for (uint32_t n = 0; n < block_count; n++) {
    const uint32_t data_size = comp_info.blocks[n].data_size;
    const uint32_t zero_size = comp_info.blocks[n].zero_size;
    uncompressed_size += data_size + zero_size;
  }

  // Calculate the total size of the XEX image from its headers.
  uint32_t total_size = 0;
  for (uint32_t i = 0; i < xex_security_info()->page_descriptor_count; i++) {
    // Byteswap the bitfield manually.
    xex2_page_descriptor desc;
    desc.value = rex::byte_swap(xex_security_info()->page_descriptors[i].value);

    total_size += desc.page_count * heap->page_size();
  }

  // Allocate in-place the XEX memory.
  bool alloc_result =
      heap->AllocFixed(base_address_, total_size, 4096,
                       rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                       rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite);
  if (!alloc_result) {
    REXLOG_ERROR("Unable to allocate XEX memory at {:08X}-{:08X}.", base_address_,
                 uncompressed_size);
    return 1;
  }

  uint8_t* buffer = memory()->TranslateVirtual(base_address_);
  std::memset(buffer, 0, total_size);  // Quickly zero the contents.
  uint8_t* d = buffer;

  uint32_t rk[4 * (MAXNR + 1)];
  uint8_t ivec[16] = {0};
  int32_t Nr = rijndaelKeySetupDec(rk, session_key_, 128);

  for (size_t n = 0; n < block_count; n++) {
    const uint32_t data_size = comp_info.blocks[n].data_size;
    const uint32_t zero_size = comp_info.blocks[n].zero_size;

    switch (opt_file_format_info()->encryption_type) {
      case XEX_ENCRYPTION_NONE:
        if (data_size > uncompressed_size - (d - buffer)) {
          // Overflow.
          return 1;
        }
        memcpy(d, p, data_size);
        break;
      case XEX_ENCRYPTION_NORMAL: {
        const uint8_t* ct = p;
        uint8_t* pt = d;
        for (size_t m = 0; m < data_size; m += 16, ct += 16, pt += 16) {
          // Decrypt 16 uint8_ts from input -> output.
          rijndaelDecrypt(rk, Nr, ct, pt);
          for (size_t i = 0; i < 16; i++) {
            // XOR with previous.
            pt[i] ^= ivec[i];
            // Set previous.
            ivec[i] = ct[i];
          }
        }
      } break;
      default:
        assert_always();
        return 1;
    }

    p += data_size;
    d += data_size + zero_size;
  }

  return 0;
}

int XexModule::ReadImageCompressed(const void* xex_addr, size_t xex_length) {
  const uint32_t exe_length = static_cast<uint32_t>(xex_length - xex_header()->header_size);
  const uint8_t* exe_buffer = (const uint8_t*)xex_addr + xex_header()->header_size;

  // src -> dest:
  // - decrypt (if encrypted)
  // - de-block:
  //    4b total size of next block in uint8_ts
  //   20b hash of entire next block (including size/hash)
  //    Nb block uint8_ts
  // - decompress block contents

  uint8_t* compress_buffer = NULL;
  const uint8_t* p = NULL;
  uint8_t* d = NULL;
  sha1::SHA1 s;

  // Decrypt (if needed).
  bool free_input = false;
  const uint8_t* input_buffer = exe_buffer;
  size_t input_size = exe_length;

  switch (opt_file_format_info()->encryption_type) {
    case XEX_ENCRYPTION_NONE:
      // No-op.
      break;
    case XEX_ENCRYPTION_NORMAL:
      // TODO: a way to do without a copy/alloc?
      free_input = true;
      input_buffer = (const uint8_t*)calloc(1, exe_length);
      aes_decrypt_buffer(session_key_, exe_buffer, exe_length, (uint8_t*)input_buffer, exe_length);
      break;
    default:
      assert_always();
      return 1;
  }

  const auto* compression_info = &opt_file_format_info()->compression_info;
  const xex2_compressed_block_info* cur_block = &compression_info->normal.first_block;

  compress_buffer = (uint8_t*)calloc(1, exe_length);

  p = input_buffer;
  d = compress_buffer;

  // De-block.
  int result_code = 0;

  uint8_t block_calced_digest[0x14];
  while (cur_block->block_size) {
    const uint8_t* pnext = p + cur_block->block_size;
    const auto* next_block = (const xex2_compressed_block_info*)p;

    // Compare block hash, if no match we probably used wrong decrypt key
    s.reset();
    s.processBytes(p, cur_block->block_size);
    s.finalize(block_calced_digest);
    if (memcmp(block_calced_digest, cur_block->block_hash, 0x14) != 0) {
      result_code = 2;
      break;
    }

    // skip block info
    p += 4;
    p += 20;

    while (true) {
      const size_t chunk_size = (p[0] << 8) | p[1];
      p += 2;
      if (!chunk_size) {
        break;
      }

      memcpy(d, p, chunk_size);
      p += chunk_size;
      d += chunk_size;
    }

    p = pnext;
    cur_block = next_block;
  }

  if (!result_code) {
    uint32_t uncompressed_size = image_size();

    // Allocate in-place the XEX memory.
    bool alloc_result =
        memory()
            ->LookupHeap(base_address_)
            ->AllocFixed(
                base_address_, uncompressed_size, 4096,
                rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite);

    if (alloc_result) {
      uint8_t* buffer = memory()->TranslateVirtual(base_address_);
      std::memset(buffer, 0, uncompressed_size);

      // Decompress into XEX base
      result_code = lzx_decompress(compress_buffer, d - compress_buffer, buffer, uncompressed_size,
                                   compression_info->normal.window_size, nullptr, 0);
    } else {
      REXLOG_ERROR("Unable to allocate XEX memory at {:08X}-{:08X}.", base_address_,
                   uncompressed_size);
      result_code = 3;
    }
  }

  if (compress_buffer) {
    free((void*)compress_buffer);
  }
  if (free_input) {
    free((void*)input_buffer);
  }
  return result_code;
}

int XexModule::ReadPEHeaders() {
  const uint8_t* p = memory()->TranslateVirtual(base_address_);

  // Verify DOS signature (MZ).
  auto doshdr = reinterpret_cast<const IMAGE_DOS_HEADER*>(p);
  if (doshdr->e_magic != IMAGE_DOS_SIGNATURE) {
    REXLOG_ERROR("PE signature mismatch; likely bad decryption/decompression");
    return 1;
  }

  // Move to the NT header offset from the DOS header.
  p += doshdr->e_lfanew;

  // Verify NT signature (PE\0\0).
  auto nthdr = reinterpret_cast<const IMAGE_NT_HEADERS32*>(p);
  if (nthdr->Signature != IMAGE_NT_SIGNATURE) {
    return 1;
  }

  // Verify matches an Xbox PE.
  const IMAGE_FILE_HEADER* filehdr = &nthdr->FileHeader;
  if ((filehdr->Machine != IMAGE_FILE_MACHINE_POWERPCBE) ||
      !(filehdr->Characteristics & IMAGE_FILE_32BIT_MACHINE)) {
    return 1;
  }
  // Verify the expected size.
  if (filehdr->SizeOfOptionalHeader != IMAGE_SIZEOF_NT_OPTIONAL_HEADER) {
    return 1;
  }

  // Verify optional header is 32bit.
  const IMAGE_OPTIONAL_HEADER32* opthdr = &nthdr->OptionalHeader;
  if (opthdr->Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
    return 1;
  }
  // Verify subsystem.
  if (opthdr->Subsystem != IMAGE_SUBSYSTEM_XBOX) {
    return 1;
  }

// Linker version - likely 8+
// Could be useful for recognizing certain patterns
// opthdr->MajorLinkerVersion; opthdr->MinorLinkerVersion;

// Data directories of interest:
// EXPORT           IMAGE_EXPORT_DIRECTORY
// IMPORT           IMAGE_IMPORT_DESCRIPTOR[]
// EXCEPTION        IMAGE_CE_RUNTIME_FUNCTION_ENTRY[]
// BASERELOC
// DEBUG            IMAGE_DEBUG_DIRECTORY[]
// ARCHITECTURE     /IMAGE_ARCHITECTURE_HEADER/ ----- import thunks!
// TLS              IMAGE_TLS_DIRECTORY
// IAT              Import Address Table ptr
// opthdr->DataDirectory[IMAGE_DIRECTORY_ENTRY_X].VirtualAddress / .Size

// The macros in pe_image.h don't work with clang, for some reason.
// offsetof seems to be unable to find OptionalHeader.
#define offsetof1(type, member) ((std::size_t)&(((type*)0)->member))
#define IMAGE_FIRST_SECTION1(ntheader)                                                        \
  ((PIMAGE_SECTION_HEADER)((uint8_t*)ntheader + offsetof1(IMAGE_NT_HEADERS, OptionalHeader) + \
                           ((PIMAGE_NT_HEADERS)(ntheader))->FileHeader.SizeOfOptionalHeader))

  // Quick scan to determine bounds of sections.
  size_t upper_address = 0;
  const IMAGE_SECTION_HEADER* sechdr = IMAGE_FIRST_SECTION1(nthdr);
  for (size_t n = 0; n < filehdr->NumberOfSections; n++, sechdr++) {
    const size_t physical_address = opthdr->ImageBase + sechdr->VirtualAddress;
    upper_address = std::max(upper_address, physical_address + sechdr->Misc.VirtualSize);
  }

  // Setup/load sections.
  sechdr = IMAGE_FIRST_SECTION1(nthdr);
  for (size_t n = 0; n < filehdr->NumberOfSections; n++, sechdr++) {
    PESection section;
    memcpy(section.name, sechdr->Name, sizeof(sechdr->Name));
    section.name[8] = 0;
    section.raw_address = sechdr->PointerToRawData;
    section.raw_size = sechdr->SizeOfRawData;
    section.address = base_address_ + sechdr->VirtualAddress;
    section.size = sechdr->Misc.VirtualSize;
    section.flags = sechdr->Characteristics;
    pe_sections_.push_back(section);
  }

  // Extract Exception DataDirectory (PDATA table location)
  // This is the authoritative source for PDATA location, not the .pdata section header.
  // The .pdata section's VirtualAddress may differ from the DataDirectory entry.
  // NOTE: PE headers are little-endian (PE spec), no byte-swap needed.
  exception_dir_rva_ = opthdr->DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].VirtualAddress;
  exception_dir_size_ = opthdr->DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].Size;

  // DumpTLSDirectory(pImageBase, pNTHeader, (PIMAGE_TLS_DIRECTORY32)0);
  // DumpExportsSection(pImageBase, pNTHeader);
  return 0;
}

void XexModule::ReadSecurityInfo() {
  if (xex_format_ == kFormatXex1) {
    const xex1_security_info* xex1_sec_info =
        reinterpret_cast<const xex1_security_info*>(GetSecurityInfo(xex_header()));

    security_info_.rsa_signature = xex1_sec_info->rsa_signature;
    security_info_.aes_key = xex1_sec_info->aes_key;
    security_info_.image_size = xex1_sec_info->image_size;
    security_info_.image_flags = xex1_sec_info->image_flags;
    security_info_.export_table = xex1_sec_info->export_table;
    security_info_.load_address = xex1_sec_info->load_address;
    security_info_.page_descriptor_count = xex1_sec_info->page_descriptor_count;
    security_info_.page_descriptors = xex1_sec_info->page_descriptors;
  } else if (xex_format_ == kFormatXex2) {
    const xex2_security_info* xex2_sec_info =
        reinterpret_cast<const xex2_security_info*>(GetSecurityInfo(xex_header()));

    security_info_.rsa_signature = xex2_sec_info->rsa_signature;
    security_info_.aes_key = xex2_sec_info->aes_key;
    security_info_.image_size = xex2_sec_info->image_size;
    security_info_.image_flags = xex2_sec_info->image_flags;
    security_info_.export_table = xex2_sec_info->export_table;
    security_info_.load_address = xex2_sec_info->load_address;
    security_info_.page_descriptor_count = xex2_sec_info->page_descriptor_count;
    security_info_.page_descriptors = xex2_sec_info->page_descriptors;
  }
}

bool XexModule::Load(const std::string_view name, const std::string_view path, const void* xex_addr,
                     size_t xex_length) {
  auto src_header = reinterpret_cast<const xex2_header*>(xex_addr);

  if (src_header->magic == kXEX1Signature) {
    xex_format_ = kFormatXex1;
  } else if (src_header->magic == kXEX2Signature) {
    xex_format_ = kFormatXex2;
  } else {
    return false;
  }

  assert_false(loaded_);
  loaded_ = true;

  // Read in XEX headers
  xex_header_mem_.resize(src_header->header_size);
  std::memcpy(xex_header_mem_.data(), src_header, src_header->header_size);

  // Read/convert XEX1/XEX2 security info to a common format
  ReadSecurityInfo();

  auto sec_header = xex_security_info();

  // Try setting our base_address based on XEX_HEADER_IMAGE_BASE_ADDRESS, fall
  // back to xex_security_info otherwise
  base_address_ = xex_security_info()->load_address;
  rex::be<uint32_t>* base_addr_opt = nullptr;
  if (GetOptHeader(XEX_HEADER_IMAGE_BASE_ADDRESS, &base_addr_opt))
    base_address_ = *base_addr_opt;

  // Setup debug info.
  name_ = name;
  path_ = path;

  uint8_t* data = memory()->TranslateVirtual(base_address_);

  // Load in the XEX basefile
  // We'll try using both XEX2 keys to see if any give a valid PE
  int result_code = ReadImage(xex_addr, xex_length, false);
  if (result_code) {
    REXLOG_WARN("XEX load failed with code {}, trying with devkit encryption key...", result_code);

    result_code = ReadImage(xex_addr, xex_length, true);
    if (result_code) {
      REXLOG_ERROR("XEX load failed with code {}, tried both encryption keys", result_code);
      return false;
    }
  }

  // Note: caller will have to call LoadContinue once it's determined whether a
  // patch file exists or not!
  return true;
}

bool XexModule::LoadContinue() {
  // Second part of image load
  // Split from Load() so that we can patch the XEX before loading this data
  assert_false(finished_load_);
  if (finished_load_) {
    return true;
  }

  finished_load_ = true;

  if (ReadPEHeaders()) {
    REXLOG_ERROR("Failed to load XEX PE headers!");
    return false;
  }

  // Parse any "unsafe" headers into safer variants
  xex2_opt_generic_u32* alternate_titleids;
  if (GetOptHeader(xex2_header_keys::XEX_HEADER_ALTERNATE_TITLE_IDS, &alternate_titleids)) {
    auto count = alternate_titleids->count();
    for (uint32_t i = 0; i < count; i++) {
      opt_alternate_title_ids_.push_back(alternate_titleids->values[i]);
    }
  }

  // Scan and find the low/high addresses.
  // All code sections are continuous, so this should be easy.
  auto heap = memory()->LookupHeap(base_address_);
  auto page_size = heap->page_size();

  low_address_ = UINT_MAX;
  high_address_ = 0;

  auto sec_header = xex_security_info();
  for (uint32_t i = 0, page = 0; i < sec_header->page_descriptor_count; i++) {
    // Byteswap the bitfield manually.
    xex2_page_descriptor desc;
    desc.value = rex::byte_swap(sec_header->page_descriptors[i].value);

    const auto start_address = base_address_ + (page * page_size);
    const auto end_address = start_address + (desc.page_count * page_size);
    if (desc.info == XEX_SECTION_CODE) {
      low_address_ = std::min(low_address_, start_address);
      high_address_ = std::max(high_address_, end_address);
    }

    page += desc.page_count;
  }

  // NOTE(tomc): Backend notification not needed - no JIT in rexglue
  // processor_->backend()->CommitExecutableRange(low_address_, high_address_);

  // Add all imports (variables/functions).
  xex2_opt_import_libraries* opt_import_libraries = nullptr;
  GetOptHeader(XEX_HEADER_IMPORT_LIBRARIES, &opt_import_libraries);

  if (opt_import_libraries) {
    // FIXME: Don't know if 32 is the actual limit, but haven't seen more than
    // 2.
    const char* string_table[32];
    std::memset(string_table, 0, sizeof(string_table));

    // Parse the string table
    for (size_t i = 0, o = 0; i < opt_import_libraries->string_table.size &&
                              o < opt_import_libraries->string_table.count;
         ++o) {
      assert_true(o < rex::countof(string_table));
      const char* str = &opt_import_libraries->string_table.data[i];

      string_table[o] = str;
      i += std::strlen(str) + 1;

      // Padding
      if ((i % 4) != 0) {
        i += 4 - (i % 4);
      }
    }

    auto library_data = reinterpret_cast<uint8_t*>(opt_import_libraries);
    uint32_t library_offset = opt_import_libraries->string_table.size + 12;
    while (library_offset < opt_import_libraries->size) {
      auto library = reinterpret_cast<xex2_import_library*>(library_data + library_offset);
      if (!library->size) {
        break;
      }
      size_t library_name_index = library->name_index & 0xFF;
      assert_true(library_name_index < opt_import_libraries->string_table.count);
      assert_not_null(string_table[library_name_index]);
      auto library_name = std::string(string_table[library_name_index]);
      SetupLibraryImports(library_name, library);
      library_offset += library->size;
    }
  }

  // Setup memory protection.
  for (uint32_t i = 0, page = 0; i < sec_header->page_descriptor_count; i++) {
    // Byteswap the bitfield manually.
    xex2_page_descriptor desc;
    desc.value = rex::byte_swap(sec_header->page_descriptors[i].value);

    auto address = base_address_ + (page * page_size);
    auto size = desc.page_count * page_size;
    switch (desc.info) {
      case XEX_SECTION_CODE:
      case XEX_SECTION_READONLY_DATA:
        heap->Protect(address, size, memory::kMemoryProtectRead);
        break;
      case XEX_SECTION_DATA:
        heap->Protect(address, size, memory::kMemoryProtectRead | memory::kMemoryProtectWrite);
        break;
    }

    page += desc.page_count;
  }

  // Populate binary introspection data
  PopulateBinaryData();

  return true;
}

bool XexModule::Unload() {
  if (!loaded_) {
    return true;
  }
  loaded_ = false;

  // If this isn't a patch, just deallocate the memory occupied by the exe
  if (!is_patch()) {
    assert_not_zero(base_address_);

    memory()->LookupHeap(base_address_)->Release(base_address_);
  }

  xex_header_mem_.resize(0);

  return true;
}

bool XexModule::SetupLibraryImports(const std::string_view name,
                                    const xex2_import_library* library) {
  // NOTE(tomc): import resolution is done at compile time.
  // however, we still need to patch variable imports in guest memory
  // since they are accessed via memory loads, not function calls.

  auto base_name = rex::string::utf8_find_base_name_from_guest_path(name);

  // Get export resolver for variable import patching
  auto* export_resolver = kernel_state_->emulator()->export_resolver();

  ImportLibrary library_info;
  library_info.name = base_name;
  library_info.id = library->id;
  library_info.version.value = library->version().value;
  library_info.min_version.value = library->version_min().value;

  // Use a map to properly pair type 0 (variable) and type 1 (thunk) records by ordinal.
  // Import table entries alternate: type 0 has ordinal info, type 1 has thunk address.
  // They may not come in immediate succession, so we pair by ordinal.
  std::unordered_map<uint16_t, ImportLibraryFn> import_map;

  for (uint32_t i = 0; i < library->count; i++) {
    uint32_t record_addr = library->import_table[i];
    if (!record_addr)
      continue;

    auto record_slot = memory()->TranslateVirtual<rex::be<uint32_t>*>(record_addr);
    uint32_t record_value = *record_slot;

    uint16_t record_type = (record_value & 0xFF000000) >> 24;
    uint16_t ordinal = record_value & 0xFFFF;

    auto& import_info = import_map[ordinal];
    import_info.ordinal = ordinal;

    if (record_type == 0) {
      // Variable import - value_address is where the variable value is stored
      import_info.value_address = record_addr;

      // Patch variable imports in guest memory with the actual address
      if (export_resolver) {
        auto kernel_export = export_resolver->GetExportByOrdinal(base_name, ordinal);
        if (kernel_export && kernel_export->type == runtime::Export::Type::kVariable) {
          if (kernel_export->is_implemented() && kernel_export->variable_ptr) {
            // Write the variable address to guest memory
            *record_slot = kernel_export->variable_ptr;
            REXLOG_DEBUG("Patched variable import {}:{:#x} ({}) -> {:#x}", base_name, ordinal,
                         kernel_export->name, kernel_export->variable_ptr);
          } else {
            // write garbage value if we don't have it implemented
            *record_slot = 0xD000BEEF | (kernel_export->ordinal & 0xFFF) << 16;
            REXLOG_WARN("Variable import {}:{:#x} ({}) not implemented", base_name, ordinal,
                        kernel_export->name);
          }
        }
      }
    } else if (record_type == 1) {
      // Thunk import - thunk_address is the function pointer location
      // This is the address we need for function table registration
      import_info.thunk_address = record_addr;
    }
  }

  // Convert map to vector (sorted by ordinal for consistent output)
  std::vector<uint16_t> ordinals;
  ordinals.reserve(import_map.size());
  for (const auto& [ordinal, _] : import_map) {
    ordinals.push_back(ordinal);
  }
  std::sort(ordinals.begin(), ordinals.end());

  for (uint16_t ordinal : ordinals) {
    library_info.imports.push_back(import_map[ordinal]);
  }

  import_libs_.push_back(library_info);
  REXLOG_DEBUG("created symbols for import library {} with {} imports", base_name,
               library_info.imports.size());

  return true;
}

bool XexModule::ContainsAddress(uint32_t address) {
  return address >= low_address_ && address < high_address_;
}

std::unique_ptr<Function> XexModule::CreateFunction(uint32_t address) {
  // NOTE(tomc): this used to be a JIT function. now a cousin of it lives in the codegen lib.
  (void)address;
  return nullptr;
}

bool XexModule::FindSaveRest() {
  // NOTE(tomc): see XexModule::CreateFunction
  return true;
}

// Binary introspection implementation
void XexModule::PopulateBinaryData() {
  binary_sections_.clear();
  binary_symbols_.clear();

  // Populate sections from existing PE sections
  for (const auto& pe_sec : pe_sections_) {
    BinarySection sec;
    sec.name = pe_sec.name;
    sec.virtual_address = pe_sec.address;
    sec.virtual_size = pe_sec.size;
    sec.host_data = memory()->TranslateVirtual<uint8_t*>(pe_sec.address);
    sec.executable = (pe_sec.flags & kXEPESectionMemoryExecute) != 0;
    sec.writable = (pe_sec.flags & kXEPESectionMemoryWrite) != 0;
    binary_sections_.push_back(std::move(sec));
  }

  // Populate symbols from import libraries
  for (const auto& lib : import_libs_) {
    for (const auto& import : lib.imports) {
      BinarySymbol sym;
      sym.name = fmt::format("{}@{}", lib.name, import.ordinal);
      sym.address = import.thunk_address;
      sym.size = 16;  // Thunk size: 2 ordinal words + mtctr + bctr
      sym.type = BinarySymbolType::Import;
      binary_symbols_.push_back(std::move(sym));
    }
  }
}

std::span<const BinarySection> XexModule::binary_sections() const {
  return binary_sections_;
}

const BinarySection* XexModule::FindSectionByName(std::string_view name) const {
  for (const auto& sec : binary_sections_) {
    if (sec.name == name)
      return &sec;
  }
  return nullptr;
}

std::span<const BinarySymbol> XexModule::binary_symbols() const {
  return binary_symbols_;
}

}  // namespace rex::runtime
