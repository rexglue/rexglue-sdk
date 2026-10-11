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
#include <rex/move_only_function.h>

namespace rex::gpu::d3d12 {

class D3D12OverlayDrawContext final : public gpu::OverlayDrawContext {
 public:
  D3D12OverlayDrawContext(Presenter& presenter, u32 render_target_width, u32 render_target_height,
                          ID3D12GraphicsCommandList* command_list, UINT64 submission_index_current,
                          UINT64 submission_index_completed)
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
  explicit D3D12Presenter(Key, HostGPULossCallback host_gpu_loss_callback,
                          const D3D12Provider& provider)
      : Presenter(std::move(host_gpu_loss_callback)), provider_(provider) {}

  static constexpr DXGI_FORMAT kGuestOutputFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
  static constexpr D3D12_RESOURCE_STATES kGuestOutputInternalState =
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

  /** Format used internally by Windows composition. */
  static constexpr DXGI_FORMAT kSwapChainFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

  /** Refresh context whose callback must use the main direct queue of the provider. */
  class D3D12GuestOutputRefreshContext final : public GuestOutputRefreshContext {
   public:
    D3D12GuestOutputRefreshContext(bool& is_8bpc_out_ref, ID3D12Resource* resource)
        : GuestOutputRefreshContext(is_8bpc_out_ref), resource_(resource) {}

    /**
     * Returns the UAV capable output resource in kGuestOutputFormat.
     *
     * The callback starts in kGuestOutputInternalState and must transition back to it before
     * finishing.
     *
     * @return The output resource.
     */
    ID3D12Resource* resource_uav_capable() const { return resource_.Get(); }

   private:
    Microsoft::WRL::ComPtr<ID3D12Resource> resource_;
  };

  static std::unique_ptr<D3D12Presenter> Create(HostGPULossCallback host_gpu_loss_callback,
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
      gpu::Surface& new_surface, u32 new_surface_width, u32 new_surface_height, bool was_paintable,
      bool& is_vsync_implicit_out) override;
  void DisconnectPaintingFromSurfaceFromUIThreadImpl() override;

  bool RefreshGuestOutputImpl(
      u32 mailbox_index, u32 frontbuffer_width, u32 frontbuffer_height,
      rex::MoveOnlyFunction<bool(GuestOutputRefreshContext& context)> refresher,
      bool& is_8bpc_out) override;

  PaintResult PaintAndPresentImpl(bool execute_ui_drawers) override;

 private:
  struct GuestOutputPaintRectangleConstants {
    union {
      struct {
        f32 x;
        f32 y;
      };
      f32 offset[2];
    };
    union {
      struct {
        f32 width;
        f32 height;
      };
      f32 size[2];
    };
  };

  enum class GuestOutputPaintRootParameter : u8 {
    kSource,
    kRectangle,
    kEffectConstants,

    kCount,
  };

  enum GuestOutputPaintRootSignatureIndex : u8 {
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

    static constexpr u32 kSwapChainBufferCount = 3;

    enum RTVIndex : u8 {
      kRTVIndexSwapChainBuffer0,

      kRTVCount = kRTVIndexSwapChainBuffer0 + kSwapChainBufferCount,
    };

    enum ViewIndex : u8 {
      kViewIndexGuestOutput0Srv,

      kViewCount = kViewIndexGuestOutput0Srv + kGuestOutputMailboxSize,
    };

    void AwaitSwapChainUsageCompletion() {
      present_submission_tracker.AwaitAllSubmissionsCompletion();
      paint_submission_tracker.AwaitAllSubmissionsCompletion();
    }

    void DestroySwapChain();

    /** Signaled before presenting. */
    D3D12SubmissionTracker paint_submission_tracker;
    /** Signaled after presenting. */
    D3D12SubmissionTracker present_submission_tracker;

    std::array<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>, kSwapChainBufferCount>
        command_allocators;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> command_list;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv_heap;
    /** Shader visible CBV/SRV/UAV heap. */
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> view_heap;

    /**
     * Pairs of the paint submission index of last use and the nullable guest output texture.
     *
     * Indices are not mailbox indices. A reference missing from the array replaces the most
     * outdated one after awaiting its last paint usage.
     */
    std::array<std::pair<UINT64, Microsoft::WRL::ComPtr<ID3D12Resource>>, kGuestOutputMailboxSize>
        guest_output_resource_paint_refs;

    u32 swap_chain_width = 0;
    u32 swap_chain_height = 0;
    bool swap_chain_allows_tearing = false;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swap_chain;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, kSwapChainBufferCount> swap_chain_buffers;
  };

  bool InitializeSurfaceIndependent();

  const D3D12Provider& provider_;

  /** True when DXGI supports DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING, used for variable refresh rate. */
  bool dxgi_supports_tearing_ = false;

  /** Destroyed only after awaiting main target usage completion. */
  std::array<Microsoft::WRL::ComPtr<ID3D12RootSignature>, kGuestOutputPaintRootSignatureCount>
      guest_output_paint_root_signatures_;
  std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>,
             static_cast<size_t>(GuestOutputPaintEffect::kCount)>
      guest_output_paint_pipelines_;

  /**
   * Pairs of the refresher fence value of the last refresh and the nullable guest output texture.
   *
   * Indexed by mailbox index.
   */
  std::array<std::pair<UINT64, Microsoft::WRL::ComPtr<ID3D12Resource>>, kGuestOutputMailboxSize>
      guest_output_resources_;
  D3D12SubmissionTracker guest_output_resource_refresher_submission_tracker_;

  /** Submission tracker whose indices go to UI drawers, accessed from the UI thread only. */
  D3D12SubmissionTracker ui_submission_tracker_;

  /**
   * Accessed only by painting, surface connection management, and presenter initialization and
   * shutdown, from the thread doing it.
   */
  PaintContext paint_context_;
};

}  // namespace rex::gpu::d3d12
