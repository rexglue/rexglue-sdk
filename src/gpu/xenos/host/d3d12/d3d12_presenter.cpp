/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <climits>
#include <cmath>
#include <memory>
#include <utility>

#include <rex/assert.h>
#include <rex/logging/log.h>
#include <rex/math.h>
#include "gpu/xenos/host/d3d12/d3d12_presenter.h"
#include "gpu/xenos/host/d3d12/d3d12_provider.h"
#include "gpu/xenos/host/d3d12/d3d12_objects.h"
#include <rex/utility.h>
#include <rex/settings.h>

#include "gpu/xenos/host/present_policy.h"

namespace rex::gpu::d3d12 {

namespace shaders {
#include "gpu/xenos/host/shaders/bytecode/d3d12_5_1/guest_output_bilinear_dither_ps.h"
#include "gpu/xenos/host/shaders/bytecode/d3d12_5_1/guest_output_bilinear_ps.h"
#include "gpu/xenos/host/shaders/bytecode/d3d12_5_1/guest_output_triangle_strip_rect_vs.h"
}  // namespace shaders

D3D12Presenter::~D3D12Presenter() {
  paint_context_.AwaitSwapChainUsageCompletion();
  guest_output_resource_refresher_submission_tracker_.Shutdown();
  ui_submission_tracker_.Shutdown();
}

gpu::Surface::TypeFlags D3D12Presenter::GetSupportedSurfaceTypes() const {
  gpu::Surface::TypeFlags types = 0;
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP | WINAPI_PARTITION_GAMES)
  types |= gpu::Surface::kTypeFlagWin32HWND;
#endif
  return types;
}

Presenter::SurfacePaintConnectResult
D3D12Presenter::ConnectOrReconnectPaintingToSurfaceFromUIThread(gpu::Surface& new_surface,
                                                                u32 new_surface_width,
                                                                u32 new_surface_height,
                                                                bool was_paintable,
                                                                bool& is_vsync_implicit_out) {
  u32 new_swap_chain_width =
      std::min(new_surface_width, static_cast<u32>(D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION));
  u32 new_swap_chain_height =
      std::min(new_surface_height, static_cast<u32>(D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION));

  if (paint_context_.swap_chain) {
    if (was_paintable && paint_context_.swap_chain_width == new_swap_chain_width &&
        paint_context_.swap_chain_height == new_swap_chain_height) {
      is_vsync_implicit_out = IsVsyncImplicit(
          ChoosePresentSync(GetPaintConnectionVsync(), paint_context_.swap_chain_allows_tearing));
      return SurfacePaintConnectResult::kSuccessUnchanged;
    }
    paint_context_.AwaitSwapChainUsageCompletion();
    // ResizeBuffers cannot toggle the tearing flag, so reuse the value from swap chain creation.
    for (Microsoft::WRL::ComPtr<ID3D12Resource>& swap_chain_buffer_ref :
         paint_context_.swap_chain_buffers) {
      swap_chain_buffer_ref.Reset();
    }
    bool swap_chain_resized = SUCCEEDED(paint_context_.swap_chain->ResizeBuffers(
        0, static_cast<UINT>(new_swap_chain_width), static_cast<UINT>(new_swap_chain_height),
        DXGI_FORMAT_UNKNOWN,
        paint_context_.swap_chain_allows_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0));
    if (swap_chain_resized) {
      for (u32 i = 0; i < PaintContext::kSwapChainBufferCount; ++i) {
        if (FAILED(paint_context_.swap_chain->GetBuffer(
                i, IID_PPV_ARGS(&paint_context_.swap_chain_buffers[i])))) {
          swap_chain_resized = false;
          break;
        }
      }
      if (swap_chain_resized) {
        paint_context_.swap_chain_width = new_swap_chain_width;
        paint_context_.swap_chain_height = new_swap_chain_height;
      }
    }
    if (!swap_chain_resized) {
      REXLOG_ERROR(ui, "D3D12Presenter: Failed to resize a swap chain");
      paint_context_.DestroySwapChain();
    }
  }

  if (!paint_context_.swap_chain) {
    gpu::Surface::TypeIndex surface_type = new_surface.GetType();
    DXGI_SWAP_CHAIN_DESC1 swap_chain_desc;
    swap_chain_desc.Width = static_cast<UINT>(new_swap_chain_width);
    swap_chain_desc.Height = static_cast<UINT>(new_swap_chain_height);
    swap_chain_desc.Format = kSwapChainFormat;
    swap_chain_desc.Stereo = FALSE;
    swap_chain_desc.SampleDesc.Count = 1;
    swap_chain_desc.SampleDesc.Quality = 0;
    swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_chain_desc.BufferCount = static_cast<UINT>(PaintContext::kSwapChainBufferCount);
    // DXGI_SCALING_STRETCH makes content shake while resizing, so stretch only when needed.
    swap_chain_desc.Scaling =
        (new_swap_chain_width == new_surface_width && new_swap_chain_height == new_surface_height)
            ? DXGI_SCALING_NONE
            : DXGI_SCALING_STRETCH;
    swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swap_chain_desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    swap_chain_desc.Flags = 0;
    if (provider_.GetSettings().allow_tearing.Get() && dxgi_supports_tearing_) {
      swap_chain_desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }
    IDXGIFactory2* dxgi_factory = provider_.GetDXGIFactory();
    ID3D12CommandQueue* direct_queue = provider_.GetDirectQueue();
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_chain_1;
    switch (surface_type) {
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP | WINAPI_PARTITION_GAMES)
      case gpu::Surface::kTypeIndexWin32HWND: {
        HWND surface_hwnd =
            static_cast<HWND>(static_cast<const gpu::NativeSurface&>(new_surface).GetHWND());
        if (FAILED(dxgi_factory->CreateSwapChainForHwnd(
                direct_queue, surface_hwnd, &swap_chain_desc, nullptr, nullptr, &swap_chain_1))) {
          REX_FAIL(ui, SurfacePaintConnectResult::kFailure,
                   "D3D12Presenter: Failed to create a swap chain for the HWND");
        }
        // DXGI fullscreen does not support ALLOW_TEARING, so disable Alt+Enter handling.
        dxgi_factory->MakeWindowAssociation(surface_hwnd, DXGI_MWA_NO_ALT_ENTER);
      } break;
#endif
      default:
        REX_UNHANDLED_CASE(surface_type);
        REX_FAIL(ui, SurfacePaintConnectResult::kFailureSurfaceUnusable,
                 "D3D12Presenter: Tried to create a swap chain for an unsupported "
                 "ReXGlue surface type");
    }
    if (FAILED(swap_chain_1->QueryInterface(IID_PPV_ARGS(&paint_context_.swap_chain)))) {
      REX_FAIL(ui, SurfacePaintConnectResult::kFailure,
               "D3D12Presenter: Failed to get version 3 of the swap chain "
               "interface");
    }
    // Any failure from here on must call DestroySwapChain before returning.
    paint_context_.swap_chain_width = new_swap_chain_width;
    paint_context_.swap_chain_height = new_swap_chain_height;
    paint_context_.swap_chain_allows_tearing =
        (swap_chain_desc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0;
    for (u32 i = 0; i < PaintContext::kSwapChainBufferCount; ++i) {
      if (FAILED(paint_context_.swap_chain->GetBuffer(
              i, IID_PPV_ARGS(&paint_context_.swap_chain_buffers[i])))) {
        REXLOG_ERROR(ui, "D3D12Presenter: Failed to get buffer {} of a {}-buffer swap chain", i,
                     PaintContext::kSwapChainBufferCount);
        paint_context_.DestroySwapChain();
        return SurfacePaintConnectResult::kFailure;
      }
    }
  }

  ID3D12Device* device = provider_.GetDevice();

  D3D12_CPU_DESCRIPTOR_HANDLE rtv_heap_start =
      paint_context_.rtv_heap->GetCPUDescriptorHandleForHeapStart();
  D3D12_RENDER_TARGET_VIEW_DESC rtv_desc;
  rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
  rtv_desc.Texture2D.MipSlice = 0;
  rtv_desc.Texture2D.PlaneSlice = 0;
  for (u32 i = 0; i < PaintContext::kSwapChainBufferCount; ++i) {
    ID3D12Resource* swap_chain_buffer = paint_context_.swap_chain_buffers[i].Get();
    rtv_desc.Format = kSwapChainFormat;
    device->CreateRenderTargetView(
        swap_chain_buffer, &rtv_desc,
        provider_.OffsetRTVDescriptor(rtv_heap_start, PaintContext::kRTVIndexSwapChainBuffer0 + i));
  }

  is_vsync_implicit_out = IsVsyncImplicit(
      ChoosePresentSync(GetPaintConnectionVsync(), paint_context_.swap_chain_allows_tearing));
  return SurfacePaintConnectResult::kSuccess;
}

void D3D12Presenter::DriverDisconnectPaintingFromSurface() {
  paint_context_.DestroySwapChain();
}

bool D3D12Presenter::DriverRefreshGuestOutput(
    u32 mailbox_index, u32 frontbuffer_width, u32 frontbuffer_height,
    rex::MoveOnlyFunction<bool(GuestOutputRefreshContext& context)> refresher,
    bool& is_8bpc_out_ref) {
  REX_ASSERT(frontbuffer_width != 0);
  REX_ASSERT(frontbuffer_height != 0);
  std::pair<UINT64, Microsoft::WRL::ComPtr<ID3D12Resource>>& guest_output_resource_ref =
      guest_output_resources_[mailbox_index];
  if (guest_output_resource_ref.second) {
    D3D12_RESOURCE_DESC guest_output_resource_current_desc =
        guest_output_resource_ref.second->GetDesc();
    if (guest_output_resource_current_desc.Width != frontbuffer_width ||
        guest_output_resource_current_desc.Height != frontbuffer_height) {
      guest_output_resource_refresher_submission_tracker_.AwaitSubmissionCompletion(
          guest_output_resource_ref.first);
      guest_output_resource_ref.second.Reset();
    }
  }
  if (!guest_output_resource_ref.second) {
    ID3D12Device* device = provider_.GetDevice();
    D3D12_RESOURCE_DESC guest_output_resource_new_desc;
    guest_output_resource_new_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    guest_output_resource_new_desc.Alignment = 0;
    guest_output_resource_new_desc.Width = frontbuffer_width;
    guest_output_resource_new_desc.Height = frontbuffer_height;
    guest_output_resource_new_desc.DepthOrArraySize = 1;
    guest_output_resource_new_desc.MipLevels = 1;
    guest_output_resource_new_desc.Format = kGuestOutputFormat;
    guest_output_resource_new_desc.SampleDesc.Count = 1;
    guest_output_resource_new_desc.SampleDesc.Quality = 0;
    guest_output_resource_new_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    guest_output_resource_new_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(device->CreateCommittedResource(
            &objects::kHeapPropertiesDefault, provider_.GetHeapFlagCreateNotZeroed(),
            &guest_output_resource_new_desc, kGuestOutputInternalState, nullptr,
            IID_PPV_ARGS(&guest_output_resource_ref.second)))) {
      REX_FAIL(ui, false, "D3D12Presenter: Failed to create the guest output {}x{} texture",
               frontbuffer_width, frontbuffer_height);
    }
  }
  D3D12GuestOutputRefreshContext context(is_8bpc_out_ref, guest_output_resource_ref.second.Get());
  bool refresher_succeeded = refresher(context);
  // The refresher may submit commands referencing the resource even when it returns false.
  guest_output_resource_ref.first =
      guest_output_resource_refresher_submission_tracker_.GetCurrentSubmission();
  guest_output_resource_refresher_submission_tracker_.NextSubmission();
  return refresher_succeeded;
}

void D3D12Presenter::PaintContext::DestroySwapChain() {
  if (!swap_chain) {
    return;
  }
  AwaitSwapChainUsageCompletion();
  for (Microsoft::WRL::ComPtr<ID3D12Resource>& swap_chain_buffer_ref : swap_chain_buffers) {
    swap_chain_buffer_ref.Reset();
  }
  swap_chain.Reset();
  swap_chain_allows_tearing = false;
  swap_chain_height = 0;
  swap_chain_width = 0;
}

Presenter::PaintResult D3D12Presenter::DriverPaintAndPresent(bool execute_ui_drawers) {
  UINT64 current_paint_submission = paint_context_.paint_submission_tracker.GetCurrentSubmission();
  UINT64 command_allocator_count = static_cast<UINT64>(paint_context_.command_allocators.size());
  if (current_paint_submission >= command_allocator_count) {
    paint_context_.paint_submission_tracker.AwaitSubmissionCompletion(current_paint_submission -
                                                                      command_allocator_count);
  }
  ID3D12CommandAllocator* command_allocator =
      paint_context_.command_allocators[current_paint_submission % command_allocator_count].Get();
  command_allocator->Reset();
  ID3D12GraphicsCommandList* command_list = paint_context_.command_list.Get();
  command_list->Reset(command_allocator, nullptr);

  ID3D12Device* device = provider_.GetDevice();

  D3D12_CPU_DESCRIPTOR_HANDLE rtv_heap_start =
      paint_context_.rtv_heap->GetCPUDescriptorHandleForHeapStart();
  UINT back_buffer_index = paint_context_.swap_chain->GetCurrentBackBufferIndex();
  D3D12_CPU_DESCRIPTOR_HANDLE back_buffer_rtv = provider_.OffsetRTVDescriptor(
      rtv_heap_start, PaintContext::kRTVIndexSwapChainBuffer0 + back_buffer_index);
  ID3D12Resource* back_buffer = paint_context_.swap_chain_buffers[back_buffer_index].Get();
  bool back_buffer_acquired = false;
  bool back_buffer_bound = false;
  bool back_buffer_clear_needed = true;
  constexpr f32 kBackBufferClearColor[] = {0.0f, 0.0f, 0.0f, 1.0f};

  GuestOutputProperties guest_output_properties;
  GuestOutputPaintConfig guest_output_paint_config;
  Microsoft::WRL::ComPtr<ID3D12Resource> guest_output_resource;
  {
    u32 guest_output_mailbox_index;
    std::unique_lock<std::mutex> guest_output_consumer_lock(ConsumeGuestOutput(
        guest_output_mailbox_index, &guest_output_properties, &guest_output_paint_config));
    if (guest_output_mailbox_index != UINT32_MAX) {
      guest_output_resource = guest_output_resources_[guest_output_mailbox_index].second;
    }
  }

  if (guest_output_resource) {
    GuestOutputPaintFlow guest_output_flow = GetGuestOutputPaintFlow(
        guest_output_properties, paint_context_.swap_chain_width, paint_context_.swap_chain_height,
        D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION, D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION,
        guest_output_paint_config);

    if (guest_output_flow.has_effect &&
        !guest_output_paint_pipelines_[static_cast<size_t>(guest_output_flow.effect)]) {
      guest_output_flow.has_effect = false;
    }

    if (guest_output_flow.has_effect) {
      ID3D12DescriptorHeap* view_heap = paint_context_.view_heap.Get();
      D3D12_CPU_DESCRIPTOR_HANDLE view_heap_cpu_start =
          view_heap->GetCPUDescriptorHandleForHeapStart();

      size_t guest_output_resource_paint_ref_index = SIZE_MAX;
      size_t guest_output_resource_paint_ref_new_index = SIZE_MAX;
      for (size_t i = 0; i < paint_context_.guest_output_resource_paint_refs.size(); ++i) {
        const std::pair<UINT64, Microsoft::WRL::ComPtr<ID3D12Resource>>&
            guest_output_resource_paint_ref = paint_context_.guest_output_resource_paint_refs[i];
        if (guest_output_resource_paint_ref.second == guest_output_resource) {
          guest_output_resource_paint_ref_index = i;
          break;
        }
        if (guest_output_resource_paint_ref_new_index == SIZE_MAX &&
            (!guest_output_resource_paint_ref.second || !guest_output_resource_paint_ref.first)) {
          guest_output_resource_paint_ref_new_index = i;
        }
      }
      if (guest_output_resource_paint_ref_index == SIZE_MAX) {
        if (guest_output_resource_paint_ref_new_index == SIZE_MAX) {
          guest_output_resource_paint_ref_new_index = 0;
          for (size_t i = 1; i < paint_context_.guest_output_resource_paint_refs.size(); ++i) {
            if (paint_context_.guest_output_resource_paint_refs[i].first <
                paint_context_
                    .guest_output_resource_paint_refs[guest_output_resource_paint_ref_new_index]
                    .first) {
              guest_output_resource_paint_ref_new_index = i;
            }
          }
          paint_context_.paint_submission_tracker.AwaitSubmissionCompletion(
              paint_context_
                  .guest_output_resource_paint_refs[guest_output_resource_paint_ref_new_index]
                  .first);
        }
        guest_output_resource_paint_ref_index = guest_output_resource_paint_ref_new_index;
        paint_context_.guest_output_resource_paint_refs[guest_output_resource_paint_ref_index] =
            std::make_pair(static_cast<UINT64>(0), guest_output_resource);
        D3D12_SHADER_RESOURCE_VIEW_DESC guest_output_resource_srv_desc;
        guest_output_resource_srv_desc.Format = kGuestOutputFormat;
        guest_output_resource_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        guest_output_resource_srv_desc.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        guest_output_resource_srv_desc.Texture2D.MostDetailedMip = 0;
        guest_output_resource_srv_desc.Texture2D.MipLevels = 1;
        guest_output_resource_srv_desc.Texture2D.PlaneSlice = 0;
        guest_output_resource_srv_desc.Texture2D.ResourceMinLODClamp = 0.0f;
        device->CreateShaderResourceView(
            guest_output_resource.Get(), &guest_output_resource_srv_desc,
            provider_.OffsetViewDescriptor(
                view_heap_cpu_start, PaintContext::kViewIndexGuestOutput0SRV +
                                         static_cast<u32>(guest_output_resource_paint_ref_index)));
      }

      paint_context_.guest_output_resource_paint_refs[guest_output_resource_paint_ref_index].first =
          current_paint_submission;

      command_list->SetDescriptorHeaps(1, &view_heap);

      D3D12_GPU_DESCRIPTOR_HANDLE view_heap_gpu_start =
          view_heap->GetGPUDescriptorHandleForHeapStart();

      GuestOutputPaintEffect effect = guest_output_flow.effect;

      if (!back_buffer_acquired) {
        D3D12_RESOURCE_BARRIER barrier_present_to_rtv;
        barrier_present_to_rtv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier_present_to_rtv.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier_present_to_rtv.Transition.pResource = back_buffer;
        barrier_present_to_rtv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier_present_to_rtv.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier_present_to_rtv.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        command_list->ResourceBarrier(1, &barrier_present_to_rtv);
        back_buffer_acquired = true;
      }
      if (!back_buffer_bound) {
        command_list->OMSetRenderTargets(1, &back_buffer_rtv, TRUE, nullptr);
        back_buffer_bound = true;
      }

      D3D12_RESOURCE_DESC back_buffer_desc = back_buffer->GetDesc();
      D3D12_VIEWPORT viewport;
      viewport.TopLeftX = 0.0f;
      viewport.TopLeftY = 0.0f;
      viewport.Width = static_cast<f32>(back_buffer_desc.Width);
      viewport.Height = static_cast<f32>(back_buffer_desc.Height);
      viewport.MinDepth = 0.0f;
      viewport.MaxDepth = 1.0f;
      command_list->RSSetViewports(1, &viewport);
      D3D12_RECT scissor;
      scissor.left = 0;
      scissor.top = 0;
      scissor.right = static_cast<LONG>(back_buffer_desc.Width);
      scissor.bottom = static_cast<LONG>(back_buffer_desc.Height);
      command_list->RSSetScissorRects(1, &scissor);

      command_list->SetPipelineState(
          guest_output_paint_pipelines_[static_cast<size_t>(effect)].Get());
      GuestOutputPaintRootSignatureIndex guest_output_paint_root_signature_index =
          GetGuestOutputPaintRootSignatureIndex(effect);
      command_list->SetGraphicsRootSignature(
          guest_output_paint_root_signatures_[static_cast<size_t>(
                                                  guest_output_paint_root_signature_index)]
              .Get());

      UINT effect_src_view_index = static_cast<UINT>(PaintContext::kViewIndexGuestOutput0SRV +
                                                     guest_output_resource_paint_ref_index);
      command_list->SetGraphicsRootDescriptorTable(
          static_cast<UINT>(GuestOutputPaintRootParameter::kSource),
          provider_.OffsetViewDescriptor(view_heap_gpu_start, effect_src_view_index));

      GuestOutputPaintRectangleConstants effect_rect_constants;
      f32 effect_x_to_ndc = 2.0f / viewport.Width;
      f32 effect_y_to_ndc = 2.0f / viewport.Height;
      effect_rect_constants.x =
          -1.0f + static_cast<f32>(guest_output_flow.output_x) * effect_x_to_ndc;
      effect_rect_constants.y =
          1.0f - static_cast<f32>(guest_output_flow.output_y) * effect_y_to_ndc;
      effect_rect_constants.width =
          static_cast<f32>(guest_output_flow.effect_output_size.first) * effect_x_to_ndc;
      effect_rect_constants.height =
          -static_cast<f32>(guest_output_flow.effect_output_size.second) * effect_y_to_ndc;
      command_list->SetGraphicsRoot32BitConstants(
          static_cast<UINT>(GuestOutputPaintRootParameter::kRectangle),
          sizeof(effect_rect_constants) / sizeof(u32), &effect_rect_constants, 0);

      UINT effect_constants_size = 0;
      union {
        BilinearConstants bilinear;
      } effect_constants;
      switch (guest_output_paint_root_signature_index) {
        case kGuestOutputPaintRootSignatureIndexBilinear: {
          effect_constants_size = sizeof(effect_constants.bilinear);
          effect_constants.bilinear.Initialize(guest_output_flow);
        } break;
        default:
          break;
      }
      if (effect_constants_size) {
        command_list->SetGraphicsRoot32BitConstants(
            static_cast<UINT>(GuestOutputPaintRootParameter::kEffectConstants),
            effect_constants_size / sizeof(u32), &effect_constants, 0);
      }

      command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
      command_list->DrawInstanced(4, 1, 0, 0);

      if (guest_output_flow.letterbox_clear_rectangle_count) {
        D3D12_RECT letterbox_clear_d3d12_rectangles[GuestOutputPaintFlow::kMaxClearRectangles];
        for (size_t i = 0; i < guest_output_flow.letterbox_clear_rectangle_count; ++i) {
          D3D12_RECT& letterbox_clear_d3d12_rectangle = letterbox_clear_d3d12_rectangles[i];
          const GuestOutputPaintFlow::ClearRectangle& letterbox_clear_rectangle =
              guest_output_flow.letterbox_clear_rectangles[i];
          letterbox_clear_d3d12_rectangle.left = static_cast<LONG>(letterbox_clear_rectangle.x);
          letterbox_clear_d3d12_rectangle.top = static_cast<LONG>(letterbox_clear_rectangle.y);
          letterbox_clear_d3d12_rectangle.right =
              static_cast<LONG>(letterbox_clear_rectangle.x) +
              static_cast<LONG>(letterbox_clear_rectangle.width);
          letterbox_clear_d3d12_rectangle.bottom =
              static_cast<LONG>(letterbox_clear_rectangle.y) +
              static_cast<LONG>(letterbox_clear_rectangle.height);
        }
        command_list->ClearRenderTargetView(
            back_buffer_rtv, kBackBufferClearColor,
            static_cast<UINT>(guest_output_flow.letterbox_clear_rectangle_count),
            letterbox_clear_d3d12_rectangles);
      }
      back_buffer_clear_needed = false;
    }
  }

  // Read the completed submission after the guest output waits above so it is current.
  UINT64 completed_paint_submission =
      paint_context_.paint_submission_tracker.GetCompletedSubmission();
  for (std::pair<UINT64, Microsoft::WRL::ComPtr<ID3D12Resource>>& guest_output_resource_paint_ref :
       paint_context_.guest_output_resource_paint_refs) {
    if (!guest_output_resource_paint_ref.second ||
        guest_output_resource_paint_ref.second == guest_output_resource) {
      continue;
    }
    if (completed_paint_submission >= guest_output_resource_paint_ref.first) {
      guest_output_resource_paint_ref.second.Reset();
    }
  }

  if (!back_buffer_acquired) {
    D3D12_RESOURCE_BARRIER barrier_present_to_rtv;
    barrier_present_to_rtv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier_present_to_rtv.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier_present_to_rtv.Transition.pResource = back_buffer;
    barrier_present_to_rtv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier_present_to_rtv.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier_present_to_rtv.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    command_list->ResourceBarrier(1, &barrier_present_to_rtv);
    back_buffer_acquired = true;
  }

  if (back_buffer_clear_needed) {
    command_list->ClearRenderTargetView(back_buffer_rtv, kBackBufferClearColor, 0, nullptr);
    back_buffer_clear_needed = false;
  }

  if (execute_ui_drawers) {
    if (!back_buffer_bound) {
      command_list->OMSetRenderTargets(1, &back_buffer_rtv, TRUE, nullptr);
      back_buffer_bound = true;
    }
    D3D12OverlayDrawContext ui_draw_context(*this, paint_context_.swap_chain_width,
                                            paint_context_.swap_chain_height, command_list,
                                            ui_submission_tracker_.GetCurrentSubmission(),
                                            ui_submission_tracker_.GetCompletedSubmission());
    ExecuteOverlayLayersFromUIThread(ui_draw_context);
  }

  D3D12_RESOURCE_BARRIER barrier_rtv_to_present;
  barrier_rtv_to_present.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier_rtv_to_present.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier_rtv_to_present.Transition.pResource = back_buffer;
  barrier_rtv_to_present.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier_rtv_to_present.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
  barrier_rtv_to_present.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
  command_list->ResourceBarrier(1, &barrier_rtv_to_present);

  command_list->Close();
  ID3D12CommandList* execute_command_list = command_list;
  provider_.GetDirectQueue()->ExecuteCommandLists(1, &execute_command_list);
  if (execute_ui_drawers) {
    ui_submission_tracker_.NextSubmission();
  }
  paint_context_.paint_submission_tracker.NextSubmission();
  const PresentSync present_sync =
      ChoosePresentSync(GetPaintConnectionVsync(), paint_context_.swap_chain_allows_tearing);
  UINT present_flags = 0;
  if (present_sync.sync_interval == 0) {
    present_flags =
        DXGI_PRESENT_RESTART | (present_sync.allow_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
  }
  HRESULT present_result =
      paint_context_.swap_chain->Present(present_sync.sync_interval, present_flags);
  // Present may enqueue work internally even when it fails.
  paint_context_.present_submission_tracker.NextSubmission();
  switch (present_result) {
    case DXGI_ERROR_DEVICE_REMOVED:
      return PaintResult::kGPULostExternally;
    case DXGI_ERROR_DEVICE_RESET:
      return PaintResult::kGPULostResponsible;
    default:
      return SUCCEEDED(present_result) ? PaintResult::kPresented : PaintResult::kNotPresented;
  }
}

bool D3D12Presenter::InitializeSurfaceIndependent() {
  {
    Microsoft::WRL::ComPtr<IDXGIFactory5> dxgi_factory_5;
    if (SUCCEEDED(provider_.GetDXGIFactory()->QueryInterface(IID_PPV_ARGS(&dxgi_factory_5)))) {
      BOOL tearing_feature_data;
      dxgi_supports_tearing_ = SUCCEEDED(dxgi_factory_5->CheckFeatureSupport(
                                   DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing_feature_data,
                                   sizeof(tearing_feature_data))) &&
                               tearing_feature_data;
    }
  }

  ID3D12Device* device = provider_.GetDevice();

  D3D12_ROOT_PARAMETER
  guest_output_paint_root_parameters[static_cast<UINT>(GuestOutputPaintRootParameter::kCount)];
  D3D12_DESCRIPTOR_RANGE guest_output_paint_root_descriptor_range_source;
  guest_output_paint_root_descriptor_range_source.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  guest_output_paint_root_descriptor_range_source.NumDescriptors = 1;
  guest_output_paint_root_descriptor_range_source.BaseShaderRegister = 0;
  guest_output_paint_root_descriptor_range_source.RegisterSpace = 0;
  guest_output_paint_root_descriptor_range_source.OffsetInDescriptorsFromTableStart = 0;
  {
    D3D12_ROOT_PARAMETER& guest_output_paint_root_parameter_source =
        guest_output_paint_root_parameters[static_cast<UINT>(
            GuestOutputPaintRootParameter::kSource)];
    guest_output_paint_root_parameter_source.ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    guest_output_paint_root_parameter_source.DescriptorTable.NumDescriptorRanges = 1;
    guest_output_paint_root_parameter_source.DescriptorTable.pDescriptorRanges =
        &guest_output_paint_root_descriptor_range_source;
    guest_output_paint_root_parameter_source.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  }
  {
    D3D12_ROOT_PARAMETER& guest_output_paint_root_parameter_rect =
        guest_output_paint_root_parameters[static_cast<UINT>(
            GuestOutputPaintRootParameter::kRectangle)];
    guest_output_paint_root_parameter_rect.ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    guest_output_paint_root_parameter_rect.Constants.ShaderRegister = 0;
    guest_output_paint_root_parameter_rect.Constants.RegisterSpace = 0;
    guest_output_paint_root_parameter_rect.Constants.Num32BitValues =
        sizeof(GuestOutputPaintRectangleConstants) / sizeof(u32);
    guest_output_paint_root_parameter_rect.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
  }
  D3D12_ROOT_PARAMETER& guest_output_paint_root_parameter_effect_constants =
      guest_output_paint_root_parameters[static_cast<UINT>(
          GuestOutputPaintRootParameter::kEffectConstants)];
  guest_output_paint_root_parameter_effect_constants.ParameterType =
      D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  guest_output_paint_root_parameter_effect_constants.Constants.ShaderRegister = 0;
  guest_output_paint_root_parameter_effect_constants.Constants.RegisterSpace = 0;
  guest_output_paint_root_parameter_effect_constants.ShaderVisibility =
      D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_STATIC_SAMPLER_DESC guest_output_paint_root_sampler;
  guest_output_paint_root_sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  guest_output_paint_root_sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  guest_output_paint_root_sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  guest_output_paint_root_sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  guest_output_paint_root_sampler.MipLODBias = 0.0f;
  guest_output_paint_root_sampler.MaxAnisotropy = 1;
  guest_output_paint_root_sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
  guest_output_paint_root_sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
  guest_output_paint_root_sampler.MinLOD = 0.0f;
  guest_output_paint_root_sampler.MaxLOD = 0.0f;
  guest_output_paint_root_sampler.ShaderRegister = 0;
  guest_output_paint_root_sampler.RegisterSpace = 0;
  guest_output_paint_root_sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_ROOT_SIGNATURE_DESC guest_output_paint_root_signature_desc;
  guest_output_paint_root_signature_desc.NumParameters =
      static_cast<UINT>(GuestOutputPaintRootParameter::kCount);
  guest_output_paint_root_signature_desc.pParameters = guest_output_paint_root_parameters;
  guest_output_paint_root_signature_desc.NumStaticSamplers = 1;
  guest_output_paint_root_signature_desc.pStaticSamplers = &guest_output_paint_root_sampler;
  guest_output_paint_root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
  guest_output_paint_root_parameter_effect_constants.Constants.Num32BitValues =
      sizeof(BilinearConstants) / sizeof(u32);
  {
    ID3D12RootSignature* guest_output_paint_root_signature =
        objects::CreateRootSignature(provider_, guest_output_paint_root_signature_desc);
    if (!guest_output_paint_root_signature) {
      REX_FAIL(ui, false,
               "D3D12Presenter: Failed to create the guest output bilinear "
               "filtering presentation root signature");
    }
    *(guest_output_paint_root_signatures_[kGuestOutputPaintRootSignatureIndexBilinear]
          .ReleaseAndGetAddressOf()) = guest_output_paint_root_signature;
  }

  auto guest_output_paint_pipeline_desc = objects::BuildGraphicsPipelineStateDesc();
  guest_output_paint_pipeline_desc.VS.pShaderBytecode = shaders::kGuestOutputTriangleStripRectVS;
  guest_output_paint_pipeline_desc.VS.BytecodeLength =
      sizeof(shaders::kGuestOutputTriangleStripRectVS);
  guest_output_paint_pipeline_desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
      D3D12_COLOR_WRITE_ENABLE_ALL;
  guest_output_paint_pipeline_desc.SampleMask = UINT_MAX;
  guest_output_paint_pipeline_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  guest_output_paint_pipeline_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  guest_output_paint_pipeline_desc.RasterizerState.DepthClipEnable = TRUE;
  guest_output_paint_pipeline_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  guest_output_paint_pipeline_desc.NumRenderTargets = 1;
  guest_output_paint_pipeline_desc.SampleDesc.Count = 1;
  for (size_t i = 0; i < static_cast<size_t>(GuestOutputPaintEffect::kCount); ++i) {
    GuestOutputPaintEffect guest_output_paint_effect = static_cast<GuestOutputPaintEffect>(i);
    switch (guest_output_paint_effect) {
      case GuestOutputPaintEffect::kBilinear:
        guest_output_paint_pipeline_desc.PS.pShaderBytecode = shaders::kGuestOutputBilinearPS;
        guest_output_paint_pipeline_desc.PS.BytecodeLength =
            sizeof(shaders::kGuestOutputBilinearPS);
        break;
      case GuestOutputPaintEffect::kBilinearDither:
        guest_output_paint_pipeline_desc.PS.pShaderBytecode = shaders::kGuestOutputBilinearDitherPS;
        guest_output_paint_pipeline_desc.PS.BytecodeLength =
            sizeof(shaders::kGuestOutputBilinearDitherPS);
        break;
      default:
        continue;
    }
    guest_output_paint_pipeline_desc.pRootSignature =
        guest_output_paint_root_signatures_[GetGuestOutputPaintRootSignatureIndex(
                                                guest_output_paint_effect)]
            .Get();
    guest_output_paint_pipeline_desc.RTVFormats[0] = kSwapChainFormat;
    if (FAILED(device->CreateGraphicsPipelineState(
            &guest_output_paint_pipeline_desc, IID_PPV_ARGS(&guest_output_paint_pipelines_[i])))) {
      REX_FAIL(ui, false,
               "D3D12Presenter: Failed to create the guest output painting "
               "pipeline for effect {} writing to a swap chain buffer",
               i);
    }
  }

  ID3D12CommandQueue* direct_queue = provider_.GetDirectQueue();

  if (!paint_context_.paint_submission_tracker.Initialize(device, direct_queue) ||
      !paint_context_.present_submission_tracker.Initialize(device, direct_queue)) {
    return false;
  }

  for (Microsoft::WRL::ComPtr<ID3D12CommandAllocator>& paint_command_allocator_ref :
       paint_context_.command_allocators) {
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&paint_command_allocator_ref)))) {
      REX_FAIL(ui, false,
               "D3D12Presenter: Failed to create a command allocator for drawing to "
               "a swap chain");
    }
  }
  if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                       paint_context_.command_allocators[0].Get(), nullptr,
                                       IID_PPV_ARGS(&paint_context_.command_list)))) {
    REX_FAIL(ui, false,
             "D3D12Presenter: Failed to create the command list for drawing to a "
             "swap chain");
  }
  // Command lists are created open, so close it before the first Reset.
  paint_context_.command_list->Close();

  D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc;
  rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rtv_heap_desc.NumDescriptors = PaintContext::kRTVCount;
  rtv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  rtv_heap_desc.NodeMask = 0;
  if (FAILED(
          device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&paint_context_.rtv_heap)))) {
    REX_FAIL(ui, false,
             "D3D12Presenter: Failed to create an RTV descriptor heap with {} "
             "descriptors",
             rtv_heap_desc.NumDescriptors);
  }

  D3D12_DESCRIPTOR_HEAP_DESC view_heap_desc;
  view_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  view_heap_desc.NumDescriptors = PaintContext::kViewCount;
  view_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  view_heap_desc.NodeMask = 0;
  if (FAILED(
          device->CreateDescriptorHeap(&view_heap_desc, IID_PPV_ARGS(&paint_context_.view_heap)))) {
    REX_FAIL(ui, false,
             "D3D12Presenter: Failed to create a shader visible CBV/SRV/UAV "
             "descriptor heap with {} descriptors",
             view_heap_desc.NumDescriptors);
  }

  if (!guest_output_resource_refresher_submission_tracker_.Initialize(device, direct_queue)) {
    return false;
  }

  if (!ui_submission_tracker_.Initialize(device, direct_queue)) {
    return false;
  }

  return InitializeCommonSurfaceIndependent();
}

}  // namespace rex::gpu::d3d12
