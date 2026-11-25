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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <rex/assert.h>
#include <rex/dbg.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/vulkan/command_processor.h>
#include <rex/graphics/vulkan/deferred_command_buffer.h>
#include <rex/math.h>

namespace rex::graphics::vulkan {

DeferredCommandBuffer::DeferredCommandBuffer(const VulkanCommandProcessor& command_processor,
                                             size_t initial_size)
    : command_processor_(command_processor) {
  size_t first_block = std::max(kBlockElements, initial_size / sizeof(uintmax_t));
  blocks_[0] = std::make_unique<uintmax_t[]>(first_block);
  block_capacity_[0] = first_block;
}

void DeferredCommandBuffer::Reset() {
  write_block_ = 0;
  write_offset_ = 0;
  timestamp_category_marked_ = kNoTimestampCategory;
  published_.store(0, std::memory_order_relaxed);
}

void DeferredCommandBuffer::Publish(std::memory_order order) {
  // Release by default (a plain store on x86): ExecutePublished reads it with
  // acquire. A caller that then reads another thread's flag (Dekker-style) adds
  // its own seq_cst fence, see VulkanCommandProcessor::PublishSubmitJob.
  published_.store((uint64_t(write_block_) << kPublishedBlockShift) | uint64_t(write_offset_),
                   order);
}

void DeferredCommandBuffer::Execute(VkCommandBuffer command_buffer) {
  Publish();
  Cursor cursor;
  ExecutePublished(command_buffer, cursor);
}

bool DeferredCommandBuffer::ExecutePublished(VkCommandBuffer command_buffer,
                                             Cursor& cursor) const {
  const uint64_t published = published_.load(std::memory_order_acquire);
  const uint32_t published_block = uint32_t(published >> kPublishedBlockShift);
  const size_t published_offset = size_t(published & kPublishedOffsetMask);
  bool executed = false;
  while (cursor.block < published_block ||
         (cursor.block == published_block && cursor.offset < published_offset)) {
    const size_t end =
        cursor.block < published_block ? block_end_[cursor.block] : published_offset;
    if (end > cursor.offset) {
      ExecuteRange(command_buffer, blocks_[cursor.block].get() + cursor.offset,
                   end - cursor.offset);
      executed = true;
    }
    if (cursor.block < published_block) {
      ++cursor.block;
      cursor.offset = 0;
    } else {
      cursor.offset = end;
    }
  }
  return executed;
}

void DeferredCommandBuffer::ExecuteRange(VkCommandBuffer command_buffer, const uintmax_t* stream,
                                         size_t stream_remaining) const {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  const ui::vulkan::VulkanDevice::Functions& dfn =
      command_processor_.GetVulkanDevice()->functions();
  while (stream_remaining) {
    const CommandHeader& header = *reinterpret_cast<const CommandHeader*>(stream);
    stream += kCommandHeaderSizeElements;
    stream_remaining -= kCommandHeaderSizeElements;

    switch (header.command) {
      case Command::kTimestampMarker: {
        if (timestamp_sink_) {
          timestamp_sink_->WriteTimestamp(
              command_buffer, reinterpret_cast<const ArgsTimestampMarker*>(stream)->category);
        }
      } break;

      case Command::kVkBeginRenderPass: {
        auto& args = *reinterpret_cast<const ArgsVkBeginRenderPass*>(stream);
        size_t offset_bytes = sizeof(ArgsVkBeginRenderPass);
        VkRenderPassBeginInfo render_pass_begin_info;
        render_pass_begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        render_pass_begin_info.pNext = nullptr;
        render_pass_begin_info.renderPass = args.render_pass;
        render_pass_begin_info.framebuffer = args.framebuffer;
        render_pass_begin_info.renderArea = args.render_area;
        render_pass_begin_info.clearValueCount = args.clear_value_count;
        if (render_pass_begin_info.clearValueCount) {
          offset_bytes = rex::align(offset_bytes, alignof(VkClearValue));
          render_pass_begin_info.pClearValues = reinterpret_cast<const VkClearValue*>(
              reinterpret_cast<const uint8_t*>(stream) + offset_bytes);
          offset_bytes += sizeof(VkClearValue) * render_pass_begin_info.clearValueCount;
        } else {
          render_pass_begin_info.pClearValues = nullptr;
        }
        dfn.vkCmdBeginRenderPass(command_buffer, &render_pass_begin_info, args.contents);
      } break;

      case Command::kVkBeginQuery: {
        auto& args = *reinterpret_cast<const ArgsVkBeginQuery*>(stream);
        dfn.vkCmdBeginQuery(command_buffer, args.query_pool, args.query, args.flags);
      } break;

      case Command::kVkBindDescriptorSets: {
        auto& args = *reinterpret_cast<const ArgsVkBindDescriptorSets*>(stream);
        size_t offset_bytes =
            rex::align(sizeof(ArgsVkBindDescriptorSets), alignof(VkDescriptorSet));
        const VkDescriptorSet* descriptor_sets = reinterpret_cast<const VkDescriptorSet*>(
            reinterpret_cast<const uint8_t*>(stream) + offset_bytes);
        offset_bytes += sizeof(VkDescriptorSet) * args.descriptor_set_count;
        const uint32_t* dynamic_offsets = nullptr;
        if (args.dynamic_offset_count) {
          offset_bytes = rex::align(offset_bytes, alignof(uint32_t));
          dynamic_offsets = reinterpret_cast<const uint32_t*>(
              reinterpret_cast<const uint8_t*>(stream) + offset_bytes);
          offset_bytes += sizeof(uint32_t) * args.dynamic_offset_count;
        }
        dfn.vkCmdBindDescriptorSets(command_buffer, args.pipeline_bind_point, args.layout,
                                    args.first_set, args.descriptor_set_count, descriptor_sets,
                                    args.dynamic_offset_count, dynamic_offsets);
      } break;

      case Command::kVkBindIndexBuffer: {
        auto& args = *reinterpret_cast<const ArgsVkBindIndexBuffer*>(stream);
        dfn.vkCmdBindIndexBuffer(command_buffer, args.buffer, args.offset, args.index_type);
      } break;

      case Command::kVkBindPipeline: {
        auto& args = *reinterpret_cast<const ArgsVkBindPipeline*>(stream);
        dfn.vkCmdBindPipeline(command_buffer, args.pipeline_bind_point, args.pipeline);
      } break;

      case Command::kVkBindVertexBuffers: {
        auto& args = *reinterpret_cast<const ArgsVkBindVertexBuffers*>(stream);
        size_t offset_bytes = rex::align(sizeof(ArgsVkBindVertexBuffers), alignof(VkBuffer));
        const VkBuffer* buffers = reinterpret_cast<const VkBuffer*>(
            reinterpret_cast<const uint8_t*>(stream) + offset_bytes);
        offset_bytes =
            rex::align(offset_bytes + sizeof(VkBuffer) * args.binding_count, alignof(VkDeviceSize));
        const VkDeviceSize* offsets = reinterpret_cast<const VkDeviceSize*>(
            reinterpret_cast<const uint8_t*>(stream) + offset_bytes);
        dfn.vkCmdBindVertexBuffers(command_buffer, args.first_binding, args.binding_count, buffers,
                                   offsets);
      } break;

      case Command::kVkClearAttachments: {
        auto& args = *reinterpret_cast<const ArgsVkClearAttachments*>(stream);
        size_t offset_bytes =
            rex::align(sizeof(ArgsVkClearAttachments), alignof(VkClearAttachment));
        const VkClearAttachment* attachments = reinterpret_cast<const VkClearAttachment*>(
            reinterpret_cast<const uint8_t*>(stream) + offset_bytes);
        offset_bytes = rex::align(offset_bytes + sizeof(VkClearAttachment) * args.attachment_count,
                                  alignof(VkClearRect));
        const VkClearRect* rects = reinterpret_cast<const VkClearRect*>(
            reinterpret_cast<const uint8_t*>(stream) + offset_bytes);
        dfn.vkCmdClearAttachments(command_buffer, args.attachment_count, attachments,
                                  args.rect_count, rects);
      } break;

      case Command::kVkClearColorImage: {
        auto& args = *reinterpret_cast<const ArgsVkClearColorImage*>(stream);
        dfn.vkCmdClearColorImage(
            command_buffer, args.image, args.image_layout, &args.color, args.range_count,
            reinterpret_cast<const VkImageSubresourceRange*>(
                reinterpret_cast<const uint8_t*>(stream) +
                rex::align(sizeof(ArgsVkClearColorImage), alignof(VkImageSubresourceRange))));
      } break;

      case Command::kVkCopyBuffer: {
        auto& args = *reinterpret_cast<const ArgsVkCopyBuffer*>(stream);
        dfn.vkCmdCopyBuffer(command_buffer, args.src_buffer, args.dst_buffer, args.region_count,
                            reinterpret_cast<const VkBufferCopy*>(
                                reinterpret_cast<const uint8_t*>(stream) +
                                rex::align(sizeof(ArgsVkCopyBuffer), alignof(VkBufferCopy))));
      } break;

      case Command::kVkCopyBufferToImage: {
        auto& args = *reinterpret_cast<const ArgsVkCopyBufferToImage*>(stream);
        dfn.vkCmdCopyBufferToImage(
            command_buffer, args.src_buffer, args.dst_image, args.dst_image_layout,
            args.region_count,
            reinterpret_cast<const VkBufferImageCopy*>(
                reinterpret_cast<const uint8_t*>(stream) +
                rex::align(sizeof(ArgsVkCopyBufferToImage), alignof(VkBufferImageCopy))));
      } break;

      case Command::kVkCopyOrResolveImage: {
        auto& args = *reinterpret_cast<const ArgsVkCopyOrResolveImage*>(stream);
        if (args.mode == ImageCopyMode::kBlit) {
          VkImageBlit region;
          region.srcSubresource = args.region.srcSubresource;
          region.srcOffsets[0] = args.region.srcOffset;
          region.srcOffsets[1].x = args.region.srcOffset.x + int32_t(args.region.extent.width);
          region.srcOffsets[1].y = args.region.srcOffset.y + int32_t(args.region.extent.height);
          region.srcOffsets[1].z = 1;
          region.dstSubresource = args.region.dstSubresource;
          region.dstOffsets[0] = args.region.dstOffset;
          region.dstOffsets[1].x = args.region.dstOffset.x + int32_t(args.region.extent.width);
          region.dstOffsets[1].y = args.region.dstOffset.y + int32_t(args.region.extent.height);
          region.dstOffsets[1].z = 1;
          dfn.vkCmdBlitImage(command_buffer, args.src_image, args.src_image_layout, args.dst_image,
                             args.dst_image_layout, 1, &region, VK_FILTER_NEAREST);
        } else if (args.mode == ImageCopyMode::kResolve) {
          VkImageResolve region;
          region.srcSubresource = args.region.srcSubresource;
          region.srcOffset = args.region.srcOffset;
          region.dstSubresource = args.region.dstSubresource;
          region.dstOffset = args.region.dstOffset;
          region.extent = args.region.extent;
          dfn.vkCmdResolveImage(command_buffer, args.src_image, args.src_image_layout,
                                args.dst_image, args.dst_image_layout, 1, &region);
        } else {
          dfn.vkCmdCopyImage(command_buffer, args.src_image, args.src_image_layout, args.dst_image,
                             args.dst_image_layout, 1, &args.region);
        }
      } break;

      case Command::kVkCopyQueryPoolResults: {
        auto& args = *reinterpret_cast<const ArgsVkCopyQueryPoolResults*>(stream);
        dfn.vkCmdCopyQueryPoolResults(command_buffer, args.query_pool, args.first_query,
                                      args.query_count, args.dst_buffer, args.dst_offset,
                                      args.stride, args.flags);
      } break;

      case Command::kVkDispatch: {
        auto& args = *reinterpret_cast<const ArgsVkDispatch*>(stream);
        dfn.vkCmdDispatch(command_buffer, args.group_count_x, args.group_count_y,
                          args.group_count_z);
      } break;

      case Command::kVkDraw: {
        auto& args = *reinterpret_cast<const ArgsVkDraw*>(stream);
        dfn.vkCmdDraw(command_buffer, args.vertex_count, args.instance_count, args.first_vertex,
                      args.first_instance);
      } break;

      case Command::kVkDrawIndexed: {
        auto& args = *reinterpret_cast<const ArgsVkDrawIndexed*>(stream);
        dfn.vkCmdDrawIndexed(command_buffer, args.index_count, args.instance_count,
                             args.first_index, args.vertex_offset, args.first_instance);
      } break;

      case Command::kVkEndQuery: {
        auto& args = *reinterpret_cast<const ArgsVkEndQuery*>(stream);
        dfn.vkCmdEndQuery(command_buffer, args.query_pool, args.query);
      } break;

      case Command::kVkEndRenderPass:
        dfn.vkCmdEndRenderPass(command_buffer);
        break;

      case Command::kVkBeginRendering: {
        auto& args = *reinterpret_cast<const ArgsVkBeginRendering*>(stream);
        size_t offset_bytes =
            rex::align(sizeof(ArgsVkBeginRendering), alignof(VkRenderingAttachmentInfo));
        const VkRenderingAttachmentInfo* color_attachments =
            args.color_attachment_count
                ? reinterpret_cast<const VkRenderingAttachmentInfo*>(
                      reinterpret_cast<const uint8_t*>(stream) + offset_bytes)
                : nullptr;
        offset_bytes += sizeof(VkRenderingAttachmentInfo) * args.color_attachment_count;
        const VkRenderingAttachmentInfo* depth_attachment =
            args.has_depth_attachment ? reinterpret_cast<const VkRenderingAttachmentInfo*>(
                                            reinterpret_cast<const uint8_t*>(stream) + offset_bytes)
                                      : nullptr;
        if (args.has_depth_attachment) {
          offset_bytes += sizeof(VkRenderingAttachmentInfo);
        }
        const VkRenderingAttachmentInfo* stencil_attachment =
            args.has_stencil_attachment
                ? reinterpret_cast<const VkRenderingAttachmentInfo*>(
                      reinterpret_cast<const uint8_t*>(stream) + offset_bytes)
                : nullptr;
        VkRenderingInfo rendering_info = {};
        rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering_info.flags = args.flags;
        rendering_info.renderArea = args.render_area;
        rendering_info.layerCount = args.layer_count;
        rendering_info.viewMask = args.view_mask;
        rendering_info.colorAttachmentCount = args.color_attachment_count;
        rendering_info.pColorAttachments = color_attachments;
        rendering_info.pDepthAttachment = depth_attachment;
        rendering_info.pStencilAttachment = stencil_attachment;
        dfn.vkCmdBeginRendering(command_buffer, &rendering_info);
      } break;

      case Command::kVkEndRendering:
        dfn.vkCmdEndRendering(command_buffer);
        break;

      case Command::kVkPipelineBarrier: {
        auto& args = *reinterpret_cast<const ArgsVkPipelineBarrier*>(stream);
        size_t barrier_offset_bytes = sizeof(ArgsVkPipelineBarrier);
        const VkMemoryBarrier* memory_barriers = nullptr;
        if (args.memory_barrier_count) {
          barrier_offset_bytes = rex::align(barrier_offset_bytes, alignof(VkMemoryBarrier));
          memory_barriers = reinterpret_cast<const VkMemoryBarrier*>(
              reinterpret_cast<const uint8_t*>(stream) + barrier_offset_bytes);
          barrier_offset_bytes += sizeof(VkMemoryBarrier) * args.memory_barrier_count;
        }
        const VkBufferMemoryBarrier* buffer_memory_barriers = nullptr;
        if (args.buffer_memory_barrier_count) {
          barrier_offset_bytes = rex::align(barrier_offset_bytes, alignof(VkBufferMemoryBarrier));
          buffer_memory_barriers = reinterpret_cast<const VkBufferMemoryBarrier*>(
              reinterpret_cast<const uint8_t*>(stream) + barrier_offset_bytes);
          barrier_offset_bytes += sizeof(VkBufferMemoryBarrier) * args.buffer_memory_barrier_count;
        }
        const VkImageMemoryBarrier* image_memory_barriers = nullptr;
        if (args.image_memory_barrier_count) {
          barrier_offset_bytes = rex::align(barrier_offset_bytes, alignof(VkImageMemoryBarrier));
          image_memory_barriers = reinterpret_cast<const VkImageMemoryBarrier*>(
              reinterpret_cast<const uint8_t*>(stream) + barrier_offset_bytes);
          barrier_offset_bytes += sizeof(VkImageMemoryBarrier) * args.image_memory_barrier_count;
        }
        dfn.vkCmdPipelineBarrier(command_buffer, args.src_stage_mask, args.dst_stage_mask,
                                 args.dependency_flags, args.memory_barrier_count, memory_barriers,
                                 args.buffer_memory_barrier_count, buffer_memory_barriers,
                                 args.image_memory_barrier_count, image_memory_barriers);
      } break;

      case Command::kVkPushConstants: {
        auto& args = *reinterpret_cast<const ArgsVkPushConstants*>(stream);
        dfn.vkCmdPushConstants(
            command_buffer, args.layout, args.stage_flags, args.offset, args.size,
            reinterpret_cast<const uint8_t*>(stream) + sizeof(ArgsVkPushConstants));
      } break;

      case Command::kVkResetQueryPool: {
        auto& args = *reinterpret_cast<const ArgsVkResetQueryPool*>(stream);
        dfn.vkCmdResetQueryPool(command_buffer, args.query_pool, args.first_query,
                                args.query_count);
      } break;

      case Command::kVkSetBlendConstants: {
        auto& args = *reinterpret_cast<const ArgsVkSetBlendConstants*>(stream);
        dfn.vkCmdSetBlendConstants(command_buffer, args.blend_constants);
      } break;

      case Command::kVkSetDepthBias: {
        auto& args = *reinterpret_cast<const ArgsVkSetDepthBias*>(stream);
        dfn.vkCmdSetDepthBias(command_buffer, args.depth_bias_constant_factor,
                              args.depth_bias_clamp, args.depth_bias_slope_factor);
      } break;

      case Command::kVkSetScissor: {
        auto& args = *reinterpret_cast<const ArgsVkSetScissor*>(stream);
        dfn.vkCmdSetScissor(command_buffer, args.first_scissor, args.scissor_count,
                            reinterpret_cast<const VkRect2D*>(
                                reinterpret_cast<const uint8_t*>(stream) +
                                rex::align(sizeof(ArgsVkSetScissor), alignof(VkRect2D))));
      } break;

      case Command::kVkSetStencilCompareMask: {
        auto& args = *reinterpret_cast<const ArgsSetStencilMaskReference*>(stream);
        dfn.vkCmdSetStencilCompareMask(command_buffer, args.face_mask, args.mask_reference);
      } break;

      case Command::kVkSetStencilReference: {
        auto& args = *reinterpret_cast<const ArgsSetStencilMaskReference*>(stream);
        dfn.vkCmdSetStencilReference(command_buffer, args.face_mask, args.mask_reference);
      } break;

      case Command::kVkSetStencilWriteMask: {
        auto& args = *reinterpret_cast<const ArgsSetStencilMaskReference*>(stream);
        dfn.vkCmdSetStencilWriteMask(command_buffer, args.face_mask, args.mask_reference);
      } break;

      case Command::kVkSetViewport: {
        auto& args = *reinterpret_cast<const ArgsVkSetViewport*>(stream);
        dfn.vkCmdSetViewport(command_buffer, args.first_viewport, args.viewport_count,
                             reinterpret_cast<const VkViewport*>(
                                 reinterpret_cast<const uint8_t*>(stream) +
                                 rex::align(sizeof(ArgsVkSetViewport), alignof(VkViewport))));
      } break;

      default:
        assert_unhandled_case(header.command);
        break;
    }

    stream += header.arguments_size_elements;
    stream_remaining -= header.arguments_size_elements;
  }
}

void DeferredCommandBuffer::CmdVkPipelineBarrier(
    VkPipelineStageFlags src_stage_mask, VkPipelineStageFlags dst_stage_mask,
    VkDependencyFlags dependency_flags, uint32_t memory_barrier_count,
    const VkMemoryBarrier* memory_barriers, uint32_t buffer_memory_barrier_count,
    const VkBufferMemoryBarrier* buffer_memory_barriers, uint32_t image_memory_barrier_count,
    const VkImageMemoryBarrier* image_memory_barriers) {
  size_t arguments_size = sizeof(ArgsVkPipelineBarrier);
  size_t memory_barriers_offset = 0;
  if (memory_barrier_count) {
    arguments_size = rex::align(arguments_size, alignof(VkMemoryBarrier));
    memory_barriers_offset = arguments_size;
    arguments_size += sizeof(VkMemoryBarrier) * memory_barrier_count;
  }
  size_t buffer_memory_barriers_offset = 0;
  if (buffer_memory_barrier_count) {
    arguments_size = rex::align(arguments_size, alignof(VkBufferMemoryBarrier));
    buffer_memory_barriers_offset = arguments_size;
    arguments_size += sizeof(VkBufferMemoryBarrier) * buffer_memory_barrier_count;
  }
  size_t image_memory_barriers_offset = 0;
  if (image_memory_barrier_count) {
    arguments_size = rex::align(arguments_size, alignof(VkImageMemoryBarrier));
    image_memory_barriers_offset = arguments_size;
    arguments_size += sizeof(VkImageMemoryBarrier) * image_memory_barrier_count;
  }
  uint8_t* args_ptr =
      reinterpret_cast<uint8_t*>(WriteCommand(Command::kVkPipelineBarrier, arguments_size));
  auto& args = *reinterpret_cast<ArgsVkPipelineBarrier*>(args_ptr);
  args.src_stage_mask = src_stage_mask;
  args.dst_stage_mask = dst_stage_mask;
  args.dependency_flags = dependency_flags;
  args.memory_barrier_count = memory_barrier_count;
  args.buffer_memory_barrier_count = buffer_memory_barrier_count;
  args.image_memory_barrier_count = image_memory_barrier_count;
  if (memory_barrier_count) {
    std::memcpy(args_ptr + memory_barriers_offset, memory_barriers,
                sizeof(VkMemoryBarrier) * memory_barrier_count);
  }
  if (buffer_memory_barrier_count) {
    std::memcpy(args_ptr + buffer_memory_barriers_offset, buffer_memory_barriers,
                sizeof(VkBufferMemoryBarrier) * buffer_memory_barrier_count);
  }
  if (image_memory_barrier_count) {
    std::memcpy(args_ptr + image_memory_barriers_offset, image_memory_barriers,
                sizeof(VkImageMemoryBarrier) * image_memory_barrier_count);
  }
}

void DeferredCommandBuffer::CmdVkBeginRendering(const VkRenderingInfo* rendering_info) {
  assert_null(rendering_info->pNext);

  size_t arguments_size =
      rex::align(sizeof(ArgsVkBeginRendering), alignof(VkRenderingAttachmentInfo));
  size_t color_attachments_offset = arguments_size;
  arguments_size += sizeof(VkRenderingAttachmentInfo) * rendering_info->colorAttachmentCount;
  size_t depth_attachment_offset = arguments_size;
  if (rendering_info->pDepthAttachment) {
    arguments_size += sizeof(VkRenderingAttachmentInfo);
  }
  size_t stencil_attachment_offset = arguments_size;
  if (rendering_info->pStencilAttachment) {
    arguments_size += sizeof(VkRenderingAttachmentInfo);
  }

  uint8_t* args_ptr =
      reinterpret_cast<uint8_t*>(WriteCommand(Command::kVkBeginRendering, arguments_size));
  auto& args = *reinterpret_cast<ArgsVkBeginRendering*>(args_ptr);
  args.flags = rendering_info->flags;
  args.render_area = rendering_info->renderArea;
  args.layer_count = rendering_info->layerCount;
  args.view_mask = rendering_info->viewMask;
  args.color_attachment_count = rendering_info->colorAttachmentCount;
  args.has_depth_attachment = rendering_info->pDepthAttachment != nullptr;
  args.has_stencil_attachment = rendering_info->pStencilAttachment != nullptr;

  if (rendering_info->colorAttachmentCount) {
    std::memcpy(args_ptr + color_attachments_offset, rendering_info->pColorAttachments,
                sizeof(VkRenderingAttachmentInfo) * rendering_info->colorAttachmentCount);
  }
  if (rendering_info->pDepthAttachment) {
    std::memcpy(args_ptr + depth_attachment_offset, rendering_info->pDepthAttachment,
                sizeof(VkRenderingAttachmentInfo));
  }
  if (rendering_info->pStencilAttachment) {
    std::memcpy(args_ptr + stencil_attachment_offset, rendering_info->pStencilAttachment,
                sizeof(VkRenderingAttachmentInfo));
  }
}

void* DeferredCommandBuffer::WriteCommand(Command command, size_t arguments_size_bytes) {
  if (timestamp_category_ != timestamp_category_marked_) {
    timestamp_category_marked_ = timestamp_category_;
    if (timestamp_category_ != kNoTimestampCategory) {
      auto& marker = *reinterpret_cast<ArgsTimestampMarker*>(
          WriteCommandRaw(Command::kTimestampMarker, sizeof(ArgsTimestampMarker)));
      marker.category = timestamp_category_;
    }
  }
  return WriteCommandRaw(command, arguments_size_bytes);
}

void* DeferredCommandBuffer::WriteCommandRaw(Command command, size_t arguments_size_bytes) {
  size_t arguments_size_elements =
      (arguments_size_bytes + sizeof(uintmax_t) - 1) / sizeof(uintmax_t);
  const size_t command_size_elements = kCommandHeaderSizeElements + arguments_size_elements;
  if (write_offset_ + command_size_elements > block_capacity_[write_block_]) {
    // Continue in the next block (commands don't span blocks). The end of this
    // one becomes visible to readers when the next block is published.
    assert_true(write_block_ + 1 < kMaxBlocks);
    block_end_[write_block_] = write_offset_;
    ++write_block_;
    write_offset_ = 0;
    if (block_capacity_[write_block_] < command_size_elements) {
      size_t capacity = std::max(kBlockElements, command_size_elements);
      blocks_[write_block_] = std::make_unique<uintmax_t[]>(capacity);
      block_capacity_[write_block_] = capacity;
    }
  }
  uintmax_t* command_ptr = blocks_[write_block_].get() + write_offset_;
  write_offset_ += command_size_elements;
  CommandHeader& header = *reinterpret_cast<CommandHeader*>(command_ptr);
  header.command = command;
  header.arguments_size_elements = uint32_t(arguments_size_elements);
  return command_ptr + kCommandHeaderSizeElements;
}

}  // namespace rex::graphics::vulkan
