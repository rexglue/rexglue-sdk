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

#pragma once

#include <array>
#include <memory>
#include <utility>

#include <rex/math.h>
#include "gpu/xenos/host/d3d12/d3d12_provider.h"
#include "gpu/xenos/host/d3d12/d3d12_submission_tracker.h"
#include "gpu/xenos/host/presenter.h"
#include <rex/gpu/surface.h>
#include <rex/types.h>

namespace rex::gpu::d3d12 {

class D3D12OverlayDrawContext final : public gpu::OverlayDrawContext {
 public:
  D3D12OverlayDrawContext(Presenter& presenter, uint32_t render_target_width,
                          uint32_t render_target_height, ID3D12GraphicsCommandList* command_list,
                          UINT64 submission_index_current, UINT64 submission_index_completed)
      : gpu::OverlayDrawContext(&presenter, render_target_width, render_target_height),
        command_list_(command_list),
        submission_index_current_(submission_index_current),
        submission_index_completed_(submission_index_completed) {}

  ID3D12GraphicsCommandList* command_list() const { return command_list_.Get(); }
  UINT64 submission_index_current() const { return submission_index_current_; }
  UINT64 submission_index_completed() const { return submission_index_completed_; }

 private:
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> command_list_;
  UINT64 submission_index_current_;
  UINT64 submission_index_completed_;
};

class D3D12Presenter final : public Presenter {
  struct Key {};

 public:
  explicit D3D12Presenter(Key, HostGpuLossCallback host_gpu_loss_callback,
                          const D3D12Provider& provider)
      : Presenter(std::move(host_gpu_loss_callback)), provider_(provider) {}

  static constexpr DXGI_FORMAT kGuestOutputFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
  static constexpr D3D12_RESOURCE_STATES kGuestOutputInternalState =
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

  // The format used internally by Windows composition.
  static constexpr DXGI_FORMAT kSwapChainFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

  // The callback must use the main direct queue of the provider.
  class D3D12GuestOutputRefreshContext final : public GuestOutputRefreshContext {
   public:
    D3D12GuestOutputRefreshContext(bool& is_8bpc_out_ref, ID3D12Resource* resource)
        : GuestOutputRefreshContext(is_8bpc_out_ref), resource_(resource) {}

    // kGuestOutputFormat, supports UAV. The initial state in the callback is
    // kGuestOutputInternalState, and the callback must also transition it back
    // to kGuestOutputInternalState before finishing.
    ID3D12Resource* resource_uav_capable() const { return resource_.Get(); }

   private:
    Microsoft::WRL::ComPtr<ID3D12Resource> resource_;
  };

  static std::unique_ptr<D3D12Presenter> Create(HostGpuLossCallback host_gpu_loss_callback,
                                                const D3D12Provider& provider) {
    auto presenter =
        std::make_unique<D3D12Presenter>(Key{}, std::move(host_gpu_loss_callback), provider);
    if (!presenter->InitializeSurfaceIndependent()) {
      return nullptr;
    }
    return presenter;
  }

  ~D3D12Presenter();

  const D3D12Provider& provider() const { return provider_; }

  gpu::Surface::TypeFlags GetSupportedSurfaceTypes() const override;

  void AwaitUISubmissionCompletionFromUIThread(UINT64 submission_index) {
    ui_submission_tracker_.AwaitSubmissionCompletion(submission_index);
  }

 protected:
  SurfacePaintConnectResult ConnectOrReconnectPaintingToSurfaceFromUIThread(
      gpu::Surface& new_surface, uint32_t new_surface_width, uint32_t new_surface_height,
      bool was_paintable, bool& is_vsync_implicit_out) override;
  void DisconnectPaintingFromSurfaceFromUIThreadImpl() override;

  bool RefreshGuestOutputImpl(
      uint32_t mailbox_index, uint32_t frontbuffer_width, uint32_t frontbuffer_height,
      std::move_only_function<bool(GuestOutputRefreshContext& context)> refresher,
      bool& is_8bpc_out) override;

  PaintResult PaintAndPresentImpl(bool execute_ui_drawers) override;

 private:
  struct GuestOutputPaintRectangleConstants {
    union {
      struct {
        float x;
        float y;
      };
      float offset[2];
    };
    union {
      struct {
        float width;
        float height;
      };
      float size[2];
    };
  };

  enum class GuestOutputPaintRootParameter : UINT {
    kSource,
    kRectangle,
    kEffectConstants,

    kCount,
  };

  enum GuestOutputPaintRootSignatureIndex : size_t {
    kGuestOutputPaintRootSignatureIndexBilinear,

    kGuestOutputPaintRootSignatureCount,
  };

  static constexpr GuestOutputPaintRootSignatureIndex GetGuestOutputPaintRootSignatureIndex(
      GuestOutputPaintEffect effect) {
    switch (effect) {
      case GuestOutputPaintEffect::kBilinear:
      case GuestOutputPaintEffect::kBilinearDither:
        return kGuestOutputPaintRootSignatureIndexBilinear;
      default:
        REX_UNHANDLED_CASE(effect);
        return kGuestOutputPaintRootSignatureCount;
    }
  }

  struct PaintContext : public rex::NonCopyable {
    explicit PaintContext() = default;

    static constexpr uint32_t kSwapChainBufferCount = 3;

    enum RTVIndex : UINT {
      // Swap chain buffers - updated when creating the swap chain
      // (connection-specific).
      kRTVIndexSwapChainBuffer0,

      kRTVCount = kRTVIndexSwapChainBuffer0 + kSwapChainBufferCount,
    };

    enum ViewIndex : UINT {
      // Guest output textures - indices are the same as in
      // guest_output_resource_paint_refs, and the last usage is tied to them.
      kViewIndexGuestOutput0Srv,

      kViewCount = kViewIndexGuestOutput0Srv + kGuestOutputMailboxSize,
    };

    void AwaitSwapChainUsageCompletion() {
      // Presentation engine usage.
      present_submission_tracker.AwaitAllSubmissionsCompletion();
      // Paint (render target) usage. While the presentation fence is signaled
      // on the same queue, and presentation happens after painting, awaiting
      // anyway for safety to make less assumptions in the architecture.
      paint_submission_tracker.AwaitAllSubmissionsCompletion();
    }

    void DestroySwapChain();

    // Connection-independent.

    // Signaled before presenting.
    D3D12SubmissionTracker paint_submission_tracker;
    // Signaled after presenting.
    D3D12SubmissionTracker present_submission_tracker;

    std::array<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>, kSwapChainBufferCount>
        command_allocators;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> command_list;

    // Descriptor heaps for views of the current resources related to the guest
    // output and to painting, updated either during painting or during
    // connection lifetime management if outdated after awaiting usage
    // completion.
    // RTV heap.
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv_heap;
    // Shader-visible CBV/SRV/UAV heap.
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> view_heap;

    // Refreshed and cleaned up during guest output painting. The first is the
    // paint submission index in which the guest output texture (and its
    // descriptors) was last used, the second is the reference to the texture,
    // which may be null. The indices are not mailbox indices here, rather, if
    // the reference is not in this array yet, the most outdated reference, if
    // needed, is replaced with the new one, awaiting the completion of the last
    // paint usage.
    std::array<std::pair<UINT64, Microsoft::WRL::ComPtr<ID3D12Resource>>, kGuestOutputMailboxSize>
        guest_output_resource_paint_refs;

    // Connection-specific.

    uint32_t swap_chain_width = 0;
    uint32_t swap_chain_height = 0;
    bool swap_chain_allows_tearing = false;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swap_chain;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, kSwapChainBufferCount> swap_chain_buffers;
  };

  bool InitializeSurfaceIndependent();

  const D3D12Provider& provider_;

  // Whether DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING is supported by DXGI (depends in
  // particular on the Windows 10 version and hardware support), primarily for
  // variable refresh rate support.
  bool dxgi_supports_tearing_ = false;

  // Static objects for guest output presentation, used only when painting the
  // main target (can be destroyed only after awaiting main target usage
  // completion).
  std::array<Microsoft::WRL::ComPtr<ID3D12RootSignature>, kGuestOutputPaintRootSignatureCount>
      guest_output_paint_root_signatures_;
  std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>,
             static_cast<size_t>(GuestOutputPaintEffect::kCount)>
      guest_output_paint_pipelines_;

  // The first is the refresher submission tracker fence value at which the
  // guest output texture was last refreshed, the second is the reference to the
  // texture, which may be null. The indices are the mailbox indices.
  std::array<std::pair<UINT64, Microsoft::WRL::ComPtr<ID3D12Resource>>, kGuestOutputMailboxSize>
      guest_output_resources_;
  // The guest output resources are protected by two submission trackers - the
  // refresher ones (for writing to them via the guest_output_resources_
  // references) and the paint one (for presenting it via the
  // paint_context_.guest_output_resource_paint_refs references taken from
  // guest_output_resources_).
  D3D12SubmissionTracker guest_output_resource_refresher_submission_tracker_;

  // UI submission tracker with the submission index that can be given to UI
  // drawers (accessible from the UI thread only, at any time).
  D3D12SubmissionTracker ui_submission_tracker_;

  // Accessible only by painting and by surface connection lifetime management
  // (ConnectOrReconnectPaintingToSurfaceFromUIThread,
  // DisconnectPaintingFromSurfaceFromUIThreadImpl) by the thread doing it, as
  // well as by presenter initialization and shutdown.
  PaintContext paint_context_;
};

}  // namespace rex::gpu::d3d12
