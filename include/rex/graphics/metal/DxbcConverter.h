/**
 * Minimal DxbcConverter.h for ReXGlue
 * Defines the official IDxbcConverter interface without full DXC dependencies
 */

#pragma once

#include <cstdint>

// Minimal Windows types
using HRESULT = int32_t;
using UINT32 = uint32_t;
using LPCVOID = const void*;
using LPVOID = void*;
using LPCWSTR = const wchar_t*;
using LPWSTR = wchar_t*;

constexpr HRESULT S_OK = 0;

struct GUID {
  uint32_t Data1;
  uint16_t Data2;
  uint16_t Data3;
  uint8_t Data4[8];
};

using REFCLSID = const GUID*;
using REFIID = const GUID*;

// IUnknown interface
// IMPORTANT: virtual ~IUnknown() must be declared here to match the vtable
// layout in wmarti/DirectXShaderCompiler's WinAdapter.h IUnknown (which has
// a virtual destructor at slot 3).  The Itanium ABI emits two destructor
// entries (D1 + D2), shifting Convert to vtable slot 5.  Without this,
// ReXGlue dispatches Convert via slot 3 which aliases the destructor — giving
// garbage HRESULTs like 0xEF0BEF80 / 0xF702CA00.
struct IUnknown {
  virtual HRESULT QueryInterface(REFIID riid, void** ppvObject) = 0;
  virtual uint32_t AddRef() = 0;
  virtual uint32_t Release() = 0;
  virtual ~IUnknown() {}
};

// IDxbcConverter interface — vtable must match DXC's DxbcConverter.h exactly.
// Library vtable order after IUnknown (slots 0-4):
//   slot 5: Convert
//   slot 6: ConvertInDriver  (declared here even though we never call it)
struct IDxbcConverter : public IUnknown {
  virtual HRESULT Convert(
    LPCVOID pDxbc,
    UINT32 DxbcSize,
    LPCWSTR pExtraOptions,
    LPVOID* ppDxil,
    UINT32* pDxilSize,
    LPWSTR* ppDiag) = 0;

  // Slot 6 — not used by ReXGlue but must be present so the vtable size matches.
  virtual HRESULT ConvertInDriver(
    const UINT32* pBytecode,
    LPCVOID pInputSignature, UINT32 NumInputSignatureElements,
    LPCVOID pOutputSignature, UINT32 NumOutputSignatureElements,
    LPCVOID pPatchConstantSignature, UINT32 NumPatchConstantSignatureElements,
    LPCWSTR pExtraOptions,
    void** ppDxilModule,
    LPWSTR* ppDiag) = 0;
};

// CLSID for IDxbcConverter
static const GUID CLSID_DxbcConverter = {
    0x4900391e,
    0xb752,
    0x4edd,
    {0xa8, 0x85, 0x6f, 0xb7, 0x6e, 0x25, 0xad, 0xdb}};

// __uuidof emulation for macOS
template<typename T> struct __uuidof_helper;
template<> struct __uuidof_helper<IDxbcConverter> {
  static constexpr GUID value = {
      0x5F956ED5,
      0x78D1,
      0x4B15,
      {0x82, 0x47, 0xF7, 0x18, 0x76, 0x14, 0xA0, 0x41}};
};

#define __uuidof(T) (&__uuidof_helper<T>::value)

// DxcCreateInstance function pointer type
extern "C" HRESULT DxcCreateInstance(
  REFCLSID rclsid,
  REFIID riid,
  LPVOID* ppv);
