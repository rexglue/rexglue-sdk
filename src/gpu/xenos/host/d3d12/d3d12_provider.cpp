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

#include <cstdlib>

#include <rex/assert.h>
#include <rex/logging/log_category.h>
#include <rex/math.h>
#include "gpu/xenos/host/d3d12/d3d12_immediate_drawer.h"
#include "gpu/xenos/host/d3d12/d3d12_presenter.h"
#include "gpu/xenos/host/d3d12/d3d12_provider.h"

#include <malloc.h>

namespace rex::gpu::d3d12 {

bool D3D12Provider::IsD3D12APIAvailable() {
  HMODULE library_d3d12 = LoadLibraryW(L"D3D12.dll");
  if (!library_d3d12) {
    return false;
  }
  FreeLibrary(library_d3d12);
  return true;
}

std::unique_ptr<D3D12Provider> D3D12Provider::Create(i32 adapter_index,
                                                     const D3D12Settings& settings) {
  auto provider = std::make_unique<D3D12Provider>(Key{});
  provider->settings_ = &settings;
  if (!provider->Initialize(adapter_index)) {
    REXGPU_FATAL(
        "Unable to initialize Direct3D 12 graphics subsystem.\n"
        "\n"
        "Install the latest drivers for your GPU, which must support "
        "Direct3D 12 with the feature level of at least 11_0.");
  }
  return provider;
}

D3D12Provider::~D3D12Provider() {
  if (graphics_analysis_ != nullptr) {
    graphics_analysis_->Release();
  }
  if (direct_queue_ != nullptr) {
    direct_queue_->Release();
  }
  if (device_ != nullptr) {
    device_->Release();
  }
  if (dxgi_factory_ != nullptr) {
    dxgi_factory_->Release();
  }

  if (settings_->debug.Get() && pfn_dxgi_get_debug_interface1_) {
    Microsoft::WRL::ComPtr<IDXGIDebug> dxgi_debug;
    if (SUCCEEDED(pfn_dxgi_get_debug_interface1_(0, IID_PPV_ARGS(&dxgi_debug)))) {
      dxgi_debug->ReportLiveObjects(DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_ALL);
    }
  }

  if (library_dxcompiler_ != nullptr) {
    FreeLibrary(library_dxcompiler_);
  }
  if (library_dxilconv_ != nullptr) {
    FreeLibrary(library_dxilconv_);
  }
  if (library_d3dcompiler_ != nullptr) {
    FreeLibrary(library_d3dcompiler_);
  }
  if (library_d3d12_ != nullptr) {
    FreeLibrary(library_d3d12_);
  }
  if (library_dxgi_ != nullptr) {
    FreeLibrary(library_dxgi_);
  }
}

bool D3D12Provider::EnableIncreaseBasePriorityPrivilege() {
  TOKEN_PRIVILEGES privileges;
  privileges.PrivilegeCount = 1;
  privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  if (!LookupPrivilegeValue(nullptr, SE_INC_BASE_PRIORITY_NAME, &privileges.Privileges[0].Luid)) {
    return false;
  }
  HANDLE token;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &token)) {
    return false;
  }
  bool enabled =
      AdjustTokenPrivileges(token, FALSE, &privileges, sizeof(privileges), nullptr, nullptr) &&
      GetLastError() != ERROR_NOT_ALL_ASSIGNED;
  CloseHandle(token);
  return enabled;
}

bool D3D12Provider::Initialize(i32 adapter_index_wanted) {
  library_dxgi_ = LoadLibraryW(L"dxgi.dll");
  library_d3d12_ = LoadLibraryW(L"D3D12.dll");
  if (!library_dxgi_ || !library_d3d12_) {
    REXUI_FAIL(false, "Failed to load dxgi.dll or D3D12.dll");
  }
  bool libraries_loaded = true;
  libraries_loaded &= (pfn_create_dxgi_factory2_ = reinterpret_cast<PFNCreateDXGIFactory2>(
                           GetProcAddress(library_dxgi_, "CreateDXGIFactory2"))) != nullptr;
  libraries_loaded &= (pfn_dxgi_get_debug_interface1_ = reinterpret_cast<PFNDXGIGetDebugInterface1>(
                           GetProcAddress(library_dxgi_, "DXGIGetDebugInterface1"))) != nullptr;
  libraries_loaded &=
      (pfn_d3d12_get_debug_interface_ = reinterpret_cast<PFN_D3D12_GET_DEBUG_INTERFACE>(
           GetProcAddress(library_d3d12_, "D3D12GetDebugInterface"))) != nullptr;
  libraries_loaded &= (pfn_d3d12_create_device_ = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(
                           GetProcAddress(library_d3d12_, "D3D12CreateDevice"))) != nullptr;
  libraries_loaded &=
      (pfn_d3d12_serialize_root_signature_ = reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(
           GetProcAddress(library_d3d12_, "D3D12SerializeRootSignature"))) != nullptr;
  if (!libraries_loaded) {
    REXUI_FAIL(false, "Failed to get DXGI or Direct3D 12 functions");
  }

  pfn_d3d_disassemble_ = nullptr;
  library_d3dcompiler_ = LoadLibraryW(L"D3DCompiler_47.dll");
  if (library_d3dcompiler_) {
    pfn_d3d_disassemble_ =
        reinterpret_cast<pD3DDisassemble>(GetProcAddress(library_d3dcompiler_, "D3DDisassemble"));
    if (pfn_d3d_disassemble_ == nullptr) {
      REXUI_DEBUG(
          "Failed to get D3DDisassemble from D3DCompiler_47.dll, DXBC "
          "disassembly for debugging will be unavailable");
    }
  } else {
    REXUI_DEBUG(
        "Failed to load D3DCompiler_47.dll, DXBC disassembly for debugging "
        "will be unavailable");
  }

  pfn_dxilconv_dxc_create_instance_ = nullptr;
  library_dxilconv_ = LoadLibraryW(L"dxilconv.dll");
  if (library_dxilconv_) {
    pfn_dxilconv_dxc_create_instance_ = reinterpret_cast<DxcCreateInstanceProc>(
        GetProcAddress(library_dxilconv_, "DxcCreateInstance"));
    if (pfn_dxilconv_dxc_create_instance_ == nullptr) {
      REXUI_DEBUG(
          "Failed to get DxcCreateInstance from dxilconv.dll, converted DXIL "
          "disassembly for debugging will be unavailable");
    }
  } else {
    REXUI_DEBUG(
        "Failed to load dxilconv.dll, converted DXIL disassembly for debugging "
        "will be unavailable, DXIL may be unsupported by your OS version");
  }

  pfn_dxcompiler_dxc_create_instance_ = nullptr;
  library_dxcompiler_ = LoadLibraryW(L"dxcompiler.dll");
  if (library_dxcompiler_) {
    pfn_dxcompiler_dxc_create_instance_ = reinterpret_cast<DxcCreateInstanceProc>(
        GetProcAddress(library_dxcompiler_, "DxcCreateInstance"));
    if (pfn_dxcompiler_dxc_create_instance_ == nullptr) {
      REXUI_DEBUG(
          "Failed to get DxcCreateInstance from dxcompiler.dll, converted DXIL "
          "disassembly for debugging will be unavailable");
    }
  } else {
    REXUI_DEBUG(
        "Failed to load dxcompiler.dll, converted DXIL disassembly for "
        "debugging will be unavailable, if needed, download the DirectX "
        "Shader Compiler and place the DLL in the ReXGlue directory");
  }

  if (settings_->break_on_error.Get() || settings_->break_on_warning.Get()) {
    IDXGIInfoQueue* dxgi_info_queue;
    if (SUCCEEDED(pfn_dxgi_get_debug_interface1_(0, IID_PPV_ARGS(&dxgi_info_queue)))) {
      if (settings_->break_on_error.Get()) {
        dxgi_info_queue->SetBreakOnSeverity(DXGI_DEBUG_ALL,
                                            DXGI_INFO_QUEUE_MESSAGE_SEVERITY_CORRUPTION, TRUE);
        dxgi_info_queue->SetBreakOnSeverity(DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_ERROR,
                                            TRUE);
      }
      if (settings_->break_on_warning.Get()) {
        dxgi_info_queue->SetBreakOnSeverity(DXGI_DEBUG_ALL,
                                            DXGI_INFO_QUEUE_MESSAGE_SEVERITY_WARNING, TRUE);
      }
      dxgi_info_queue->Release();
    }
  }

  bool debug = settings_->debug.Get();
  if (debug) {
    ID3D12Debug* debug_interface;
    if (SUCCEEDED(pfn_d3d12_get_debug_interface_(IID_PPV_ARGS(&debug_interface)))) {
      debug_interface->EnableDebugLayer();
      debug_interface->Release();
    } else {
      REXUI_WARN("Failed to enable the Direct3D 12 debug layer");
      debug = false;
    }

    {
      Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dred_settings;
      if (SUCCEEDED(pfn_d3d12_get_debug_interface_(IID_PPV_ARGS(&dred_settings)))) {
        dred_settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dred_settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        REXUI_INFO("DRED (Device Removed Extended Data) enabled");
      } else {
        REXUI_WARN(
            "Failed to enable DRED, device removal diagnostics will be "
            "limited");
      }
    }
  }
  IDXGIFactory2* dxgi_factory;
  if (FAILED(pfn_create_dxgi_factory2_(debug ? DXGI_CREATE_FACTORY_DEBUG : 0,
                                       IID_PPV_ARGS(&dxgi_factory)))) {
    REXUI_FAIL(false, "Failed to create a DXGI factory");
  }

  u32 adapter_index = 0;
  IDXGIAdapter1* adapter = nullptr;
  while (dxgi_factory->EnumAdapters1(adapter_index, &adapter) == S_OK) {
    DXGI_ADAPTER_DESC1 adapter_desc;
    if (SUCCEEDED(adapter->GetDesc1(&adapter_desc))) {
      if (SUCCEEDED(pfn_d3d12_create_device_(adapter, D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device),
                                             nullptr))) {
        if (adapter_index_wanted >= 0) {
          if (adapter_index == static_cast<u32>(adapter_index_wanted)) {
            break;
          }
        } else if (adapter_index_wanted == -2) {
          if (adapter_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            break;
          }
        } else {
          if (!(adapter_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            break;
          }
        }
      }
    }
    adapter->Release();
    adapter = nullptr;
    ++adapter_index;
  }
  if (adapter == nullptr) {
    REXUI_ERROR(
        "Failed to get an adapter supporting Direct3D 12 with the feature "
        "level of at least 11_0");
    dxgi_factory->Release();
    return false;
  }
  DXGI_ADAPTER_DESC adapter_desc;
  if (FAILED(adapter->GetDesc(&adapter_desc))) {
    REXUI_ERROR("Failed to get the DXGI adapter description");
    adapter->Release();
    dxgi_factory->Release();
    return false;
  }
  adapter_vendor_id_ = static_cast<GPUVendorID>(adapter_desc.VendorId);
  int adapter_name_mb_size =
      WideCharToMultiByte(CP_UTF8, 0, adapter_desc.Description, -1, nullptr, 0, nullptr, nullptr);
  if (adapter_name_mb_size != 0) {
    char* adapter_name_mb = reinterpret_cast<char*>(alloca(adapter_name_mb_size));
    if (WideCharToMultiByte(CP_UTF8, 0, adapter_desc.Description, -1, adapter_name_mb,
                            adapter_name_mb_size, nullptr, nullptr) != 0) {
      REXGPU_INFO("DXGI adapter: {} (vendor 0x{:04X}, device 0x{:04X})", adapter_name_mb,
                  adapter_desc.VendorId, adapter_desc.DeviceId);
    }
  }

  ID3D12Device* device;
  if (FAILED(pfn_d3d12_create_device_(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
    REXUI_ERROR("Failed to create a Direct3D 12 feature level 11_0 device");
    adapter->Release();
    dxgi_factory->Release();
    return false;
  }
  adapter->Release();

  ID3D12InfoQueue* d3d12_info_queue;
  if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&d3d12_info_queue)))) {
    D3D12_MESSAGE_SEVERITY d3d12_info_queue_denied_severities[] = {
        D3D12_MESSAGE_SEVERITY_INFO,
    };
    D3D12_MESSAGE_ID d3d12_info_queue_denied_messages[] = {
        D3D12_MESSAGE_ID_CREATEINPUTLAYOUT_EMPTY_LAYOUT,
        // Older debug layers wrongly report a format mismatch for gaps in render target bindings.
        D3D12_MESSAGE_ID_RENDER_TARGET_FORMAT_MISMATCH_PIPELINE_STATE,
        D3D12_MESSAGE_ID_CREATEGRAPHICSPIPELINESTATE_RENDERTARGETVIEW_NOT_SET,
        D3D12_MESSAGE_ID_DRAW_EMPTY_SCISSOR_RECTANGLE,
        D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
        D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE,
    };
    D3D12_INFO_QUEUE_FILTER d3d12_info_queue_filter = {};
    d3d12_info_queue_filter.DenyList.NumSeverities =
        static_cast<UINT>(rex::CountOf(d3d12_info_queue_denied_severities));
    d3d12_info_queue_filter.DenyList.pSeverityList = d3d12_info_queue_denied_severities;
    d3d12_info_queue_filter.DenyList.NumIDs =
        static_cast<UINT>(rex::CountOf(d3d12_info_queue_denied_messages));
    d3d12_info_queue_filter.DenyList.pIDList = d3d12_info_queue_denied_messages;
    d3d12_info_queue->PushStorageFilter(&d3d12_info_queue_filter);
    if (settings_->break_on_error.Get()) {
      d3d12_info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
      d3d12_info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
    }
    if (settings_->break_on_warning.Get()) {
      d3d12_info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, TRUE);
    }
    d3d12_info_queue->Release();
  }

  D3D12_COMMAND_QUEUE_DESC queue_desc;
  queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  if (settings_->queue_priority.Get() >= 2) {
    queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME;
    if (!EnableIncreaseBasePriorityPrivilege()) {
      REXUI_WARN(
          "Failed to enable SeIncreaseBasePriorityPrivilege for global "
          "realtime Direct3D 12 command queue priority, falling back to high "
          "priority, try launching ReXGlue as administrator");
      queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    }
  } else if (settings_->queue_priority.Get() >= 1) {
    queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
  } else {
    queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
  }
  queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
  queue_desc.NodeMask = 0;
  ID3D12CommandQueue* direct_queue;
  if (FAILED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&direct_queue)))) {
    bool queue_created = false;
    if (queue_desc.Priority == D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME) {
      REXUI_WARN(
          "Failed to create a Direct3D 12 direct command queue with global "
          "realtime priority, falling back to high priority, try launching "
          "ReXGlue as administrator");
      queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
      queue_created =
          SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&direct_queue)));
    }
    if (!queue_created) {
      REXUI_ERROR("Failed to create a Direct3D 12 direct command queue");
      device->Release();
      dxgi_factory->Release();
      return false;
    }
  }

  dxgi_factory_ = dxgi_factory;
  device_ = device;
  direct_queue_ = direct_queue;

  for (u32 i = 0; i < D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES; ++i) {
    descriptor_sizes_[i] =
        device->GetDescriptorHandleIncrementSize(static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(i));
  }

  // D3D12_HEAP_FLAG_CREATE_NOT_ZEROED requires Windows 10 2004 (ID3D12Device8 or OPTIONS7).
  heap_flag_create_not_zeroed_ = D3D12_HEAP_FLAG_NONE;
  D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7;
  if (SUCCEEDED(
          device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7)))) {
    heap_flag_create_not_zeroed_ = D3D12_HEAP_FLAG_CREATE_NOT_ZEROED;
  }
  ps_specified_stencil_reference_supported_ = false;
  rasterizer_ordered_views_supported_ = false;
  resource_binding_tier_ = D3D12_RESOURCE_BINDING_TIER_1;
  tiled_resources_tier_ = D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED;
  unaligned_block_textures_supported_ = false;
  D3D12_FEATURE_DATA_D3D12_OPTIONS options;
  if (SUCCEEDED(
          device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)))) {
    ps_specified_stencil_reference_supported_ =
        static_cast<bool>(options.PSSpecifiedStencilRefSupported);
    rasterizer_ordered_views_supported_ = static_cast<bool>(options.ROVsSupported);
    resource_binding_tier_ = options.ResourceBindingTier;
    tiled_resources_tier_ = options.TiledResourcesTier;
  }
  programmable_sample_positions_tier_ = D3D12_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_NOT_SUPPORTED;
  D3D12_FEATURE_DATA_D3D12_OPTIONS2 options2;
  if (SUCCEEDED(
          device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS2, &options2, sizeof(options2)))) {
    programmable_sample_positions_tier_ = options2.ProgrammableSamplePositionsTier;
  }
  D3D12_FEATURE_DATA_D3D12_OPTIONS8 options8;
  if (SUCCEEDED(
          device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS8, &options8, sizeof(options8)))) {
    unaligned_block_textures_supported_ =
        static_cast<bool>(options8.UnalignedBlockTexturesSupported);
  }
  virtual_address_bits_per_resource_ = 0;
  D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT virtual_address_support;
  if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT,
                                            &virtual_address_support,
                                            sizeof(virtual_address_support)))) {
    virtual_address_bits_per_resource_ =
        virtual_address_support.MaxGPUVirtualAddressBitsPerResource;
  }
  REXGPU_INFO(
      "Direct3D 12 device and OS features:\n"
      "* Max GPU virtual address bits per resource: {}\n"
      "* Non-zeroed heap creation: {}\n"
      "* Pixel shader specified stencil reference: {}\n"
      "* Programmable sample positions: tier {}\n"
      "* Rasterizer ordered views: {}\n"
      "* Resource binding: tier {}\n"
      "* Tiled resources: tier {}\n"
      "* Unaligned block-compressed textures: {}",
      virtual_address_bits_per_resource_,
      (heap_flag_create_not_zeroed_ & D3D12_HEAP_FLAG_CREATE_NOT_ZEROED) ? "yes" : "no",
      ps_specified_stencil_reference_supported_ ? "yes" : "no",
      static_cast<u32>(programmable_sample_positions_tier_),
      rasterizer_ordered_views_supported_ ? "yes" : "no", static_cast<u32>(resource_binding_tier_),
      static_cast<u32>(tiled_resources_tier_), unaligned_block_textures_supported_ ? "yes" : "no");

  pfn_dxgi_get_debug_interface1_(0, IID_PPV_ARGS(&graphics_analysis_));

  return true;
}

std::unique_ptr<Presenter> D3D12Provider::CreatePresenter(
    const XenosGraphicsDriverBase& driver, Presenter::HostGPULossCallback host_gpu_loss_callback) {
  return D3D12Presenter::Create(std::move(host_gpu_loss_callback), driver, *this);
}

std::unique_ptr<OverlayDrawer> D3D12Provider::CreateImmediateDrawer() {
  return D3D12ImmediateDrawer::Create(*this);
}

}  // namespace rex::gpu::d3d12
