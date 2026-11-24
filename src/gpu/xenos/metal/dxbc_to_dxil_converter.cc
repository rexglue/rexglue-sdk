/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// DXC ships Windows COM declarations; WinAdapter supplies them on Apple, and it must come before anything
// that names HRESULT, REFIID or IMalloc.
#include "WinAdapter.h"
#include "dxcapi.h"
// The converter interface names two SAL annotations this SDK's WinAdapter does not carry.
#ifndef _Outptr_result_bytebuffer_maybenull_
#define _Outptr_result_bytebuffer_maybenull_(size)
#endif
#ifndef _Outptr_result_maybenull_z_
#define _Outptr_result_maybenull_z_
#endif
#include "rex/graphics/metal/dxbc_to_dxil_converter.h"

#include <cstdio>
#include <cstdlib>

#include <rex/logging/log_category.h>
#include "rex/graphics/metal/metal_converter_options.h"

// dxilconv (Microsoft DirectXShaderCompiler component). On non-Windows this
// pulls in the WinAdapter COM shim (__EMULATE_UUID, CoTaskMemFree=free, etc.).
#include "DxbcConverter.h"

// On the emulated-UUID path (non-Windows) libdxilconv does not export its COM
// shim symbols, so the consumer must supply them in exactly one translation
// unit: UuidStrHash (whose algorithm MUST match the one compiled into the dylib,
// WinAdapter.cpp, or __uuidof() values won't match the dylib's internal IIDs),
// the IUnknown uuid definition, and the CLSID value (declaration-only in the
// header for non-Windows).
#if !defined(_WIN32) && defined(__EMULATE_UUID)
size_t UuidStrHash(const char* k) {
  long h = 0;
  while (*k) {
    h = (h << 4) + *(k++);
    long g = h & 0xF0000000L;
    if (g != 0) h ^= g >> 24;
    h &= ~g;
  }
  return h;
}
DEFINE_CROSS_PLATFORM_UUIDOF(IUnknown)
#endif

namespace {
const CLSID kClsidDxbcConverter = {
    0x4900391e, 0xb752, 0x4edd, {0xa8, 0x85, 0x6f, 0xb7, 0x6e, 0x25, 0xad, 0xdb}};
}  // namespace

namespace rex {
namespace graphics {
namespace metal {

namespace {

std::wstring WidenAscii(const char* value) {
  std::wstring out;
  for (const char* p = value; p && *p; ++p) {
    out.push_back(static_cast<wchar_t>(*p));
  }
  return out;
}

std::string NarrowAscii(const wchar_t* value) {
  std::string out;
  for (const wchar_t* p = value; p && *p; ++p) {
    out.push_back(static_cast<char>(*p));
  }
  return out;
}

bool HasDxbcMagic(const std::vector<uint8_t>& data) {
  return data.size() >= 4 && data[0] == 'D' && data[1] == 'X' &&
         data[2] == 'B' && data[3] == 'C';
}

}  // namespace

bool DxbcToDxilConverter::Initialize(std::string* error_out) {
  // The effective option string comes from metal_converter_options.h, which
  // ComputeMetalPipelineStorageIdentity fingerprints. The two must read the
  // same source: the options change the emitted DXIL for byte-identical DXBC
  // (measured: 3644 B under the default vs 4436 B under an empty string for the
  // same quad-domain payload), so a durable cache written under one option
  // string must not be served under another.
  options_ = WidenAscii(MetalDxbcToDxilEffectiveOptions().c_str());

  // A create/release round-trip proves the library is present and the CLSID is
  // registered before any shader depends on it.
  IDxbcConverter* probe = nullptr;
  HRESULT hr = DxcCreateInstance(kClsidDxbcConverter, __uuidof(IDxbcConverter),
                                 reinterpret_cast<void**>(&probe));
  if (hr != S_OK || !probe) {
    is_available_ = false;
    if (error_out) {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "hr=0x%08X", static_cast<unsigned>(hr));
      *error_out = std::string("dxilconv unavailable (") + buf + ")";
    }
    REXLOG_ERROR("DxbcToDxilConverter: failed to create IDxbcConverter (hr=0x{:08X})",
           static_cast<unsigned>(hr));
    return false;
  }
  probe->Release();

  is_available_ = true;
  REXLOG_INFO("DxbcToDxilConverter: ready (options='{}')", NarrowAscii(options_.c_str()));
  return true;
}

bool DxbcToDxilConverter::Convert(const std::vector<uint8_t>& dxbc,
                                  std::vector<uint8_t>& dxil_out,
                                  std::string* error_out) {
  if (!is_available_) {
    if (error_out) {
      *error_out = "DxbcToDxilConverter not initialized";
    }
    return false;
  }
  if (!HasDxbcMagic(dxbc)) {
    if (error_out) {
      *error_out = "input is not a DXBC container (missing 'DXBC' magic)";
    }
    return false;
  }

  // A FRESH IDxbcConverter per conversion, never reused: a reused instance
  // carries call HISTORY into later conversions — measured as a 14-byte
  // bitcode delta, and, decisively, an adaptive-QUAD domain artifact
  // converted after an adaptive-TRIANGLE conversion on the same instance is
  // CORRUPT at runtime (run-to-run nondeterministic per-sample coverage in
  // Viva Pinata replay; renders-nothing in the synthetic quad-after-tri
  // conformance case). Root-caused 2026-08-02; fresh-per-convert proven to
  // cure both the determinism failures and byte order-dependence end to end.
  // Cost: one DxcCreateInstance per shader translation, off the frame path.
  //
  // DO NOT reintroduce instance reuse (thread-local, pooled, or otherwise) to
  // save that create. The invariant is pinned by the conformance case
  // "dxilconv and MSC conversions are order-independent"
  // (conformance/metal_tessellation_render_conformance_test.mm): it converts a
  // quad-domain payload standalone and again after a triangle conversion and
  // requires byte-identical output — reuse fails it. Callers that need to
  // amortize should cache the RESULT (the DXIL artifact cache in
  // adopt-xenios-shader-artifact-caching), never the converter.
  IDxbcConverter* converter = nullptr;
  HRESULT chr = DxcCreateInstance(kClsidDxbcConverter, __uuidof(IDxbcConverter),
                                  reinterpret_cast<void**>(&converter));
  if (chr != S_OK || !converter) {
    if (error_out) {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "hr=0x%08X", static_cast<unsigned>(chr));
      *error_out = std::string("failed to create converter (") + buf + ")";
    }
    return false;
  }
  struct FreshReleaser {
    IDxbcConverter* converter;
    ~FreshReleaser() {
      if (converter) converter->Release();
    }
  } fresh_releaser{converter};

  void* dxil_ptr = nullptr;
  UINT32 dxil_size = 0;
  wchar_t* diagnostics = nullptr;
  HRESULT hr = converter->Convert(
      dxbc.data(), static_cast<UINT32>(dxbc.size()),
      options_.empty() ? nullptr : options_.c_str(), &dxil_ptr, &dxil_size,
      &diagnostics);

  if (hr != S_OK || !dxil_ptr || dxil_size == 0) {
    if (error_out) {
      if (diagnostics) {
        *error_out = "dxilconv Convert failed: " + NarrowAscii(diagnostics);
      } else {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "hr=0x%08X", static_cast<unsigned>(hr));
        *error_out = std::string("dxilconv Convert failed (") + buf + ")";
      }
    }
    CoTaskMemFree(diagnostics);
    CoTaskMemFree(dxil_ptr);
    return false;
  }

  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(dxil_ptr);
  dxil_out.assign(bytes, bytes + dxil_size);
  CoTaskMemFree(diagnostics);
  CoTaskMemFree(dxil_ptr);
  return true;
}

}  // namespace metal
}  // namespace gpu
}  // namespace rex
