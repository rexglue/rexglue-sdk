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
#include <cstring>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/graphics/vulkan/command_processor.h>
#include <rex/graphics/vulkan/deferred_command_buffer.h>
#include <rex/graphics/vulkan/shared_memory.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ui/vulkan/util.h>

REXCVAR_DEFINE_BOOL(vulkan_sparse_shared_memory, true, "GPU/Vulkan",
                    "Use sparse shared memory on Vulkan")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(vulkan_import_guest_memory, false, "GPU/Vulkan",
                    "Import guest physical memory as the Vulkan shared-memory buffer via "
                    "VK_EXT_external_memory_host (UMA fast path). Off by default — known to "
                    "produce black frames on Tegra X1 because the resolve-vs-host-cache "
                    "coherency story isn't finished. Enable for experimentation.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::graphics::vulkan {

VulkanSharedMemory::VulkanSharedMemory(VulkanCommandProcessor& command_processor,
                                       memory::Memory& memory, TraceWriter& trace_writer,
                                       VkPipelineStageFlags guest_shader_pipeline_stages)
    : SharedMemory(memory),
      command_processor_(command_processor),
      trace_writer_(trace_writer),
      guest_shader_pipeline_stages_(guest_shader_pipeline_stages) {}

VulkanSharedMemory::~VulkanSharedMemory() {
  Shutdown(true);
}

bool VulkanSharedMemory::Initialize() {
  InitializeCommon();

  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  const VkBufferCreateFlags sparse_flags =
      VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT;

  // Try to create a sparse buffer.
  VkBufferCreateInfo buffer_create_info;
  buffer_create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_create_info.pNext = nullptr;
  buffer_create_info.flags = sparse_flags;
  buffer_create_info.size = kBufferSize;
  buffer_create_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
  buffer_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  buffer_create_info.queueFamilyIndexCount = 0;
  buffer_create_info.pQueueFamilyIndices = nullptr;
  if (REXCVAR_GET(vulkan_sparse_shared_memory) &&
      vulkan_device->properties().sparseResidencyBuffer) {
    if (dfn.vkCreateBuffer(device, &buffer_create_info, nullptr, &buffer_) == VK_SUCCESS) {
      VkMemoryRequirements buffer_memory_requirements;
      dfn.vkGetBufferMemoryRequirements(device, buffer_, &buffer_memory_requirements);
      if (rex::bit_scan_forward(buffer_memory_requirements.memoryTypeBits &
                                    vulkan_device->memory_types().device_local,
                                &buffer_memory_type_)) {
        uint32_t allocation_size_log2;
        rex::bit_scan_forward(std::max(uint64_t(buffer_memory_requirements.alignment), uint64_t(1)),
                              &allocation_size_log2);
        if (allocation_size_log2 < kBufferSizeLog2) {
          // Maximum of 1024 allocations in the worst case for all of the
          // buffer because of the overall 4096 allocation count limit on
          // Windows drivers.
          InitializeSparseHostGpuMemory(std::max(
              allocation_size_log2,
              std::max(kHostGpuMemoryOptimalSparseAllocationLog2, kBufferSizeLog2 - uint32_t(10))));
        } else {
          // Shouldn't happen on any real platform, but no point allocating the
          // buffer sparsely.
          dfn.vkDestroyBuffer(device, buffer_, nullptr);
          buffer_ = VK_NULL_HANDLE;
        }
      } else {
        REXGPU_ERROR(
            "Shared memory: Failed to get a device-local Vulkan memory type "
            "for the sparse buffer");
        dfn.vkDestroyBuffer(device, buffer_, nullptr);
        buffer_ = VK_NULL_HANDLE;
      }
    } else {
      REXGPU_ERROR("Shared memory: Failed to create the {} MB Vulkan sparse buffer",
                   kBufferSize >> 20);
    }
  }

  // Create a non-sparse buffer if there were issues with the sparse buffer.
  if (buffer_ == VK_NULL_HANDLE) {
    // VK_EXT_external_memory_host fast path: import the existing guest
    // physical-memory shm allocation (memory().physical_membase()) directly
    // as the buffer's backing. CPU writes to guest memory become GPU-visible
    // immediately — no upload step, no SIGSEGV-based write-watch needed.
    // Critical on Tegra X1 / L4T 4.9 where mprotect-based watchpoints don't
    // deliver signals reliably; harmless elsewhere as long as the host
    // pointer alignment requirement is met. Gated by an opt-in cvar because
    // on some drivers the imported memory still needs explicit flush/cache
    // management we haven't wired up yet, producing all-black frames.
    const bool try_external_host =
        REXCVAR_GET(vulkan_import_guest_memory) &&
        vulkan_device->extensions().ext_EXT_external_memory_host &&
        vulkan_device->properties().minImportedHostPointerAlignment != 0 &&
        vulkan_device->functions().vkGetMemoryHostPointerPropertiesEXT != nullptr;
    bool used_external_host = false;
    if (try_external_host) {
      const VkDeviceSize align = vulkan_device->properties().minImportedHostPointerAlignment;
      uint8_t* const host_ptr = memory().physical_membase();
      const bool ptr_aligned = (reinterpret_cast<uintptr_t>(host_ptr) % align) == 0;
      const bool size_aligned = (kBufferSize % align) == 0;
      if (host_ptr && ptr_aligned && size_aligned) {
        VkExternalMemoryBufferCreateInfo external_buffer_info{};
        external_buffer_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
        external_buffer_info.handleTypes =
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        VkBufferCreateInfo external_buffer_create_info = buffer_create_info;
        external_buffer_create_info.flags &= ~sparse_flags;
        external_buffer_create_info.pNext = &external_buffer_info;
        if (dfn.vkCreateBuffer(device, &external_buffer_create_info, nullptr, &buffer_) ==
            VK_SUCCESS) {
          VkMemoryRequirements req;
          dfn.vkGetBufferMemoryRequirements(device, buffer_, &req);
          VkMemoryHostPointerPropertiesEXT host_ptr_props{};
          host_ptr_props.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT;
          if (vulkan_device->functions().vkGetMemoryHostPointerPropertiesEXT(
                  device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host_ptr,
                  &host_ptr_props) == VK_SUCCESS) {
            const uint32_t compatible =
                req.memoryTypeBits & host_ptr_props.memoryTypeBits;
            uint32_t mem_type;
            // Prefer device-local + host-coherent (UMA fast path); fall back to any
            // host-visible type.
            if (rex::bit_scan_forward(compatible & vulkan_device->memory_types().device_local &
                                          vulkan_device->memory_types().host_coherent,
                                      &mem_type) ||
                rex::bit_scan_forward(compatible & vulkan_device->memory_types().host_visible,
                                      &mem_type)) {
              VkImportMemoryHostPointerInfoEXT import_info{};
              import_info.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT;
              import_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
              import_info.pHostPointer = host_ptr;
              VkMemoryAllocateInfo alloc_info{};
              alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
              alloc_info.pNext = &import_info;
              alloc_info.allocationSize = kBufferSize;
              alloc_info.memoryTypeIndex = mem_type;
              VkDeviceMemory imported_memory;
              if (dfn.vkAllocateMemory(device, &alloc_info, nullptr, &imported_memory) ==
                      VK_SUCCESS &&
                  dfn.vkBindBufferMemory(device, buffer_, imported_memory, 0) == VK_SUCCESS) {
                buffer_memory_type_ = mem_type;
                buffer_memory_.push_back(imported_memory);
                used_external_host = true;
                external_host_imported_ = true;
                REXGPU_INFO(
                    "Shared memory: imported guest physical heap via "
                    "VK_EXT_external_memory_host (memory type {}, {} MB) — CPU "
                    "writes are GPU-coherent, GPU write-watch disabled",
                    mem_type, kBufferSize >> 20);
              } else {
                if (imported_memory != VK_NULL_HANDLE) {
                  dfn.vkFreeMemory(device, imported_memory, nullptr);
                }
                REXGPU_WARN(
                    "Shared memory: VK_EXT_external_memory_host import failed at "
                    "Allocate/Bind; falling back to device-local copy");
                dfn.vkDestroyBuffer(device, buffer_, nullptr);
                buffer_ = VK_NULL_HANDLE;
              }
            } else {
              REXGPU_WARN(
                  "Shared memory: no compatible host-visible memory type for "
                  "external_memory_host import; falling back");
              dfn.vkDestroyBuffer(device, buffer_, nullptr);
              buffer_ = VK_NULL_HANDLE;
            }
          } else {
            dfn.vkDestroyBuffer(device, buffer_, nullptr);
            buffer_ = VK_NULL_HANDLE;
          }
        }
      } else {
        REXGPU_WARN(
            "Shared memory: VK_EXT_external_memory_host present but guest "
            "physical_membase ({:p}) or kBufferSize ({}) isn't aligned to "
            "minImportedHostPointerAlignment ({}); falling back",
            static_cast<void*>(host_ptr), kBufferSize, align);
      }
    }

    if (buffer_ == VK_NULL_HANDLE) {
      REXGPU_INFO(
          "Vulkan sparse binding is not used for shared memory emulation - video "
          "memory usage may increase significantly because a full {} MB buffer "
          "will be created",
          kBufferSize >> 20);
      buffer_create_info.flags &= ~sparse_flags;
      if (dfn.vkCreateBuffer(device, &buffer_create_info, nullptr, &buffer_) != VK_SUCCESS) {
        REXGPU_ERROR("Shared memory: Failed to create the {} MB Vulkan buffer", kBufferSize >> 20);
        Shutdown();
        return false;
      }
      VkMemoryRequirements buffer_memory_requirements;
      dfn.vkGetBufferMemoryRequirements(device, buffer_, &buffer_memory_requirements);
      if (!rex::bit_scan_forward(
              buffer_memory_requirements.memoryTypeBits & vulkan_device->memory_types().device_local,
              &buffer_memory_type_)) {
        REXGPU_ERROR(
            "Shared memory: Failed to get a device-local Vulkan memory type for "
            "the buffer");
        Shutdown();
        return false;
      }
      VkMemoryAllocateInfo buffer_memory_allocate_info;
      VkMemoryAllocateInfo* buffer_memory_allocate_info_last = &buffer_memory_allocate_info;
      buffer_memory_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      buffer_memory_allocate_info.pNext = nullptr;
      buffer_memory_allocate_info.allocationSize = buffer_memory_requirements.size;
      buffer_memory_allocate_info.memoryTypeIndex = buffer_memory_type_;
      VkMemoryDedicatedAllocateInfo buffer_memory_dedicated_allocate_info;
      if (vulkan_device->extensions().ext_1_1_KHR_dedicated_allocation) {
        buffer_memory_allocate_info_last->pNext = &buffer_memory_dedicated_allocate_info;
        buffer_memory_allocate_info_last =
            reinterpret_cast<VkMemoryAllocateInfo*>(&buffer_memory_dedicated_allocate_info);
        buffer_memory_dedicated_allocate_info.sType =
            VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        buffer_memory_dedicated_allocate_info.pNext = nullptr;
        buffer_memory_dedicated_allocate_info.image = VK_NULL_HANDLE;
        buffer_memory_dedicated_allocate_info.buffer = buffer_;
      }
      VkDeviceMemory buffer_memory;
      if (dfn.vkAllocateMemory(device, &buffer_memory_allocate_info, nullptr, &buffer_memory) !=
          VK_SUCCESS) {
        REXGPU_ERROR(
            "Shared memory: Failed to allocate {} MB of memory for the Vulkan "
            "buffer",
            kBufferSize >> 20);
        Shutdown();
        return false;
      }
      buffer_memory_.push_back(buffer_memory);
      if (dfn.vkBindBufferMemory(device, buffer_, buffer_memory, 0) != VK_SUCCESS) {
        REXGPU_ERROR("Shared memory: Failed to bind memory to the Vulkan buffer");
        Shutdown();
        return false;
      }
    }
    (void)used_external_host;
  }

  // The first usage will likely be uploading.
  last_usage_ = Usage::kTransferDestination;
  last_written_range_ = std::make_pair<uint32_t, uint32_t>(0, 0);

  upload_buffer_pool_ = std::make_unique<ui::vulkan::VulkanUploadBufferPool>(
      vulkan_device, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      rex::align(ui::vulkan::VulkanUploadBufferPool::kDefaultPageSize, size_t(1)
                                                                           << page_size_log2()));

  return true;
}

void VulkanSharedMemory::Shutdown(bool from_destructor) {
  ResetTraceDownload();

  upload_buffer_pool_.reset();

  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, buffer_);
  for (VkDeviceMemory memory : buffer_memory_) {
    dfn.vkFreeMemory(device, memory, nullptr);
  }
  buffer_memory_.clear();

  // If calling from the destructor, the SharedMemory destructor will call
  // ShutdownCommon.
  if (!from_destructor) {
    ShutdownCommon();
  }
}

void VulkanSharedMemory::CompletedSubmissionUpdated() {
  upload_buffer_pool_->Reclaim(command_processor_.GetCompletedSubmission());
}

void VulkanSharedMemory::EndSubmission() {
  upload_buffer_pool_->FlushWrites();
}

void VulkanSharedMemory::Use(Usage usage, std::pair<uint32_t, uint32_t> written_range) {
  written_range.first = std::min(written_range.first, kBufferSize);
  written_range.second = std::min(written_range.second, kBufferSize - written_range.first);
  assert_true(usage != Usage::kRead || !written_range.second);
  if (last_usage_ != usage || last_written_range_.second) {
    VkPipelineStageFlags src_stage_mask, dst_stage_mask;
    VkAccessFlags src_access_mask, dst_access_mask;
    GetUsageMasks(last_usage_, src_stage_mask, src_access_mask);
    GetUsageMasks(usage, dst_stage_mask, dst_access_mask);
    VkDeviceSize offset, size;
    if (last_usage_ == usage) {
      // Committing the previous write, while not changing the access mask
      // (passing false as whether to skip the barrier if no masks are changed
      // for this reason).
      offset = VkDeviceSize(last_written_range_.first);
      size = VkDeviceSize(last_written_range_.second);
    } else {
      // Changing the stage and access mask - all preceding writes must be
      // available not only to the source stage, but to the destination as well.
      offset = 0;
      size = VK_WHOLE_SIZE;
      last_usage_ = usage;
    }
    command_processor_.PushBufferMemoryBarrier(
        buffer_, offset, size, src_stage_mask, dst_stage_mask, src_access_mask, dst_access_mask,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
  }
  last_written_range_ = written_range;
}

bool VulkanSharedMemory::InitializeTraceSubmitDownloads() {
  ResetTraceDownload();
  PrepareForTraceDownload();
  uint32_t download_page_count = trace_download_page_count();
  if (!download_page_count) {
    return false;
  }

  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          command_processor_.GetVulkanDevice(), download_page_count << page_size_log2(),
          VK_BUFFER_USAGE_TRANSFER_DST_BIT, ui::vulkan::util::MemoryPurpose::kReadback,
          trace_download_buffer_, trace_download_buffer_memory_)) {
    REXGPU_ERROR(
        "Shared memory: Failed to create a {} KB GPU-written memory download "
        "buffer for frame tracing",
        download_page_count << page_size_log2() >> 10);
    ResetTraceDownload();
    return false;
  }

  Use(Usage::kRead);
  command_processor_.SubmitBarriers(true);
  DeferredCommandBuffer& command_buffer = command_processor_.deferred_command_buffer();

  size_t download_range_count = trace_download_ranges().size();
  VkBufferCopy* download_regions = command_buffer.CmdCopyBufferEmplace(
      buffer_, trace_download_buffer_, uint32_t(download_range_count));
  VkDeviceSize download_buffer_offset = 0;
  for (size_t i = 0; i < download_range_count; ++i) {
    VkBufferCopy& download_region = download_regions[i];
    const std::pair<uint32_t, uint32_t>& download_range = trace_download_ranges()[i];
    download_region.srcOffset = download_range.first;
    download_region.dstOffset = download_buffer_offset;
    download_region.size = download_range.second;
    download_buffer_offset += download_range.second;
  }

  command_processor_.PushBufferMemoryBarrier(
      trace_download_buffer_, 0, VK_WHOLE_SIZE, VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);

  return true;
}

void VulkanSharedMemory::InitializeTraceCompleteDownloads() {
  if (!trace_download_buffer_memory_) {
    return;
  }
  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  void* download_mapping;
  if (dfn.vkMapMemory(device, trace_download_buffer_memory_, 0, VK_WHOLE_SIZE, 0,
                      &download_mapping) == VK_SUCCESS) {
    uint32_t download_buffer_offset = 0;
    for (const auto& download_range : trace_download_ranges()) {
      trace_writer_.WriteMemoryRead(
          download_range.first, download_range.second,
          reinterpret_cast<const uint8_t*>(download_mapping) + download_buffer_offset);
    }
    dfn.vkUnmapMemory(device, trace_download_buffer_memory_);
  } else {
    REXGPU_ERROR(
        "Shared memory: Failed to map the GPU-written memory download buffer "
        "for frame tracing");
  }
  ResetTraceDownload();
}

bool VulkanSharedMemory::AllocateSparseHostGpuMemoryRange(uint32_t offset_allocations,
                                                          uint32_t length_allocations) {
  if (!length_allocations) {
    return true;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  VkMemoryAllocateInfo memory_allocate_info;
  memory_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  memory_allocate_info.pNext = nullptr;
  memory_allocate_info.allocationSize = length_allocations
                                        << host_gpu_memory_sparse_granularity_log2();
  memory_allocate_info.memoryTypeIndex = buffer_memory_type_;
  VkDeviceMemory memory;
  if (dfn.vkAllocateMemory(device, &memory_allocate_info, nullptr, &memory) != VK_SUCCESS) {
    REXGPU_ERROR("Shared memory: Failed to allocate sparse buffer memory");
    return false;
  }
  buffer_memory_.push_back(memory);

  VkSparseMemoryBind bind;
  bind.resourceOffset = offset_allocations << host_gpu_memory_sparse_granularity_log2();
  bind.size = memory_allocate_info.allocationSize;
  bind.memory = memory;
  bind.memoryOffset = 0;
  bind.flags = 0;
  VkPipelineStageFlags bind_wait_stage_mask =
      VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
      VK_PIPELINE_STAGE_TRANSFER_BIT;
  if (vulkan_device->properties().tessellationShader) {
    bind_wait_stage_mask |= VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT;
  }
  command_processor_.SparseBindBuffer(buffer_, 1, &bind, bind_wait_stage_mask);

  return true;
}

bool VulkanSharedMemory::UploadRanges(
    const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) {
  if (upload_page_ranges.empty()) {
    return true;
  }
  if (external_host_imported_) {
    // The buffer is already a view onto guest physical memory; CPU writes
    // are GPU-visible. Just mark the ranges valid so the cache stops
    // requesting re-uploads — no actual copy work needed.
    for (auto upload_range : upload_page_ranges) {
      MakeRangeValid(upload_range.first << page_size_log2(),
                     upload_range.second << page_size_log2(), false);
    }
    return true;
  }
  // upload_page_ranges are sorted, use them to determine the range for the
  // ordering barrier.
  Use(Usage::kTransferDestination,
      std::make_pair(upload_page_ranges.front().first << page_size_log2(),
                     (upload_page_ranges.back().first + upload_page_ranges.back().second -
                      upload_page_ranges.front().first)
                         << page_size_log2()));
  command_processor_.SubmitBarriers(true);
  DeferredCommandBuffer& command_buffer = command_processor_.deferred_command_buffer();
  uint64_t submission_current = command_processor_.GetCurrentSubmission();
  bool successful = true;
  upload_regions_.clear();
  VkBuffer upload_buffer_previous = VK_NULL_HANDLE;
  for (auto upload_range : upload_page_ranges) {
    uint32_t upload_range_start = upload_range.first;
    uint32_t upload_range_length = upload_range.second;
    trace_writer_.WriteMemoryRead(upload_range_start << page_size_log2(),
                                  upload_range_length << page_size_log2());
    while (upload_range_length) {
      VkBuffer upload_buffer;
      VkDeviceSize upload_buffer_offset, upload_buffer_size;
      uint8_t* upload_buffer_mapping = upload_buffer_pool_->RequestPartial(
          submission_current, upload_range_length << page_size_log2(),
          size_t(1) << page_size_log2(), upload_buffer, upload_buffer_offset, upload_buffer_size);
      if (upload_buffer_mapping == nullptr) {
        REXGPU_ERROR("Shared memory: Failed to get a Vulkan upload buffer");
        successful = false;
        break;
      }
      MakeRangeValid(upload_range_start << page_size_log2(), uint32_t(upload_buffer_size), false);
      std::memcpy(upload_buffer_mapping,
                  memory().TranslatePhysical(upload_range_start << page_size_log2()),
                  upload_buffer_size);
      if (upload_buffer_previous != upload_buffer && !upload_regions_.empty()) {
        assert_true(upload_buffer_previous != VK_NULL_HANDLE);
        command_buffer.CmdVkCopyBuffer(upload_buffer_previous, buffer_,
                                       uint32_t(upload_regions_.size()), upload_regions_.data());
        upload_regions_.clear();
      }
      upload_buffer_previous = upload_buffer;
      VkBufferCopy& upload_region = upload_regions_.emplace_back();
      upload_region.srcOffset = upload_buffer_offset;
      upload_region.dstOffset = VkDeviceSize(upload_range_start << page_size_log2());
      upload_region.size = upload_buffer_size;
      uint32_t upload_buffer_pages = uint32_t(upload_buffer_size >> page_size_log2());
      upload_range_start += upload_buffer_pages;
      upload_range_length -= upload_buffer_pages;
    }
    if (!successful) {
      break;
    }
  }
  if (!upload_regions_.empty()) {
    assert_true(upload_buffer_previous != VK_NULL_HANDLE);
    command_buffer.CmdVkCopyBuffer(upload_buffer_previous, buffer_,
                                   uint32_t(upload_regions_.size()), upload_regions_.data());
    upload_regions_.clear();
  }
  return successful;
}

void VulkanSharedMemory::GetUsageMasks(Usage usage, VkPipelineStageFlags& stage_mask,
                                       VkAccessFlags& access_mask) const {
  switch (usage) {
    case Usage::kComputeWrite:
      stage_mask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      access_mask = VK_ACCESS_SHADER_READ_BIT;
      return;
    case Usage::kTransferDestination:
      stage_mask = VK_PIPELINE_STAGE_TRANSFER_BIT;
      access_mask = VK_ACCESS_TRANSFER_WRITE_BIT;
      return;
    default:
      break;
  }
  stage_mask = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | guest_shader_pipeline_stages_;
  access_mask = VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
  switch (usage) {
    case Usage::kRead:
      stage_mask |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
      access_mask |= VK_ACCESS_TRANSFER_READ_BIT;
      break;
    case Usage::kGuestDrawReadWrite:
      access_mask |= VK_ACCESS_SHADER_WRITE_BIT;
      break;
    default:
      assert_unhandled_case(usage);
  }
}

void VulkanSharedMemory::ResetTraceDownload() {
  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, trace_download_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, trace_download_buffer_memory_);
  ReleaseTraceDownloadRanges();
}

}  // namespace rex::graphics::vulkan
