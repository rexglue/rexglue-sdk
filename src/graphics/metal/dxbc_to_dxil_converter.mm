/**
 ******************************************************************************
 * ReXGlue : Xbox 360 Static Recompilation Runtime
 ******************************************************************************
 * Copyright 2026 Tom Clay. All rights reserved.
 * Released under the BSD license - see LICENSE in the root for more details.
 ******************************************************************************
 *
 * DXBC to DXIL converter implementation.
 *
 * On macOS, the wmarti/DirectXShaderCompiler fork always compiles with
 * __EMULATE_UUID (see WinAdapter.h:#ifdef __APPLE__). In that mode:
 *
 *   typedef const void* REFIID;
 *   #define IsEqualIID(a, b)  a == b          // pointer comparison!
 *   #define __uuidof(T)       T::uuidof()     // returns UuidStrHash("T")
 *
 * This means the library never sees real GUID structs for IID comparisons —
 * it uses the string-hash of the type name cast to a pointer.  Passing a
 * real &IID_IUnknown GUID struct as the REFIID will always fail because
 * the pointer address != UuidStrHash("IUnknown").
 *
 * Fix: compute the same hash the library expects and pass it directly as the
 * REFIID.  Since IDxbcConverter has no DECLARE_CROSS_PLATFORM_UUIDOF macro,
 * it inherits IUnknown::uuidof() == UuidStrHash("IUnknown") == 0xc52523e.
 * We therefore request IUnknown and cast the result straight to IDxbcConverter*
 * (valid because DxbcConverter : IDxbcConverter : IUnknown, vtable is the same).
 */

#include <rex/graphics/metal/dxbc_to_dxil_converter.h>
#include <rex/graphics/metal/DxbcConverter.h>
#include <rex/logging.h>

#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <fstream>

namespace rex::graphics::metal {

// Default extra options passed to the converter.
static const wchar_t kDefaultExtraOptions[] = L"-skip-container-parts";

// Resolved extra options (may be overridden by REX_DXBC2DXIL_FLAGS env var).
static const wchar_t* resolved_extra_options_ = kDefaultExtraOptions;
static std::wstring custom_extra_options_storage_;

// Search paths for the dxilconv dynamic library.
static const char* const kDxilconvSearchPaths[] = {
    "libdxilconv.dylib",
    "/usr/local/lib/libdxilconv.dylib",
    "/opt/homebrew/lib/libdxilconv.dylib",
    nullptr,
};

// ============================================================================
// __EMULATE_UUID hash helper
// ============================================================================
// Replicates WinAdapter.cpp::UuidStrHash exactly so we can derive the REFIID
// values the library expects without including DXC's headers.

namespace {

constexpr size_t DxcUuidStrHash(const char* k) {
  long h = 0;
  while (*k) {
    h = (h << 4) + static_cast<long>(*k++);
    long g = h & 0xF0000000L;
    if (g != 0) h ^= g >> 24;
    h &= ~g;
  }
  return static_cast<size_t>(h);
}

// The REFIID the library expects for IUnknown on macOS.
// IDxbcConverter inherits IUnknown::uuidof() because it has no
// DECLARE_CROSS_PLATFORM_UUIDOF, so this hash serves for both.
static const void* const kDxcRefiidIUnknown =
    reinterpret_cast<const void*>(DxcUuidStrHash("IUnknown"));

}  // namespace

// ============================================================================
// Per-thread converter instance
// ============================================================================

namespace {

struct ThreadConverterState {
  IDxbcConverter* converter = nullptr;
  ~ThreadConverterState() {
    if (converter) {
      converter->Release();
      converter = nullptr;
    }
  }
};

std::string HResultHex(HRESULT hr) {
  char buf[11];
  std::snprintf(buf, sizeof(buf), "%08X", static_cast<unsigned>(hr));
  return std::string(buf);
}

}  // namespace

// ============================================================================
// DxbcToDxilConverter
// ============================================================================

DxbcToDxilConverter::DxbcToDxilConverter() = default;

DxbcToDxilConverter::~DxbcToDxilConverter() {
  // Intentionally do NOT dlclose the library.
  // Thread-local ThreadConverterState destructors call into the library.
  // If we dlclose here and a thread exits later, the destructor will jump
  // into unmapped memory → segfault.
  // Leaking the handle is acceptable for a process-lifetime singleton.
  dxilconv_lib_ = nullptr;
}

bool DxbcToDxilConverter::Initialize() {
  // Try each search path until we find the library.
  for (const char* const* path = kDxilconvSearchPaths; *path; ++path) {
    dxilconv_lib_ = dlopen(*path, RTLD_LAZY);
    if (dxilconv_lib_) {
      REXGPU_INFO("DxbcToDxilConverter: Loaded dxilconv from {}", *path);
      break;
    }
  }

  if (!dxilconv_lib_) {
    REXGPU_WARN(
        "DxbcToDxilConverter: libdxilconv.dylib not found. "
        "DXBC\u2192DXIL conversion will not be available. "
        "Install libdxilconv.dylib (from the wmarti/DirectXShaderCompiler "
        "macOS fork) to enable.");
    is_available_ = false;
    return false;
  }

  // Resolve DxcCreateInstance symbol.
  create_instance_fn_ = reinterpret_cast<DxcCreateInstanceFn>(
      dlsym(dxilconv_lib_, "DxcCreateInstance"));
  if (!create_instance_fn_) {
    REXGPU_ERROR(
        "DxbcToDxilConverter: DxcCreateInstance symbol not found in library");
    dlclose(dxilconv_lib_);
    dxilconv_lib_ = nullptr;
    is_available_ = false;
    return false;
  }

  // Probe: create a converter instance using the hash-based REFIID.
  //
  // On macOS __EMULATE_UUID mode, DxcCreateInstance's internal QueryInterface
  // compares the REFIID argument using pointer equality against
  // __uuidof(IUnknown) == UuidStrHash("IUnknown").  We must pass the hash
  // value cast to const void* — NOT a pointer to a GUID struct.
  //
  // CLSID_DxbcConverter is a real GUID struct checked with memcmp by the
  // CLSID dispatcher, so we still pass its address normally.
  void* raw = nullptr;
  HRESULT hr = create_instance_fn_(
      &CLSID_DxbcConverter,
      kDxcRefiidIUnknown,
      &raw);

  if (hr != S_OK || !raw) {
    REXGPU_ERROR(
        "DxbcToDxilConverter: DxcCreateInstance failed (hr=0x{:08X}). "
        "DXBC\u2192DXIL conversion unavailable.",
        static_cast<unsigned>(hr));
    dlclose(dxilconv_lib_);
    dxilconv_lib_ = nullptr;
    is_available_ = false;
    return false;
  }

  // Cast directly — no QueryInterface needed.  The object pointer IS the
  // IDxbcConverter vtable since DxbcConverter's first base is IDxbcConverter.
  IDxbcConverter* test_converter = static_cast<IDxbcConverter*>(raw);
  test_converter->Release();

  REXGPU_INFO("DxbcToDxilConverter: IDxbcConverter interface verified OK");

  // Check for env var override of extra options.
  const char* flags_override = std::getenv("REX_DXBC2DXIL_FLAGS");
  if (flags_override && flags_override[0] != '\0') {
    // Widen ASCII to wchar_t.
    custom_extra_options_storage_.clear();
    for (const char* p = flags_override; *p; ++p) {
      custom_extra_options_storage_ += static_cast<wchar_t>(*p);
    }
    resolved_extra_options_ = custom_extra_options_storage_.c_str();
    REXGPU_INFO("DxbcToDxilConverter: Using custom extra options from "
                "REX_DXBC2DXIL_FLAGS: {}", flags_override);
  }

  is_available_ = true;
  REXGPU_INFO("DxbcToDxilConverter: Initialized successfully");
  return true;
}

IDxbcConverter* DxbcToDxilConverter::GetThreadConverter(
    std::string* error_message) {
  static thread_local ThreadConverterState thread_state;
  if (thread_state.converter) {
    return thread_state.converter;
  }

  if (!create_instance_fn_) {
    if (error_message) {
      *error_message = "DxcCreateInstance not available";
    }
    return nullptr;
  }

  void* raw = nullptr;
  HRESULT hr = create_instance_fn_(
      &CLSID_DxbcConverter,
      kDxcRefiidIUnknown,
      &raw);

  if (hr != S_OK || !raw) {
    if (error_message) {
      *error_message =
          "DxcCreateInstance failed (HRESULT 0x" + HResultHex(hr) + ")";
    }
    return nullptr;
  }

  // Direct cast — see comment in Initialize().
  thread_state.converter = static_cast<IDxbcConverter*>(raw);
  return thread_state.converter;
}

bool DxbcToDxilConverter::Convert(const std::vector<uint8_t>& dxbc_data,
                                  std::vector<uint8_t>& dxil_data_out,
                                  std::string* error_message) {
  if (!is_available_) {
    if (error_message) {
      *error_message =
          "DxbcToDxilConverter not initialized or dxilconv unavailable";
    }
    return false;
  }

  // Validate DXBC header magic.
  if (dxbc_data.size() < 4 || dxbc_data[0] != 'D' || dxbc_data[1] != 'X' ||
      dxbc_data[2] != 'B' || dxbc_data[3] != 'C') {
    if (error_message) {
      *error_message = "Invalid DXBC data - missing DXBC magic header";
    }
    return false;
  }

  // Debug output directories from environment.
  const char* dxbc_dir = std::getenv("REX_DXBC_OUTPUT_DIR");
  const char* dxil_dir = std::getenv("REX_DXIL_OUTPUT_DIR");

  std::string shader_id;
  if (dxbc_dir || dxil_dir) {
    // Generate a unique shader ID for debug dumps.
    uint64_t hash = 0;
    for (size_t i = 0; i < dxbc_data.size(); ++i) {
      hash = hash * 31 + dxbc_data[i];
    }
    shader_id = std::to_string(hash) + "_" + std::to_string(getpid());
  }

  if (dxbc_dir) {
    std::string debug_path =
        std::string(dxbc_dir) + "/shader_" + shader_id + ".dxbc";
    WriteDebugFile(debug_path, dxbc_data);
  }

  // Get or create per-thread converter.
  IDxbcConverter* converter = GetThreadConverter(error_message);
  if (!converter) {
    return false;
  }

  // Perform the conversion using the official interface.
  void* dxil_ptr = nullptr;
  UINT32 dxil_size = 0;
  wchar_t* diag = nullptr;

  HRESULT hr = converter->Convert(
      dxbc_data.data(), static_cast<UINT32>(dxbc_data.size()),
      resolved_extra_options_, &dxil_ptr, &dxil_size, &diag);

  if (hr != S_OK || dxil_ptr == nullptr || dxil_size == 0) {
    if (error_message) {
      if (diag) {
        std::string diag_utf8;
        std::mbstate_t state{};
        const wchar_t* src = diag;
        size_t len = std::wcsrtombs(nullptr, &src, 0, &state);
        if (len != static_cast<size_t>(-1)) {
          diag_utf8.resize(len);
          src = diag;
          state = std::mbstate_t{};
          std::wcsrtombs(diag_utf8.data(), &src, len + 1, &state);
        } else {
          diag_utf8 = "<non-convertible diagnostic>";
        }
        *error_message = "dxbc2dxil failed: " + diag_utf8;
      } else {
        *error_message = "dxbc2dxil failed with HRESULT 0x" + HResultHex(hr);
      }
    }
    // CoTaskMemFree is mapped to free() in wmarti fork's WinAdapter.h
    free(diag);
    free(dxil_ptr);
    return false;
  }

  dxil_data_out.assign(reinterpret_cast<const uint8_t*>(dxil_ptr),
                       reinterpret_cast<const uint8_t*>(dxil_ptr) + dxil_size);

  // CoTaskMemFree -> free() mapping
  free(diag);
  free(dxil_ptr);

  if (dxil_dir) {
    std::string debug_path =
        std::string(dxil_dir) + "/shader_" + shader_id + ".dxil";
    WriteDebugFile(debug_path, dxil_data_out);
  }

  // Validate output.
  if (dxil_data_out.size() < 4) {
    dxil_data_out.clear();
    if (error_message) {
      *error_message = "Output DXIL blob too small";
    }
    return false;
  }

  REXGPU_DEBUG(
      "DxbcToDxilConverter: Converted {} bytes DXBC -> {} bytes DXIL",
      dxbc_data.size(), dxil_data_out.size());

  return true;
}

bool DxbcToDxilConverter::WriteDebugFile(const std::string& path,
                                         const std::vector<uint8_t>& data) {
  std::ofstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  file.write(reinterpret_cast<const char*>(data.data()), data.size());
  return file.good();
}

}  // namespace rex::graphics::metal
