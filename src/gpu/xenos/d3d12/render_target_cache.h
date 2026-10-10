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

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include "gpu/xenos/d3d12/shared_memory.h"
#include "gpu/xenos/d3d12/texture_cache.h"
#include "gpu/xenos/pipeline/render_target/cache.h"
#include "gpu/xenos/util/draw.h"
#include <rex/xenos/xenos.h>
#include <rex/memory.h>
#include "gpu/xenos/host/d3d12/d3d12_cpu_descriptor_pool.h"
#include "gpu/xenos/host/d3d12/d3d12_provider.h"
#include "gpu/xenos/host/d3d12/d3d12_upload_buffer_pool.h"
#include "gpu/xenos/host/d3d12/d3d12_util.h"
namespace rex::gpu::d3d12 {

class D3D12CommandProcessor;

class D3D12RenderTargetCache final : public RenderTargetCache {
 public:
  D3D12RenderTargetCache(const xenos::RegisterFile& register_file, const runtime::Memory& memory,
                         u32 draw_resolution_scale_x, u32 draw_resolution_scale_y,
                         D3D12CommandProcessor& command_processor, bool bindless_resources_used)
      : RenderTargetCache(register_file, memory, draw_resolution_scale_x, draw_resolution_scale_y),
        command_processor_(command_processor),
        bindless_resources_used_(bindless_resources_used) {}
  ~D3D12RenderTargetCache() override;

  /**
   * Creates the EDRAM buffer, descriptors, root signatures and pipelines. Without a path cvar,
   * Intel defaults to ROV as its "always" stencil comparison function fails with driver
   * 27.20.0100.9316 on UHD Graphics 630, breaking Direct3D 9 clears. The AMD shader compiler
   * crashed often with the custom output merger code. Null render target descriptors for gaps
   * must carry a real format, while pipeline states use DXGI_FORMAT_UNKNOWN, which makes the
   * debug layer report a mismatch that needs suppression.
   *
   * @return True when initialization succeeded.
   */
  bool Initialize();
  void Shutdown(bool from_destructor = false);

  void CompletedSubmissionUpdated();
  void BeginSubmission();

  Path GetPath() const override { return path_; }

  bool Update(bool is_rasterization_done, xenos::reg::RB_DEPTHCONTROL normalized_depth_control,
              u32 normalized_color_mask, const Shader& vertex_shader) override;

  void InvalidateCommandListRenderTargets() {
    are_current_command_list_render_targets_valid_ = false;
  }

  bool msaa_2x_supported() const { return msaa_2x_supported_; }

  void WriteEdramRawSRVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle);
  void WriteEdramRawUAVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle);
  void WriteEdramUintPow2SRVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                       u32 element_size_bytes_pow2);
  void WriteEdramUintPow2UAVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                       u32 element_size_bytes_pow2);

  /**
   * Performs the resolve to a shared memory area according to the current register values and
   * clears the render targets if needed. Requires a frame in progress. Descriptors cover the
   * full shared memory without resolution scaling, and the range of the scaled resolve buffer
   * with scaling, as only 128 * 2^20 R32 elements must be addressable
   * (D3D12_REQ_BUFFER_RESOURCE_TEXEL_COUNT_2_TO_EXP).
   *
   * @param memory Guest memory.
   * @param shared_memory Shared memory receiving the resolve.
   * @param texture_cache Texture cache to invalidate over the written range.
   * @param written_address_out Receives the start of the written range.
   * @param written_length_out Receives the length of the written range.
   * @return True when the resolve succeeded.
   */
  bool Resolve(const runtime::Memory& memory, D3D12SharedMemory& shared_memory,
               D3D12TextureCache& texture_cache, u32& written_address_out, u32& written_length_out);

  bool gamma_render_target_as_unorm16() const { return gamma_render_target_as_unorm16_; }

  /**
   * Reports whether fixed16 values are truncated to the -1 to 1 range of R16G16[B16A16]_SNORM
   * instead of the needed -32 to 32. Persistent data does not depend on it, so per title
   * configuration can override it.
   *
   * @return True when fixed16 values are truncated to -1 to 1.
   */
  bool IsFixed16TruncatedToMinus1To1() const;

  bool depth_float24_round() const { return depth_float24_round_; }
  bool depth_float24_convert_in_pixel_shader() const {
    return depth_float24_convert_in_pixel_shader_;
  }

  DXGI_FORMAT GetColorResourceDXGIFormat(xenos::ColorRenderTargetFormat format) const;
  DXGI_FORMAT GetColorDrawDXGIFormat(xenos::ColorRenderTargetFormat format) const;
  DXGI_FORMAT GetColorOwnershipTransferDXGIFormat(xenos::ColorRenderTargetFormat format,
                                                  bool* is_integer_out = nullptr) const;
  static DXGI_FORMAT GetDepthResourceDXGIFormat(xenos::DepthRenderTargetFormat format);
  static DXGI_FORMAT GetDepthDSVDXGIFormat(xenos::DepthRenderTargetFormat format);
  static DXGI_FORMAT GetDepthSRVDepthDXGIFormat(xenos::DepthRenderTargetFormat format);
  static DXGI_FORMAT GetDepthSRVStencilDXGIFormat(xenos::DepthRenderTargetFormat format);

 protected:
  u32 GetMaxRenderTargetWidth() const override { return D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION; }
  u32 GetMaxRenderTargetHeight() const override { return D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION; }

  std::unique_ptr<RenderTarget> CreateRenderTarget(RenderTargetKey key) override;

  bool IsHostDepthEncodingDifferent(xenos::DepthRenderTargetFormat format) const override;

  bool IsGammaFormatHostStorageSeparate() const override;

  void RequestPixelShaderInterlockBarrier() override;

 private:
  /** Ordered by how strong the barrier conditions are. */
  enum class EdramBufferModificationStatus {
    kUnmodified,
    /** Needs a commit before the next overlapping ROV usage. */
    kAsROV,
    /** Needs a commit before any next ROV usage. */
    kAsUAV,
  };
  void TransitionEdramBuffer(D3D12_RESOURCE_STATES new_state);
  void MarkEdramBufferModified(
      EdramBufferModificationStatus modification_status = EdramBufferModificationStatus::kAsUAV);
  void CommitEdramBufferUAVWrites(
      EdramBufferModificationStatus commit_status = EdramBufferModificationStatus::kAsROV);

  D3D12CommandProcessor& command_processor_;
  bool bindless_resources_used_;

  Path path_ = Path::kHostRenderTargets;

  /**
   * With host render targets, an EDRAM sized scratch buffer for guest render target data copied
   * from host render targets during resolves, and for host float32 depth in ownership transfers
   * when the host depth texture and the destination are the same. With rasterizer ordered view,
   * the buffer containing the EDRAM data.
   */
  ID3D12Resource* edram_buffer_ = nullptr;
  D3D12_RESOURCE_STATES edram_buffer_state_;
  EdramBufferModificationStatus edram_buffer_modification_status_ =
      EdramBufferModificationStatus::kUnmodified;

  /** Pre-created SRV and UAV descriptors of the EDRAM buffer in a non shader visible heap. */
  enum class EdramBufferDescriptorIndex : u32 {
    kRawSRV,
    kR32UintSRV,
    kR32G32UintSRV,
    kR32G32B32A32UintSRV,
    kRawUAV,
    kR32UintUAV,
    kR32G32UintUAV,
    kR32G32B32A32UintUAV,

    kCount,
  };
  ID3D12DescriptorHeap* edram_buffer_descriptor_heap_ = nullptr;
  D3D12_CPU_DESCRIPTOR_HANDLE edram_buffer_descriptor_heap_start_;

  /**
   * Resolve copying root signature. Parameter 0 is draw_util::ResolveCopyShaderConstants or its
   * DestRelative, parameter 1 the destination (shared memory or a part of it), parameter 2 the
   * source (EDRAM).
   */
  ID3D12RootSignature* resolve_copy_root_signature_ = nullptr;
  struct ResolveCopyShaderCode {
    const void* unscaled;
    size_t unscaled_size;
    const void* scaled;
    size_t scaled_size;
  };
  static const ResolveCopyShaderCode
      kResolveCopyShaders[static_cast<size_t>(draw_util::ResolveCopyShaderIndex::kCount)];
  ID3D12PipelineState*
      resolve_copy_pipelines_[static_cast<size_t>(draw_util::ResolveCopyShaderIndex::kCount)] = {};

  class D3D12RenderTarget final : public RenderTarget {
   public:
    /**
     * Creates a host render target. descriptor_load_separate is present when the DXGI formats
     * differ for drawing and bit exact loading, which preserves NaN patterns across EDRAM tile
     * ownership transfers in floating point formats and tells the two -1 representations in snorm
     * formats apart.
     *
     * @param key Render target key.
     * @param resource Render target resource.
     * @param descriptor_draw Descriptor for drawing.
     * @param descriptor_load_separate Descriptor for bit exact loading, if the format differs.
     * @param descriptor_srv Texture SRV descriptor.
     * @param descriptor_srv_stencil Stencil texture SRV descriptor.
     * @param resource_state Initial resource state.
     */
    D3D12RenderTarget(RenderTargetKey key, ID3D12Resource* resource,
                      gpu::d3d12::D3D12CpuDescriptorPool::Descriptor&& descriptor_draw,
                      gpu::d3d12::D3D12CpuDescriptorPool::Descriptor&& descriptor_load_separate,
                      gpu::d3d12::D3D12CpuDescriptorPool::Descriptor&& descriptor_srv,
                      gpu::d3d12::D3D12CpuDescriptorPool::Descriptor&& descriptor_srv_stencil,
                      D3D12_RESOURCE_STATES resource_state)
        : RenderTarget(key),
          resource_(resource),
          descriptor_draw_(std::move(descriptor_draw)),
          descriptor_load_separate_(std::move(descriptor_load_separate)),
          descriptor_srv_(std::move(descriptor_srv)),
          descriptor_srv_stencil_(std::move(descriptor_srv_stencil)),
          resource_state_(resource_state) {}

    ID3D12Resource* resource() const { return resource_.Get(); }
    const gpu::d3d12::D3D12CpuDescriptorPool::Descriptor& descriptor_draw() const {
      return descriptor_draw_;
    }
    const gpu::d3d12::D3D12CpuDescriptorPool::Descriptor& descriptor_srv() const {
      return descriptor_srv_;
    }
    const gpu::d3d12::D3D12CpuDescriptorPool::Descriptor& descriptor_srv_stencil() const {
      return descriptor_srv_stencil_;
    }
    const gpu::d3d12::D3D12CpuDescriptorPool::Descriptor& descriptor_load_separate() const {
      return descriptor_load_separate_;
    }

    D3D12_RESOURCE_STATES SetResourceState(D3D12_RESOURCE_STATES new_state) {
      D3D12_RESOURCE_STATES old_state = resource_state_;
      resource_state_ = new_state;
      return old_state;
    }

    u32 temporary_srv_descriptor_index() const { return temporary_srv_descriptor_index_; }
    void SetTemporarySRVDescriptorIndex(u32 index) { temporary_srv_descriptor_index_ = index; }
    u32 temporary_srv_descriptor_index_stencil() const {
      return temporary_srv_descriptor_index_stencil_;
    }
    void SetTemporarySRVDescriptorIndexStencil(u32 index) {
      temporary_srv_descriptor_index_stencil_ = index;
    }
    u32 temporary_sort_index() const { return temporary_sort_index_; }
    void SetTemporarySortIndex(u32 index) { temporary_sort_index_ = index; }

   private:
    Microsoft::WRL::ComPtr<ID3D12Resource> resource_;
    gpu::d3d12::D3D12CpuDescriptorPool::Descriptor descriptor_draw_;
    gpu::d3d12::D3D12CpuDescriptorPool::Descriptor descriptor_load_separate_;
    /** Non shader visible texture SRV descriptors, copied to prepare shader visible ones. */
    gpu::d3d12::D3D12CpuDescriptorPool::Descriptor descriptor_srv_;
    gpu::d3d12::D3D12CpuDescriptorPool::Descriptor descriptor_srv_stencil_;
    D3D12_RESOURCE_STATES resource_state_;
    /** Temporary storage for indices in transfers and dumps. */
    u32 temporary_srv_descriptor_index_ = UINT32_MAX;
    u32 temporary_srv_descriptor_index_stencil_ = UINT32_MAX;
    u32 temporary_sort_index_ = 0;
  };

  enum TransferCBVRegister : u32 {
    kTransferCBVRegisterStencilMask,
    kTransferCBVRegisterAddress,
    kTransferCBVRegisterHostDepthAddress,
  };
  enum TransferSRVRegister : u32 {
    kTransferSRVRegisterColor,
    kTransferSRVRegisterDepth,
    kTransferSRVRegisterStencil,
    kTransferSRVRegisterHostDepth,
    kTransferSRVRegisterCount,
  };
  enum TransferUsedRootParameter : u32 {
    /** Changed 8 times per transfer. */
    kTransferUsedRootParameterStencilMaskConstant,
    kTransferUsedRootParameterColorSRV,
    /** Mutually exclusive with ColorSRV. */
    kTransferUsedRootParameterDepthSRV,
    /** Mutually exclusive with ColorSRV. */
    kTransferUsedRootParameterStencilSRV,
    /** May be the same for different sources. */
    kTransferUsedRootParameterAddressConstant,
    kTransferUsedRootParameterHostDepthSRV,
    kTransferUsedRootParameterHostDepthAddressConstant,
    kTransferUsedRootParameterCount,

    kTransferUsedRootParameterStencilMaskConstantBit =
        static_cast<u32>(1) << kTransferUsedRootParameterStencilMaskConstant,
    kTransferUsedRootParameterColorSRVBit = static_cast<u32>(1)
                                            << kTransferUsedRootParameterColorSRV,
    kTransferUsedRootParameterDepthSRVBit = static_cast<u32>(1)
                                            << kTransferUsedRootParameterDepthSRV,
    kTransferUsedRootParameterStencilSRVBit = static_cast<u32>(1)
                                              << kTransferUsedRootParameterStencilSRV,
    kTransferUsedRootParameterAddressConstantBit = static_cast<u32>(1)
                                                   << kTransferUsedRootParameterAddressConstant,
    kTransferUsedRootParameterHostDepthSRVBit = static_cast<u32>(1)
                                                << kTransferUsedRootParameterHostDepthSRV,
    kTransferUsedRootParameterHostDepthAddressConstantBit =
        static_cast<u32>(1) << kTransferUsedRootParameterHostDepthAddressConstant,

    kTransferUsedRootParametersDescriptorMask =
        kTransferUsedRootParameterColorSRVBit | kTransferUsedRootParameterDepthSRVBit |
        kTransferUsedRootParameterStencilSRVBit | kTransferUsedRootParameterHostDepthSRVBit,
  };
  enum class TransferRootSignatureIndex {
    kColor,
    kDepth,
    kDepthStencil,
    kColorToStencilBit,
    kStencilToStencilBit,
    kColorAndHostDepth,
    kDepthAndHostDepth,
    kDepthStencilAndHostDepth,
    kCount,
  };
  static const u32
      kTransferUsedRootParameters[static_cast<size_t>(TransferRootSignatureIndex::kCount)];
  /**
   * Transfer modes. The modes with host depth use it if it, converted to the guest format,
   * matches the owner source (not modified, keeps host precision), or the title data otherwise
   * (significantly modified, possibly cleared). Stencil for SV_StencilRef always comes from the
   * guest source.
   */
  enum class TransferMode : u32 {
    /** 1 SRV (color texture), source constant. */
    kColorToDepth,
    /** 1 SRV (color texture), source constant. */
    kColorToColor,

    /**
     * 1 or 2 SRVs (depth texture, stencil texture if SV_StencilRef is supported), source
     * constant.
     */
    kDepthToDepth,
    /** 2 SRVs (depth texture, stencil texture), source constant. */
    kDepthToColor,

    /**
     * 1 SRV (color texture), mask constant (most frequently changed, 8 times per transfer),
     * source constant.
     */
    kColorToStencilBit,
    /** 1 SRV (stencil texture), mask constant, source constant. */
    kDepthToStencilBit,

    /** 2 SRVs (color texture, host depth texture or buffer), source and host depth constants. */
    kColorAndHostDepthToDepth,
    /**
     * For different source and destination depth formats. 2 or 3 SRVs (depth texture, stencil
     * texture if SV_StencilRef is supported, host depth texture or buffer), source constant,
     * host depth source constant.
     */
    kDepthAndHostDepthToDepth,

    kCount,
  };
  enum class TransferOutput {
    kColor,
    kDepth,
    /** Uses kTransferCBVRegisterStencilMask. */
    kStencilBit,
  };
  struct TransferModeInfo {
    TransferOutput output;
    TransferRootSignatureIndex root_signature_no_stencil_ref;
    TransferRootSignatureIndex root_signature_with_stencil_ref;
  };
  static const TransferModeInfo kTransferModes[static_cast<size_t>(TransferMode::kCount)];

  union TransferShaderKey {
    u32 key;
    struct {
      xenos::MSAASamples dest_msaa_samples : xenos::kMSAASamplesBits;
      u32 dest_resource_format : xenos::kRenderTargetFormatBits;
      xenos::MSAASamples source_msaa_samples : xenos::kMSAASamplesBits;
      /** Always 1x when host_depth_source_is_copy is true, to avoid duplicate pipelines. */
      xenos::MSAASamples host_depth_source_msaa_samples : xenos::kMSAASamplesBits;
      u32 source_resource_format : xenos::kRenderTargetFormatBits;
      /**
       * True when host depth, if also fetched, is pre-copied to the EDRAM buffer. As a scratch
       * buffer, it starts at destination (0, 0), its tiles are laid out linearly with the pitch of
       * the original render target, and 40 sample columns are not swapped, which only the color
       * source does.
       */
      u32 host_depth_source_is_copy : 1;

      // Last because it affects the root signature, extended by a stencil SRV for depth.
      static_assert(static_cast<size_t>(TransferMode::kCount) <= (static_cast<size_t>(1) << 3));
      TransferMode mode : 3;
    };

    TransferShaderKey() : key(0) { REX_STATIC_ASSERT_SIZE(*this, sizeof(key)); }

    struct Hasher {
      size_t operator()(const TransferShaderKey& key) const { return std::hash<u32>{}(key.key); }
    };
    bool operator==(const TransferShaderKey& other_key) const { return key == other_key.key; }
    bool operator!=(const TransferShaderKey& other_key) const { return !(*this == other_key); }
    bool operator<(const TransferShaderKey& other_key) const { return key < other_key.key; }
  };

  union TransferAddressConstant {
    u32 constant;
    struct {
      // All in tiles.
      u32 dest_pitch : xenos::kEDRAMPitchTilesBits;
      u32 source_pitch : xenos::kEDRAMPitchTilesBits;
      /**
       * Destination base in tiles minus source base in tiles, as this transforms the coordinate
       * system, not the addresses. 1 extra bit for the signed difference between two EDRAM bases.
       * 0 for host_depth_source_is_copy, where destination equals source.
       */
      i32 source_to_dest : xenos::kEDRAMBaseTilesBits + 1;
    };
    TransferAddressConstant() : constant(0) { REX_STATIC_ASSERT_SIZE(*this, sizeof(constant)); }
    bool operator==(const TransferAddressConstant& other_constant) const {
      return constant == other_constant.constant;
    }
    bool operator!=(const TransferAddressConstant& other_constant) const {
      return !(*this == other_constant);
    }
  };

  struct TransferInvocation {
    Transfer transfer;
    TransferShaderKey shader_key;
    TransferInvocation(const Transfer& transfer, const TransferShaderKey& shader_key)
        : transfer(transfer), shader_key(shader_key) {}
    /**
     * Reports whether this invocation sorts before another. Sorts by shader key first to reduce
     * pipeline state switches, then by source and start tiles. Host depth render targets are
     * ignored, as their existence is part of the shader key.
     *
     * @param other_invocation Invocation to compare with.
     * @return True when this invocation sorts first.
     */
    bool operator<(const TransferInvocation& other_invocation) const {
      if (shader_key != other_invocation.shader_key) {
        return shader_key < other_invocation.shader_key;
      }
      REX_ASSERT(transfer.source != nullptr);
      REX_ASSERT(other_invocation.transfer.source != nullptr);
      u32 source_index =
          static_cast<const D3D12RenderTarget*>(transfer.source)->temporary_sort_index();
      u32 other_source_index =
          static_cast<const D3D12RenderTarget*>(other_invocation.transfer.source)
              ->temporary_sort_index();
      if (source_index != other_source_index) {
        return source_index < other_source_index;
      }
      return transfer.start_tiles < other_invocation.transfer.start_tiles;
    }
    bool CanBeMergedIntoOneDraw(const TransferInvocation& other_invocation) const {
      return shader_key == other_invocation.shader_key &&
             transfer.AreSourcesSame(other_invocation.transfer);
    }
  };

  enum {
    kHostDepthStoreRootParameterConstants,
    kHostDepthStoreRootParameterSource,
    kHostDepthStoreRootParameterDest,
    kHostDepthStoreRootParameterCount,
  };

  union DumpPipelineKey {
    u32 key;
    struct {
      xenos::MSAASamples msaa_samples : 2;
      u32 resource_format : 4;
      // Last because it affects the root signature, extended by a stencil SRV for depth.
      u32 is_depth : 1;
    };

    DumpPipelineKey() : key(0) { REX_STATIC_ASSERT_SIZE(*this, sizeof(key)); }

    struct Hasher {
      size_t operator()(const DumpPipelineKey& key) const { return std::hash<u32>{}(key.key); }
    };
    bool operator==(const DumpPipelineKey& other_key) const { return key == other_key.key; }
    bool operator!=(const DumpPipelineKey& other_key) const { return !(*this == other_key); }
    bool operator<(const DumpPipelineKey& other_key) const { return key < other_key.key; }

    xenos::ColorRenderTargetFormat GetColorFormat() const {
      REX_ASSERT(!is_depth);
      return xenos::ColorRenderTargetFormat(resource_format);
    }
    xenos::DepthRenderTargetFormat GetDepthFormat() const {
      REX_ASSERT(is_depth);
      return xenos::DepthRenderTargetFormat(resource_format);
    }
  };

  union DumpOffsets {
    u32 offsets;
    struct {
      /** May exceed the EDRAM tile count when EDRAM addressing wraps, hence 1 extra bit. */
      u32 dispatch_first_tile : xenos::kEDRAMBaseTilesBits + 1;
      u32 source_base_tiles : xenos::kEDRAMBaseTilesBits;
    };
    DumpOffsets() : offsets(0) { REX_STATIC_ASSERT_SIZE(*this, sizeof(offsets)); }
    bool operator==(const DumpOffsets& other_offsets) const {
      return offsets == other_offsets.offsets;
    }
    bool operator!=(const DumpOffsets& other_offsets) const { return !(*this == other_offsets); }
  };

  union DumpPitches {
    u32 pitches;
    struct {
      // Both in tiles.
      u32 dest_pitch : xenos::kEDRAMPitchTilesBits;
      u32 source_pitch : xenos::kEDRAMPitchTilesBits;
    };
    DumpPitches() : pitches(0) { REX_STATIC_ASSERT_SIZE(*this, sizeof(pitches)); }
    bool operator==(const DumpPitches& other_pitches) const {
      return pitches == other_pitches.pitches;
    }
    bool operator!=(const DumpPitches& other_pitches) const { return !(*this == other_pitches); }
  };

  enum DumpCbuffer : u32 {
    kDumpCbufferOffsets,
    kDumpCbufferPitches,
    kDumpCbufferCount,
  };

  enum DumpRootParameter : u32 {
    /** May be changed multiple times for the same source. */
    kDumpRootParameterOffsets,
    /** One resolve may need multiple sources. */
    kDumpRootParameterSource,

    /** May be different for different sources. */
    kDumpRootParameterColorPitches = kDumpRootParameterSource + 1,
    /** Only changed between 32bpp and 64bpp. */
    kDumpRootParameterColorEdram,

    kDumpRootParameterColorCount,

    /**
     * Changes as often as the source. The command processor cannot contiguously allocate
     * multiple descriptors with bindless.
     */
    kDumpRootParameterDepthStencil = kDumpRootParameterSource + 1,
    kDumpRootParameterDepthPitches,
    kDumpRootParameterDepthEdram,

    kDumpRootParameterDepthCount,
  };

  struct DumpInvocation {
    ResolveCopyDumpRectangle rectangle;
    DumpPipelineKey pipeline_key;
    DumpInvocation(const ResolveCopyDumpRectangle& rectangle, const DumpPipelineKey& pipeline_key)
        : rectangle(rectangle), pipeline_key(pipeline_key) {}
    /**
     * Reports whether this invocation sorts before another. Sorts by pipeline key first to reduce
     * pipeline state switches, then by render target and rows.
     *
     * @param other_invocation Invocation to compare with.
     * @return True when this invocation sorts first.
     */
    bool operator<(const DumpInvocation& other_invocation) const {
      if (pipeline_key != other_invocation.pipeline_key) {
        return pipeline_key < other_invocation.pipeline_key;
      }
      REX_ASSERT(rectangle.render_target != nullptr);
      u32 render_target_index =
          static_cast<const D3D12RenderTarget*>(rectangle.render_target)->temporary_sort_index();
      const ResolveCopyDumpRectangle& other_rectangle = other_invocation.rectangle;
      u32 other_render_target_index =
          static_cast<const D3D12RenderTarget*>(other_rectangle.render_target)
              ->temporary_sort_index();
      if (render_target_index != other_render_target_index) {
        return render_target_index < other_render_target_index;
      }
      if (rectangle.row_first != other_rectangle.row_first) {
        return rectangle.row_first < other_rectangle.row_first;
      }
      return rectangle.row_first_start < other_rectangle.row_first_start;
    }
  };

  struct DirectResolvePushConstants {
    draw_util::ResolveCopyShaderConstants resolve;
    u32 source_base_tiles;
    u32 source_pitch_tiles;
    u32 dispatch_first_tile;
  };

  struct DirectResolvePipelineKey {
    DumpPipelineKey dump_pipeline_key;
    draw_util::ResolveCopyShaderIndex copy_shader;
    bool draw_resolution_scaled;

    u64 packed() const {
      return static_cast<u64>(dump_pipeline_key.key) |
             (static_cast<u64>(static_cast<size_t>(copy_shader)) << 32) |
             (static_cast<u64>(draw_resolution_scaled ? 1 : 0) << 40);
    }
    struct Hasher {
      size_t operator()(const DirectResolvePipelineKey& key) const {
        return std::hash<u64>{}(key.packed());
      }
    };
    bool operator==(const DirectResolvePipelineKey& other_key) const {
      return packed() == other_key.packed();
    }
  };

  /**
   * Returns the transfer pipelines for a shader key, creating them on first use. 40 sample columns
   * are swapped when the destination is depth or a stencil bit as opposed to a color source, and
   * when the source is depth or stencil as opposed to a color destination. A 64bpp sample is two
   * 32bpp halves in adjacent samples at 4x, or adjacent pixels at 1x and 2x. The first sample bit
   * at 4x is the horizontal sample and the second the vertical. At 2x, native 2x has the top at 1
   * and the bottom at 0, while 2x as 4x has the top at 0 and the bottom at 3. Registers before the
   * source load are r0.xy as the pixel index within the destination 32bpp tile, r0.z as the 32bpp
   * tile index relative to the destination base, r0.w as the needed 32bpp half of 64bpp data for
   * 64bpp to 32bpp, r1.xy as the pixel index within the source 32bpp tile and r1.z as the sample
   * index within the source pixel for 2x and 4x. The source loads to r1, or for 32bpp to 64bpp
   * the first dword to r0, as addressing is no longer needed for color, and the second to r1.
   * Depth loads to w before stencil so the stencil coordinates survive, stencil to x and color to
   * x through w. A single sampled source zeroes r1.z as the LOD. 64bpp color is built from two
   * 32 bit samples in r0 and r1 or 1 64bpp sample in r1, with r2.x as a temp. 64bpp and 16 bit
   * per component formats transfer as integers for safe handling of NaNs and -32768 / -32767.
   * D24S8 depth converts by rescaling 0...0xFFFFFF to 0...0x1000000 as true float division
   * followed by multiplication does on x86-64 MSVC with default SSE rounding, which adds 1 from
   * 0x800000, then biasing the exponent, as multiplying by 1.0 / 0xFFFFFF is wrong for 0xC00000.
   *
   * @param key Transfer shader key.
   * @return A pointer to 1 pipeline writing color or depth (or stencil via SV_StencilRef), a
   *         pointer to 8 pipelines writing stencil by discarding samples depending on whether
   *         they have the bit from 1 << 0 to 1 << 7 set, or null if creation failed.
   */
  ID3D12PipelineState* const* GetOrCreateTransferPipelines(TransferShaderKey key);

  /**
   * Performs ownership transfers for render targets. Each render target or vector may be null or
   * empty when there is nothing to do for it. Barriers are inserted in one place where possible,
   * so without cross copying between the render targets none are needed between transfers. With
   * cross copying, the destination state is chosen if its use precedes the source use, otherwise
   * the source state, matching the transfer order and avoiding a useless switch back and forth.
   *
   * @param render_target_count Number of render targets.
   * @param render_targets Render targets to transfer to.
   * @param render_target_transfers Transfers per render target.
   * @param render_target_resolve_clear_values Resolve clear values per render target.
   * @param resolve_clear_rectangle Rectangle provided by PrepareHostRenderTargetsResolveClear,
   *        which does the needed size bound checks.
   */
  void PerformTransfersAndResolveClears(
      u32 render_target_count, RenderTarget* const* render_targets,
      const std::vector<Transfer>* render_target_transfers,
      const u64* render_target_resolve_clear_values = nullptr,
      const Transfer::Rectangle* resolve_clear_rectangle = nullptr);

  /**
   * Sets the render targets of the command list.
   *
   * @param depth_and_color_render_targets Array of 1 + xenos::kMaxColorRenderTargets render
   *        targets, depth first, then color.
   */
  void SetCommandListRenderTargets(RenderTarget* const* depth_and_color_render_targets);

  /**
   * Returns the compute pipeline dumping a host render target to the EDRAM buffer, creating it
   * on first use. The
   * group size has no strict dependency on the dump, to simplify calculations especially with
   * resolution scaling, so the division is manual as the group size is limited. An integer
   * multiple of it must be 80x16 samples, no larger, for 32bpp, or 40x16 samples for 64bpp, as
   * only half of a tile pair may need dumping. The Direct3D 11 group size limit is 1024, which
   * fits 40x16 but not 80x16. The exact addressing of 64bpp render targets relative to 32bpp is
   * unknown, so 64bpp tiles are treated as 40x16 samples rather than 80x16 to simplify texture
   * addressing.
   *
   * @param key Dump pipeline key.
   * @return The pipeline, or null if creation failed.
   */
  ID3D12PipelineState* GetOrCreateDumpPipeline(DumpPipelineKey key);
  ID3D12PipelineState* GetOrCreateDirectResolvePipeline(DirectResolvePipelineKey key);
  bool TryResolveCopyDirectly(const draw_util::ResolveInfo& resolve_info,
                              draw_util::ResolveCopyShaderIndex copy_shader,
                              bool draw_resolution_scaled);

  /**
   * Writes contents of host render targets within rectangles from
   * ResolveInfo::GetCopyEdramTileSpan to edram_buffer_.
   *
   * @param dump_base First EDRAM tile to dump.
   * @param dump_row_length_used Used length of a row in tiles.
   * @param dump_rows Number of rows.
   * @param dump_pitch Pitch in tiles.
   * @return True when the dump succeeded.
   */
  bool DumpRenderTargets(u32 dump_base, u32 dump_row_length_used, u32 dump_rows, u32 dump_pitch);

  bool use_stencil_reference_output_ = false;

  bool gamma_render_target_as_unorm16_ = false;

  bool depth_float24_round_ = false;
  bool depth_float24_convert_in_pixel_shader_ = false;

  bool msaa_2x_supported_ = false;

  std::shared_ptr<gpu::d3d12::D3D12CpuDescriptorPool> descriptor_pool_color_;
  std::shared_ptr<gpu::d3d12::D3D12CpuDescriptorPool> descriptor_pool_depth_;
  std::shared_ptr<gpu::d3d12::D3D12CpuDescriptorPool> descriptor_pool_srv_;
  gpu::d3d12::D3D12CpuDescriptorPool::Descriptor null_rtv_descriptor_ss_;
  gpu::d3d12::D3D12CpuDescriptorPool::Descriptor null_rtv_descriptor_ms_;

  const RenderTarget* current_command_list_render_targets_[1 + xenos::kMaxColorRenderTargets];
  bool are_current_command_list_render_targets_valid_ = false;

  /**
   * Temporary storage for descriptors of PerformTransfersAndResolveClears and DumpRenderTargets.
   */
  std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> current_temporary_descriptors_cpu_;
  std::vector<gpu::d3d12::util::DescriptorCpuGpuHandlePair> current_temporary_descriptors_gpu_;

  ID3D12RootSignature* host_depth_store_root_signature_ = nullptr;
  ID3D12PipelineState*
      host_depth_store_pipelines_[static_cast<size_t>(xenos::MSAASamples::k4X) + 1] = {};

  std::unique_ptr<gpu::d3d12::D3D12UploadBufferPool> transfer_vertex_buffer_pool_;

  ID3D12RootSignature*
      transfer_root_signatures_[static_cast<size_t>(TransferRootSignatureIndex::kCount)] = {};
  std::unordered_map<TransferShaderKey, ID3D12PipelineState*, TransferShaderKey::Hasher>
      transfer_pipelines_;
  std::unordered_map<TransferShaderKey, std::array<ID3D12PipelineState*, 8>,
                     TransferShaderKey::Hasher>
      transfer_stencil_bit_pipelines_;

  /** Temporary storage for PerformTransfersAndResolveClears. */
  std::vector<TransferInvocation> current_transfer_invocations_;

  /** Temporary storage for DumpRenderTargets. */
  std::vector<ResolveCopyDumpRectangle> dump_rectangles_;
  std::vector<DumpInvocation> dump_invocations_;
  std::vector<ResolveCopyDispatch> direct_resolve_dispatches_;

  ID3D12RootSignature* dump_root_signature_color_ = nullptr;
  ID3D12RootSignature* dump_root_signature_depth_ = nullptr;
  /** Compute pipelines copying host render targets to the EDRAM buffer, null if creation failed. */
  std::unordered_map<DumpPipelineKey, ID3D12PipelineState*, DumpPipelineKey::Hasher>
      dump_pipelines_;
  ID3D12RootSignature* direct_resolve_root_signature_color_ = nullptr;
  ID3D12RootSignature* direct_resolve_root_signature_depth_ = nullptr;
  std::unordered_map<DirectResolvePipelineKey, ID3D12PipelineState*,
                     DirectResolvePipelineKey::Hasher>
      direct_resolve_pipelines_;
  u64 direct_resolve_attempt_count_ = 0;
  u64 direct_resolve_success_count_ = 0;
  u64 direct_resolve_fallback_count_ = 0;

  /** Parameter 0 is 2 root constants (red, green). */
  ID3D12RootSignature* uint32_rtv_clear_root_signature_ = nullptr;
  /** Indexed by [32 or 32_32][MSAA samples]. */
  ID3D12PipelineState*
      uint32_rtv_clear_pipelines_[2][static_cast<size_t>(xenos::MSAASamples::k4X) + 1] = {};

  std::vector<Transfer> clear_transfers_[2];

  /** Temporary storage for DXBC building. */
  std::vector<u32> built_shader_;

  ID3D12RootSignature* resolve_rov_clear_root_signature_ = nullptr;
  /** Clears 32bpp color or depth. */
  ID3D12PipelineState* resolve_rov_clear_32bpp_pipeline_ = nullptr;
  /** Clears 64bpp color. */
  ID3D12PipelineState* resolve_rov_clear_64bpp_pipeline_ = nullptr;
};

}  // namespace rex::gpu::d3d12
