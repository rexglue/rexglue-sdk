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

#include <bit>
#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstring>
#include <string_view>

#include <format>

#include <rex/bit.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/logging/flush.h>
#include <rex/perf/counter.h>
#include <rex/chrono/clock.h>
#include "gpu/xenos/command_processor.h"
#include "gpu/xenos/graphics_system.h"
#include <rex/runtime/object_table.h>
#include <rex/xenos/texture_info.h>
#include "gpu/xenos/sampler_info.h"
#include <rex/xenos/xenos.h>
#include <rex/assert.h>
#include <rex/fail.h>
#include <rex/logging/log.h>
#include <rex/math.h>
#include <rex/memory.h>
#include <rex/memory/ring_buffer.h>
#include <rex/runtime.h>
#include <rex/runtime/thread_registry.h>
#include <rex/runtime/module_object.h>
#include <rex/move_only_function.h>

#include "gpu/xenos/cvar_table_gpu.h"
#include "gpu/xenos/cvar_table_xenos.h"

namespace rex::gpu {

using namespace rex::xenos;

namespace {

ReadbackResolveMode ParseReadbackResolveMode(std::string_view value) {
  if (value == "fast") {
    return ReadbackResolveMode::kFast;
  }
  if (value == "some") {
    return ReadbackResolveMode::kSome;
  }
  if (value == "full") {
    return ReadbackResolveMode::kFull;
  }
  return ReadbackResolveMode::kDisabled;
}

void StoreRelease(void* host, u32 value) {
  std::atomic_ref<u32>(*static_cast<u32*>(host)).store(value, std::memory_order_release);
}

u32 LoadAcquire(const void* host) {
  return std::atomic_ref<u32>(*const_cast<u32*>(static_cast<const u32*>(host)))
      .load(std::memory_order_acquire);
}

}  // namespace

CommandProcessor::CommandProcessor(GraphicsSystem* graphics_system, Runtime& runtime)
    : memory_(graphics_system->GetMemory()),
      runtime_(&runtime),
      graphics_system_(graphics_system),
      register_file_(graphics_system_->GetRegisterFile()),
      worker_running_(true),
      write_ptr_index_event_(rex::platform::Event::CreateAutoResetEvent(false)),
      write_ptr_index_(0) {
  REX_ASSERT(write_ptr_index_event_ != nullptr);
}

CommandProcessor::~CommandProcessor() = default;

bool CommandProcessor::Initialize() {
  for (u32 i = 0; i < 256; ++i) {
    u32 value = i * 0x3FF / 0xFF;
    xenos::reg::DC_LUT_30_COLOR& gamma_ramp_entry = gamma_ramp_256_entry_table_[i];
    gamma_ramp_entry.color_10_blue = value;
    gamma_ramp_entry.color_10_green = value;
    gamma_ramp_entry.color_10_red = value;
  }
  for (u32 i = 0; i < 128; ++i) {
    xenos::reg::DC_LUT_PWL_DATA gamma_ramp_entry = {};
    gamma_ramp_entry.base = (i * 0xFFFF / 0x7F) & ~UINT32_C(0x3F);
    gamma_ramp_entry.delta = i < 0x7F ? 0x200 : 0;
    for (u32 j = 0; j < 3; ++j) {
      gamma_ramp_pwl_rgb_[i][j] = gamma_ramp_entry;
    }
  }

  worker_running_ = true;
  auto thread = std::make_unique<runtime::KernelThread>(*runtime_, 128 * 1024, 0, [this]() {
    WorkerThreadMain();
    return 0;
  });
  thread->SetName("GPU Commands");
  worker_thread_ = runtime_->GetThreadRegistry().Launch(std::move(thread)).value_or(0);

  return true;
}

void CommandProcessor::Shutdown() {
  worker_running_ = false;
  write_ptr_index_event_->Set();
  runtime::Wait(*runtime_, worker_thread_, 0, 0, 0, nullptr);
  runtime::CloseOwned(runtime_->GetObjectTable(), worker_thread_);
  worker_thread_ = 0;
}

void CommandProcessor::InitializeShaderStorage(
    [[maybe_unused]] const std::filesystem::path& cache_root, [[maybe_unused]] u32 title_id,
    [[maybe_unused]] bool blocking) {}

bool CommandProcessor::IsWorkerThread() {
  return runtime::KernelThread::IsInThread(
      runtime_->GetThreadRegistry().FindByHandle(worker_thread_).GetThread());
}

bool CommandProcessor::HasPendingFns() {
  std::lock_guard lock(pending_mutex_);
  return !pending_fns_.empty();
}

void CommandProcessor::RunPendingFns() {
  while (true) {
    rex::MoveOnlyFunction<void()> fn;
    {
      std::lock_guard lock(pending_mutex_);
      if (pending_fns_.empty()) {
        return;
      }
      fn = std::move(pending_fns_.front());
      pending_fns_.pop();
    }
    fn();
  }
}

void CommandProcessor::CallInThread(rex::MoveOnlyFunction<void()> fn) {
  if (!HasPendingFns() && IsWorkerThread()) {
    fn();
    return;
  }
  {
    std::lock_guard lock(pending_mutex_);
    pending_fns_.push(std::move(fn));
  }
  write_ptr_index_event_->Set();
}

void CommandProcessor::CallInThreadAndWait(rex::MoveOnlyFunction<void()> fn) {
  if (!worker_running_ || IsWorkerThread()) {
    fn();
    return;
  }
  platform::Fence fence;
  CallInThread([&fn, &fence]() {
    fn();
    fence.Signal();
  });
  fence.Wait();
}

void CommandProcessor::ClearCaches() {}

void CommandProcessor::InvalidateGPUMemory() {}

ReadbackResolveMode CommandProcessor::GetReadbackResolveMode(
    bool legacy_readback_resolve_enabled) const {
  ReadbackResolveMode shared_mode =
      ParseReadbackResolveMode(REXCVAR_GET(::rex::gpu::cvars::readback_resolve));
  bool shared_mode_overrides_legacy = shared_mode != ReadbackResolveMode::kDisabled ||
                                      rex::cvar::IsSet(::rex::gpu::cvars::readback_resolve);
  if (shared_mode_overrides_legacy) {
    return shared_mode;
  }
  return legacy_readback_resolve_enabled ? ReadbackResolveMode::kFast
                                         : ReadbackResolveMode::kDisabled;
}

bool CommandProcessor::IsReadbackMemexportEnabled(bool legacy_backend_flag) const {
  if (legacy_readback_memexport_cvar_ && rex::cvar::IsSet(*legacy_readback_memexport_cvar_)) {
    return legacy_backend_flag;
  }
  return REXCVAR_GET(::rex::gpu::cvars::readback_memexport);
}

void CommandProcessor::SetDesiredSwapPostEffect(SwapPostEffect swap_post_effect) {
  if (swap_post_effect_desired_ == swap_post_effect) {
    return;
  }
  swap_post_effect_desired_ = swap_post_effect;
  CallInThread([this, swap_post_effect]() { swap_post_effect_actual_ = swap_post_effect; });
}

void CommandProcessor::WorkerThreadMain() {
  if (!SetupContext()) {
    REX_FATAL(gpu, "Unable to setup command processor internal state");
  }

  while (worker_running_) {
    RunPendingFns();

    u32 write_ptr_index = write_ptr_index_.load();
    if (write_ptr_index == 0xBAADF00D || read_ptr_index_ == write_ptr_index) {
      REX_PROFILE_SCOPE_TEXT("gpu", "rex::gpu::CommandProcessor::Stall");
      // Spin for new commands, as waiting on the event costs too much.
      PrepareForWait();
      u32 loop_count = 0;
      do {
        if (loop_count > 500) {
          const int wait_time_ms = 5;
          rex::platform::Wait(write_ptr_index_event_.get(), true,
                              std::chrono::milliseconds(wait_time_ms));
        }

        rex::platform::MaybeYield();
        loop_count++;
        write_ptr_index = write_ptr_index_.load();
      } while (worker_running_ && !HasPendingFns() &&
               (write_ptr_index == 0xBAADF00D || read_ptr_index_ == write_ptr_index));
      ReturnFromWait();
      if (!worker_running_ || HasPendingFns()) {
        continue;
      }
    }
    REX_ASSERT(read_ptr_index_ != write_ptr_index);

    read_ptr_index_ = ExecutePrimaryBuffer(read_ptr_index_, write_ptr_index);

    if (read_ptr_writeback_ptr_) {
      memory_->TranslatePhysical<be_u32*>(read_ptr_writeback_ptr_)
          ->Store(read_ptr_index_, std::memory_order_release);
    }
  }

  RunPendingFns();
  ShutdownContext();
}

bool CommandProcessor::SetupContext() {
  return true;
}

void CommandProcessor::ShutdownContext() {}

void CommandProcessor::InitializeRingBuffer(u32 ptr, u32 size_log2) {
  CallInThreadAndWait([this, ptr, size_log2]() {
    read_ptr_index_ = 0;
    primary_buffer_ptr_ = ptr;
    primary_buffer_size_ = static_cast<u32>(1) << (size_log2 + 3);
  });
}

void CommandProcessor::EnableReadPointerWriteBack(u32 ptr, u32 block_size_log2) {
  CallInThreadAndWait([this, ptr, block_size_log2]() {
    read_ptr_writeback_ptr_ = ptr;
    read_ptr_update_freq_ = static_cast<u32>(1) << block_size_log2 >> 2;
  });
}

void CommandProcessor::UpdateWritePointer(u32 value) {
  write_ptr_index_ = value;
  write_ptr_index_event_->Set();
}

u32 CommandProcessor::ReadRegisterValue(u32 index) const {
  if (index < xenos::RegisterFile::kRegisterCount) {
    return register_file_->values[index];
  }
  auto it = extended_register_values_.find(index);
  return it != extended_register_values_.end() ? it->second : 0;
}

void CommandProcessor::WriteRegister(u32 index, u32 value) {
  xenos::RegisterFile& regs = *register_file_;
  if (index >= xenos::RegisterFile::kRegisterCount) {
    auto [it, inserted] = extended_register_values_.insert_or_assign(index, value);
    (void)it;
    if (inserted) {
      REXLOG_WARN(
          gpu,
          "CommandProcessor::WriteRegister index out of bounds: {} (stored as extended register)",
          index);
    }
    return;
  }

  // Volatile for the WAIT_REG_MEM loop.
  const_cast<volatile u32&>(regs.values[index]) = value;
  if (!regs.GetRegisterInfo(index)) {
    REXLOG_DEBUG(gpu, "GPU: Write to unknown register ({:04X} = {:08X})", index, value);
  }

  if (index >= std::to_underlying(xenos::Register::SCRATCH_REG0) &&
      index <= std::to_underlying(xenos::Register::SCRATCH_REG7)) {
    u32 scratch_reg = index - std::to_underlying(xenos::Register::SCRATCH_REG0);
    if ((1 << scratch_reg) & regs.values[std::to_underlying(xenos::Register::SCRATCH_UMSK)]) {
      u32 scratch_addr = regs.values[std::to_underlying(xenos::Register::SCRATCH_ADDR)];
      u32 mem_addr = scratch_addr + (scratch_reg * 4);
      memory_->TranslatePhysical<be_u32*>(mem_addr)->Store(value, std::memory_order_release);
    }
  } else {
    switch (index) {
      // A COHER write sets the dirty flag so the next WAIT_REG_MEM blocks until memory syncs.
      case std::to_underlying(xenos::Register::COHER_STATUS_HOST): {
        const_cast<volatile u32&>(regs.values[index]) |= UINT32_C(0x80000000);
      } break;

      case std::to_underlying(xenos::Register::DC_LUT_RW_INDEX): {
        gamma_ramp_rw_component_ = 0;
      } break;

      case std::to_underlying(xenos::Register::DC_LUT_SEQ_COLOR): {
        REX_ASSERT((regs[std::to_underlying(xenos::Register::DC_LUT_RW_MODE)] & 0b1) == 0);
        auto gamma_ramp_rw_index = regs.Get<xenos::reg::DC_LUT_RW_INDEX>();
        // DC_LUT_SEQ_COLOR is ordered red, green, blue and the write enable mask blue, green, red.
        bool write_gamma_ramp_component =
            (regs[std::to_underlying(xenos::Register::DC_LUT_WRITE_EN_MASK)] &
             (UINT32_C(1) << (2 - gamma_ramp_rw_component_))) != 0;
        if (write_gamma_ramp_component) {
          xenos::reg::DC_LUT_30_COLOR& gamma_ramp_entry =
              gamma_ramp_256_entry_table_[gamma_ramp_rw_index.rw_index];
          // Bits 0:5 are hardwired to zero.
          u32 gamma_ramp_seq_color = regs.Get<xenos::reg::DC_LUT_SEQ_COLOR>().seq_color >> 6;
          switch (gamma_ramp_rw_component_) {
            case 0:
              gamma_ramp_entry.color_10_red = gamma_ramp_seq_color;
              break;
            case 1:
              gamma_ramp_entry.color_10_green = gamma_ramp_seq_color;
              break;
            case 2:
              gamma_ramp_entry.color_10_blue = gamma_ramp_seq_color;
              break;
            default:
              REX_UNHANDLED_CASE(gamma_ramp_rw_component_);
              break;
          }
        }
        if (++gamma_ramp_rw_component_ >= 3) {
          gamma_ramp_rw_component_ = 0;
          xenos::reg::DC_LUT_RW_INDEX new_gamma_ramp_rw_index = gamma_ramp_rw_index;
          ++new_gamma_ramp_rw_index.rw_index;
          WriteRegister(std::to_underlying(xenos::Register::DC_LUT_RW_INDEX),
                        rex::Reinterpret<u32>(new_gamma_ramp_rw_index));
        }
        if (write_gamma_ramp_component) {
          OnGammaRamp256EntryTableValueWritten();
        }
      } break;

      case std::to_underlying(xenos::Register::DC_LUT_PWL_DATA): {
        REX_ASSERT((regs[std::to_underlying(xenos::Register::DC_LUT_RW_MODE)] & 0b1) != 0);
        auto gamma_ramp_rw_index = regs.Get<xenos::reg::DC_LUT_RW_INDEX>();
        // Bit 7 of the index is ignored for PWL.
        u32 gamma_ramp_rw_index_pwl = gamma_ramp_rw_index.rw_index & 0x7F;
        // The write enable bit of a component is 2 minus its index, so the mask is B, G, R order.
        bool write_gamma_ramp_component =
            (regs[std::to_underlying(xenos::Register::DC_LUT_WRITE_EN_MASK)] &
             (UINT32_C(1) << (2 - gamma_ramp_rw_component_))) != 0;
        if (write_gamma_ramp_component) {
          xenos::reg::DC_LUT_PWL_DATA& gamma_ramp_entry =
              gamma_ramp_pwl_rgb_[gamma_ramp_rw_index_pwl][gamma_ramp_rw_component_];
          auto gamma_ramp_value = regs.Get<xenos::reg::DC_LUT_PWL_DATA>();
          // Bits 0:5 are hardwired to zero.
          gamma_ramp_entry.base = gamma_ramp_value.base & ~UINT32_C(0x3F);
          gamma_ramp_entry.delta = gamma_ramp_value.delta & ~UINT32_C(0x3F);
        }
        if (++gamma_ramp_rw_component_ >= 3) {
          gamma_ramp_rw_component_ = 0;
          xenos::reg::DC_LUT_RW_INDEX new_gamma_ramp_rw_index = gamma_ramp_rw_index;
          new_gamma_ramp_rw_index.rw_index = (gamma_ramp_rw_index.rw_index & ~UINT32_C(0x7F)) |
                                             ((gamma_ramp_rw_index_pwl + 1) & 0x7F);
          WriteRegister(std::to_underlying(xenos::Register::DC_LUT_RW_INDEX),
                        rex::Reinterpret<u32>(new_gamma_ramp_rw_index));
        }
        if (write_gamma_ramp_component) {
          OnGammaRampPWLValueWritten();
        }
      } break;

      case std::to_underlying(xenos::Register::DC_LUT_30_COLOR): {
        REX_ASSERT((regs[std::to_underlying(xenos::Register::DC_LUT_RW_MODE)] & 0b1) == 0);
        auto gamma_ramp_rw_index = regs.Get<xenos::reg::DC_LUT_RW_INDEX>();
        u32 gamma_ramp_write_enable_mask =
            regs[std::to_underlying(xenos::Register::DC_LUT_WRITE_EN_MASK)] & 0b111;
        if (gamma_ramp_write_enable_mask) {
          xenos::reg::DC_LUT_30_COLOR& gamma_ramp_entry =
              gamma_ramp_256_entry_table_[gamma_ramp_rw_index.rw_index];
          auto gamma_ramp_value = regs.Get<xenos::reg::DC_LUT_30_COLOR>();
          if (gamma_ramp_write_enable_mask & 0b001) {
            gamma_ramp_entry.color_10_blue = gamma_ramp_value.color_10_blue;
          }
          if (gamma_ramp_write_enable_mask & 0b010) {
            gamma_ramp_entry.color_10_green = gamma_ramp_value.color_10_green;
          }
          if (gamma_ramp_write_enable_mask & 0b100) {
            gamma_ramp_entry.color_10_red = gamma_ramp_value.color_10_red;
          }
        }
        gamma_ramp_rw_component_ = 0;
        xenos::reg::DC_LUT_RW_INDEX new_gamma_ramp_rw_index = gamma_ramp_rw_index;
        ++new_gamma_ramp_rw_index.rw_index;
        WriteRegister(std::to_underlying(xenos::Register::DC_LUT_RW_INDEX),
                      rex::Reinterpret<u32>(new_gamma_ramp_rw_index));
        if (gamma_ramp_write_enable_mask) {
          OnGammaRamp256EntryTableValueWritten();
        }
      } break;

      default:
        break;
    }
  }
}

void CommandProcessor::WriteRegistersFromMem(u32 start_index, u32* base, u32 num_registers) {
  for (u32 i = 0; i < num_registers; ++i) {
    u32 data = memory::load<be_u32>(base + i);
    WriteRegister(start_index + i, data);
  }
}

void CommandProcessor::WriteRegisterRangeFromRing(memory::RingBuffer* ring, u32 base,
                                                  u32 num_registers) {
  if (!num_registers) {
    return;
  }
  memory::RingBuffer::ReadRange range =
      ring->BeginRead(static_cast<size_t>(num_registers) * sizeof(u32));
  if (range.first_length != 0) {
    u32 first_count = static_cast<u32>(range.first_length / sizeof(u32));
    WriteRegistersFromMem(base, reinterpret_cast<u32*>(const_cast<u8*>(range.first)), first_count);
    base += first_count;
  }
  if (range.second_length != 0) {
    WriteRegistersFromMem(base, reinterpret_cast<u32*>(const_cast<u8*>(range.second)),
                          static_cast<u32>(range.second_length / sizeof(u32)));
  }
  ring->EndRead(range);
}

void CommandProcessor::WriteALURangeFromRing(memory::RingBuffer* ring, u32 base,
                                             u32 num_registers) {
  WriteRegisterRangeFromRing(ring, base + 0x4000, num_registers);
}

void CommandProcessor::WriteFetchRangeFromRing(memory::RingBuffer* ring, u32 base,
                                               u32 num_registers) {
  WriteRegisterRangeFromRing(ring, base + 0x4800, num_registers);
}

void CommandProcessor::WriteBoolRangeFromRing(memory::RingBuffer* ring, u32 base,
                                              u32 num_registers) {
  WriteRegisterRangeFromRing(ring, base + 0x4900, num_registers);
}

void CommandProcessor::WriteLoopRangeFromRing(memory::RingBuffer* ring, u32 base,
                                              u32 num_registers) {
  WriteRegisterRangeFromRing(ring, base + 0x4908, num_registers);
}

void CommandProcessor::WriteREGISTERSRangeFromRing(memory::RingBuffer* ring, u32 base,
                                                   u32 num_registers) {
  WriteRegisterRangeFromRing(ring, base + 0x2000, num_registers);
}

void CommandProcessor::WriteALURangeFromMem(u32 start_index, u32* base, u32 num_registers) {
  WriteRegistersFromMem(start_index + 0x4000, base, num_registers);
}

void CommandProcessor::WriteFetchRangeFromMem(u32 start_index, u32* base, u32 num_registers) {
  WriteRegistersFromMem(start_index + 0x4800, base, num_registers);
}

void CommandProcessor::WriteBoolRangeFromMem(u32 start_index, u32* base, u32 num_registers) {
  WriteRegistersFromMem(start_index + 0x4900, base, num_registers);
}

void CommandProcessor::WriteLoopRangeFromMem(u32 start_index, u32* base, u32 num_registers) {
  WriteRegistersFromMem(start_index + 0x4908, base, num_registers);
}

void CommandProcessor::WriteREGISTERSRangeFromMem(u32 start_index, u32* base, u32 num_registers) {
  WriteRegistersFromMem(start_index + 0x2000, base, num_registers);
}

void CommandProcessor::SyncCoherency() {
  REX_PROFILE_SCOPE("gpu");

  // Volatile because this may be called from the WAIT_REG_MEM loop.
  volatile u32* regs_volatile = register_file_->values;
  auto status_host = rex::Reinterpret<xenos::reg::COHER_STATUS_HOST>(
      static_cast<u32>(regs_volatile[std::to_underlying(xenos::Register::COHER_STATUS_HOST)]));
  u32 base_host = regs_volatile[std::to_underlying(xenos::Register::COHER_BASE_HOST)];
  u32 size_host = regs_volatile[std::to_underlying(xenos::Register::COHER_SIZE_HOST)];

  if (!status_host.status) {
    return;
  }

  const char* action = "N/A";
  if (status_host.vc_action_ena && status_host.tc_action_ena) {
    action = "VC | TC";
  } else if (status_host.tc_action_ena) {
    action = "TC";
  } else if (status_host.vc_action_ena) {
    action = "VC";
  }

  REXLOG_TRACE(gpu, "Make {:08X} -> {:08X} ({}b) coherent, action = {}", base_host,
               base_host + size_host, size_host, action);

  regs_volatile[std::to_underlying(xenos::Register::COHER_STATUS_HOST)] = 0;
}

void CommandProcessor::PrepareForWait() {}

void CommandProcessor::ReturnFromWait() {}

u32 CommandProcessor::ExecutePrimaryBuffer(u32 read_index, u32 write_index) {
  REX_PROFILE_SCOPE("gpu");

  memory::RingBuffer reader(memory_->TranslatePhysical(primary_buffer_ptr_), primary_buffer_size_);
  reader.SetReadOffset(read_index * sizeof(u32));
  reader.SetWriteOffset(write_index * sizeof(u32));
  do {
    if (!ExecutePacket(&reader)) {
      REXLOG_ERROR(gpu, "**** PRIMARY RINGBUFFER: Failed to execute packet.");
      REX_ASSERT_ALWAYS();
      break;
    }
  } while (reader.GetReadCount());

  OnPrimaryBufferEnd();

  return write_index;
}

void CommandProcessor::ExecuteIndirectBuffer(u32 ptr, u32 count) {
  REX_PROFILE_SCOPE("gpu");

  memory::RingBuffer reader(memory_->TranslatePhysical(ptr), count * sizeof(u32));
  reader.SetWriteOffset(count * sizeof(u32));
  do {
    if (!ExecutePacket(&reader)) {
      REXLOG_ERROR(gpu, "**** INDIRECT RINGBUFFER: Failed to execute packet.");
      REX_ASSERT_ALWAYS();
      break;
    }
  } while (reader.GetReadCount());
}

void CommandProcessor::ExecutePacket(u32 ptr, u32 count) {
  memory::RingBuffer reader(memory_->TranslatePhysical(ptr), count * sizeof(u32));
  reader.SetWriteOffset(count * sizeof(u32));
  do {
    if (!ExecutePacket(&reader)) {
      REXLOG_ERROR(gpu, "**** ExecutePacket: Failed to execute packet.");
      REX_ASSERT_ALWAYS();
      break;
    }
  } while (reader.GetReadCount());
}

bool CommandProcessor::ExecutePacket(memory::RingBuffer* reader) {
  const u32 packet = reader->ReadAndSwap<u32>();
  const u32 packet_type = packet >> 30;
  if (packet == 0) {
    return true;
  }

  if (packet == 0xCDCDCDCD) {
    REXLOG_WARN(gpu, "GPU packet is CDCDCDCD - probably read uninitialized memory!");
  }

  switch (packet_type) {
    case 0x00:
      return ExecutePacketType0(reader, packet);
    case 0x01:
      return ExecutePacketType1(reader, packet);
    case 0x02:
      return ExecutePacketType2(reader, packet);
    case 0x03:
      return ExecutePacketType3(reader, packet);
    default:
      REX_UNHANDLED_CASE(packet_type);
      return false;
  }
}

bool CommandProcessor::ExecutePacketType0(memory::RingBuffer* reader, u32 packet) {
  u32 count = ((packet >> 16) & 0x3FFF) + 1;
  if (reader->GetReadCount() < count * sizeof(u32)) {
    REX_FAIL(gpu, false, "ExecutePacketType0 overflow (read count {:08X}, packet count {:08X})",
             reader->GetReadCount(), count * sizeof(u32));
  }

  u32 base_index = (packet & 0x7FFF);
  u32 write_one_reg = (packet >> 15) & 0x1;
  for (u32 m = 0; m < count; m++) {
    u32 reg_data = reader->ReadAndSwap<u32>();
    u32 target_index = write_one_reg ? base_index : base_index + m;
    WriteRegister(target_index, reg_data);
  }

  return true;
}

bool CommandProcessor::ExecutePacketType1(memory::RingBuffer* reader, u32 packet) {
  u32 reg_index_1 = packet & 0x7FF;
  u32 reg_index_2 = (packet >> 11) & 0x7FF;
  u32 reg_data_1 = reader->ReadAndSwap<u32>();
  u32 reg_data_2 = reader->ReadAndSwap<u32>();
  WriteRegister(reg_index_1, reg_data_1);
  WriteRegister(reg_index_2, reg_data_2);
  return true;
}

bool CommandProcessor::ExecutePacketType2([[maybe_unused]] memory::RingBuffer* reader,
                                          [[maybe_unused]] u32 packet) {
  return true;
}

bool CommandProcessor::ExecutePacketType3(memory::RingBuffer* reader, u32 packet) {
  u32 opcode = (packet >> 8) & 0x7F;
  u32 count = ((packet >> 16) & 0x3FFF) + 1;
  auto data_start_offset = reader->GetReadOffset();

  if (reader->GetReadCount() < count * sizeof(u32)) {
    REX_FAIL(gpu, false, "ExecutePacketType3 overflow (read count {:08X}, packet count {:08X})",
             reader->GetReadCount(), count * sizeof(u32));
  }

  if (packet & 1) {
    bool any_pass = (bin_select_ & bin_mask_) != 0;
    if (!any_pass || opcode == PM4_XE_SWAP) {
      reader->AdvanceRead(count * sizeof(u32));
      return true;
    }
  }

  bool result = false;
  switch (opcode) {
    case PM4_ME_INIT:
      result = ExecutePacketType3_ME_INIT(reader, packet, count);
      break;
    case PM4_NOP:
      result = ExecutePacketType3_NOP(reader, packet, count);
      break;
    case PM4_INTERRUPT:
      result = ExecutePacketType3_INTERRUPT(reader, packet, count);
      break;
    case PM4_XE_SWAP:
      result = ExecutePacketType3_XE_SWAP(reader, packet, count);
      break;
    case PM4_INDIRECT_BUFFER:
    case PM4_INDIRECT_BUFFER_PFD:
      result = ExecutePacketType3_INDIRECT_BUFFER(reader, packet, count);
      break;
    case PM4_WAIT_REG_MEM:
      result = ExecutePacketType3_WAIT_REG_MEM(reader, packet, count);
      break;
    case PM4_REG_RMW:
      result = ExecutePacketType3_REG_RMW(reader, packet, count);
      break;
    case PM4_REG_TO_MEM:
      result = ExecutePacketType3_REG_TO_MEM(reader, packet, count);
      break;
    case PM4_MEM_WRITE:
      result = ExecutePacketType3_MEM_WRITE(reader, packet, count);
      break;
    case PM4_COND_WRITE:
      result = ExecutePacketType3_COND_WRITE(reader, packet, count);
      break;
    case PM4_EVENT_WRITE:
      result = ExecutePacketType3_EVENT_WRITE(reader, packet, count);
      break;
    case PM4_EVENT_WRITE_SHD:
      result = ExecutePacketType3_EVENT_WRITE_SHD(reader, packet, count);
      break;
    case PM4_EVENT_WRITE_EXT:
      result = ExecutePacketType3_EVENT_WRITE_EXT(reader, packet, count);
      break;
    case PM4_EVENT_WRITE_ZPD:
      result = ExecutePacketType3_EVENT_WRITE_ZPD(reader, packet, count);
      break;
    case PM4_DRAW_INDX:
      result = ExecutePacketType3_DRAW_INDX(reader, packet, count);
      break;
    case PM4_DRAW_INDX_2:
      result = ExecutePacketType3_DRAW_INDX_2(reader, packet, count);
      break;
    case PM4_SET_CONSTANT:
      result = ExecutePacketType3_SET_CONSTANT(reader, packet, count);
      break;
    case PM4_SET_CONSTANT2:
      result = ExecutePacketType3_SET_CONSTANT2(reader, packet, count);
      break;
    case PM4_LOAD_ALU_CONSTANT:
      result = ExecutePacketType3_LOAD_ALU_CONSTANT(reader, packet, count);
      break;
    case PM4_SET_SHADER_CONSTANTS:
      result = ExecutePacketType3_SET_SHADER_CONSTANTS(reader, packet, count);
      break;
    case PM4_IM_LOAD:
      result = ExecutePacketType3_IM_LOAD(reader, packet, count);
      break;
    case PM4_IM_LOAD_IMMEDIATE:
      result = ExecutePacketType3_IM_LOAD_IMMEDIATE(reader, packet, count);
      break;
    case PM4_INVALIDATE_STATE:
      result = ExecutePacketType3_INVALIDATE_STATE(reader, packet, count);
      break;
    case PM4_VIZ_QUERY:
      result = ExecutePacketType3_VIZ_QUERY(reader, packet, count);
      break;

    case PM4_SET_BIN_MASK_LO: {
      u32 value = reader->ReadAndSwap<u32>();
      bin_mask_ = (bin_mask_ & 0xFFFFFFFF00000000ull) | value;
      result = true;
    } break;
    case PM4_SET_BIN_MASK_HI: {
      u32 value = reader->ReadAndSwap<u32>();
      bin_mask_ = (bin_mask_ & 0xFFFFFFFFull) | (static_cast<u64>(value) << 32);
      result = true;
    } break;
    case PM4_SET_BIN_SELECT_LO: {
      u32 value = reader->ReadAndSwap<u32>();
      bin_select_ = (bin_select_ & 0xFFFFFFFF00000000ull) | value;
      result = true;
    } break;
    case PM4_SET_BIN_SELECT_HI: {
      u32 value = reader->ReadAndSwap<u32>();
      bin_select_ = (bin_select_ & 0xFFFFFFFFull) | (static_cast<u64>(value) << 32);
      result = true;
    } break;
    case PM4_SET_BIN_MASK: {
      REX_ASSERT(count == 2);
      u64 val_hi = reader->ReadAndSwap<u32>();
      u64 val_lo = reader->ReadAndSwap<u32>();
      bin_mask_ = (val_hi << 32) | val_lo;
      result = true;
    } break;
    case PM4_SET_BIN_SELECT: {
      REX_ASSERT(count == 2);
      u64 val_hi = reader->ReadAndSwap<u32>();
      u64 val_lo = reader->ReadAndSwap<u32>();
      bin_select_ = (val_hi << 32) | val_lo;
      result = true;
    } break;
    case PM4_CONTEXT_UPDATE: {
      REX_ASSERT(count == 1);
      u32 value = reader->ReadAndSwap<u32>();
      REXLOG_TRACE(gpu, "GPU context update = {:08X}", value);
      REX_ASSERT(value == 0);
      result = true;
      break;
    }
    case PM4_WAIT_FOR_IDLE: {
      REX_ASSERT(count == 1);
      u32 value = reader->ReadAndSwap<u32>();
      REXLOG_TRACE(gpu, "GPU wait for idle = {:08X}", value);
      result = true;
      break;
    }

    default:
      REXLOG_DEV(gpu, "Unimplemented GPU OPCODE: 0x{:02X}\t\tCOUNT: {}\n", opcode, count);
      REX_ASSERT_ALWAYS();
      reader->AdvanceRead(count * sizeof(u32));
      break;
  }

  REX_ASSERT(reader->GetReadOffset() ==
             (data_start_offset + (count * sizeof(u32))) % reader->GetCapacity());
  return result;
}

bool CommandProcessor::ExecutePacketType3_ME_INIT(memory::RingBuffer* reader,
                                                  [[maybe_unused]] u32 packet, u32 count) {
  me_bin_.clear();
  for (u32 i = 0; i < count; i++) {
    me_bin_.push_back(reader->ReadAndSwap<u32>());
  }

  return true;
}

bool CommandProcessor::ExecutePacketType3_NOP(memory::RingBuffer* reader,
                                              [[maybe_unused]] u32 packet, u32 count) {
  reader->AdvanceRead(count * sizeof(u32));
  return true;
}

bool CommandProcessor::ExecutePacketType3_INTERRUPT(memory::RingBuffer* reader,
                                                    [[maybe_unused]] u32 packet,
                                                    [[maybe_unused]] u32 count) {
  REX_PROFILE_SCOPE("gpu");

  u32 cpu_mask = reader->ReadAndSwap<u32>();
  for (int n = 0; n < 6; n++) {
    if (cpu_mask & (1 << n)) {
      if (graphics_system_) {
        graphics_system_->DispatchInterruptCallback(1, n);
      }
    }
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_XE_SWAP(memory::RingBuffer* reader,
                                                  [[maybe_unused]] u32 packet, u32 count) {
  REX_PROFILE_SCOPE("gpu");

#ifdef REXGLUE_ENABLE_PERF_COUNTERS
  {
    static u64 last_frame_tick = 0;
    u64 now = rex::Clock::QueryHostTickCount();
    if (last_frame_tick) {
      u64 freq = rex::Clock::QueryHostTickFrequency();
      i64 dt_us = static_cast<i64>((now - last_frame_tick) * 1000000 / freq);
      REX_PERF_SET(kFrameTimeUs, dt_us);
      REX_PERF_SET(kFps, freq / (now - last_frame_tick));
    }
    last_frame_tick = now;
  }
#endif
  rex::platform::Profiler::Flip();

  u32 magic = reader->ReadAndSwap<memory::FourCCT>();
  REX_ASSERT(magic == kSwapSignature);

  u32 frontbuffer_ptr = reader->ReadAndSwap<u32>();
  u32 frontbuffer_width = reader->ReadAndSwap<u32>();
  u32 frontbuffer_height = reader->ReadAndSwap<u32>();
  reader->AdvanceRead((count - 4) * sizeof(u32));

  IssueSwap(frontbuffer_ptr, frontbuffer_width, frontbuffer_height);

  ++counter_;
  return true;
}

bool CommandProcessor::ExecutePacketType3_INDIRECT_BUFFER(memory::RingBuffer* reader,
                                                          [[maybe_unused]] u32 packet,
                                                          [[maybe_unused]] u32 count) {
  u32 list_ptr = CPUToGPU(reader->ReadAndSwap<u32>());
  u32 list_length = reader->ReadAndSwap<u32>();
  REX_ASSERT((list_length & ~0xFFFFF) == 0);
  list_length &= 0xFFFFF;
  ExecuteIndirectBuffer(GPUToCPU(list_ptr), list_length);
  return true;
}

bool CommandProcessor::ExecutePacketType3_WAIT_REG_MEM(memory::RingBuffer* reader,
                                                       [[maybe_unused]] u32 packet,
                                                       [[maybe_unused]] u32 count) {
  REX_PROFILE_SCOPE("gpu");

  u32 wait_info = reader->ReadAndSwap<u32>();
  u32 poll_reg_addr = reader->ReadAndSwap<u32>();
  u32 ref = reader->ReadAndSwap<u32>();
  u32 mask = reader->ReadAndSwap<u32>();
  u32 wait = reader->ReadAndSwap<u32>();

  bool is_memory = (wait_info & 0x10) != 0;

  bool matched = false;
  do {
    u32 value = 0;
    if (is_memory) {
      value = LoadAcquire(memory_->TranslatePhysical(poll_reg_addr & ~static_cast<u32>(0x3)));
      value = xenos::GPUSwap(value, static_cast<xenos::Endian>(poll_reg_addr & 0x3));
    } else {
      value = ReadRegisterValue(poll_reg_addr);
      if (poll_reg_addr == std::to_underlying(xenos::Register::COHER_STATUS_HOST)) {
        SyncCoherency();
        value = ReadRegisterValue(poll_reg_addr);
      }
    }
    const u32 wait_function = wait_info & 0x7;
    switch (wait_function) {
      case 0x0:
        matched = false;
        break;
      case 0x1:
        matched = (value & mask) < ref;
        break;
      case 0x2:
        matched = (value & mask) <= ref;
        break;
      case 0x3:
        matched = (value & mask) == ref;
        break;
      case 0x4:
        matched = (value & mask) != ref;
        break;
      case 0x5:
        matched = (value & mask) >= ref;
        break;
      case 0x6:
        matched = (value & mask) > ref;
        break;
      case 0x7:
        matched = true;
        break;
      default:
        REX_UNHANDLED_CASE(wait_function);
        break;
    }
    if (!matched) {
      if (wait >= 0x100) {
        PrepareForWait();
        if (!REXCVAR_GET(::rex::gpu::cvars::vsync)) {
          rex::platform::MaybeYield();
        } else {
          rex::platform::Sleep(std::chrono::milliseconds(wait / 0x100));
        }
        rex::platform::SyncMemory();
        ReturnFromWait();

        if (!worker_running_) {
          return false;
        }
      } else {
        rex::platform::MaybeYield();
      }
    }
  } while (!matched);

  return true;
}

bool CommandProcessor::ExecutePacketType3_REG_RMW(memory::RingBuffer* reader,
                                                  [[maybe_unused]] u32 packet,
                                                  [[maybe_unused]] u32 count) {
  u32 rmw_info = reader->ReadAndSwap<u32>();
  u32 and_mask = reader->ReadAndSwap<u32>();
  u32 or_mask = reader->ReadAndSwap<u32>();
  u32 value = register_file_->values[rmw_info & 0x1FFF];
  if ((rmw_info >> 31) & 0x1) {
    value &= register_file_->values[and_mask & 0x1FFF];
  } else {
    value &= and_mask;
  }
  if ((rmw_info >> 30) & 0x1) {
    value |= register_file_->values[or_mask & 0x1FFF];
  } else {
    value |= or_mask;
  }
  WriteRegister(rmw_info & 0x1FFF, value);
  return true;
}

bool CommandProcessor::ExecutePacketType3_REG_TO_MEM(memory::RingBuffer* reader,
                                                     [[maybe_unused]] u32 packet,
                                                     [[maybe_unused]] u32 count) {
  u32 reg_addr = reader->ReadAndSwap<u32>();
  u32 mem_addr = reader->ReadAndSwap<u32>();

  u32 reg_val = ReadRegisterValue(reg_addr);

  auto endianness = static_cast<xenos::Endian>(mem_addr & 0x3);
  mem_addr &= ~0x3;
  reg_val = GPUSwap(reg_val, endianness);
  StoreRelease(memory_->TranslatePhysical(mem_addr), reg_val);

  return true;
}

bool CommandProcessor::ExecutePacketType3_MEM_WRITE(memory::RingBuffer* reader,
                                                    [[maybe_unused]] u32 packet, u32 count) {
  u32 write_addr = reader->ReadAndSwap<u32>();
  for (u32 i = 0; i < count - 1; i++) {
    u32 write_data = reader->ReadAndSwap<u32>();

    auto endianness = static_cast<xenos::Endian>(write_addr & 0x3);
    auto addr = write_addr & ~0x3;
    write_data = GPUSwap(write_data, endianness);
    StoreRelease(memory_->TranslatePhysical(addr), write_data);
    write_addr += 4;
  }

  return true;
}

bool CommandProcessor::ExecutePacketType3_COND_WRITE(memory::RingBuffer* reader,
                                                     [[maybe_unused]] u32 packet,
                                                     [[maybe_unused]] u32 count) {
  u32 wait_info = reader->ReadAndSwap<u32>();
  u32 poll_reg_addr = reader->ReadAndSwap<u32>();
  u32 ref = reader->ReadAndSwap<u32>();
  u32 mask = reader->ReadAndSwap<u32>();
  u32 write_reg_addr = reader->ReadAndSwap<u32>();
  u32 write_data = reader->ReadAndSwap<u32>();
  u32 value;
  if (wait_info & 0x10) {
    auto endianness = static_cast<xenos::Endian>(poll_reg_addr & 0x3);
    poll_reg_addr &= ~0x3;
    value = LoadAcquire(memory_->TranslatePhysical(poll_reg_addr));
    value = GPUSwap(value, endianness);
  } else {
    value = ReadRegisterValue(poll_reg_addr);
  }
  bool matched = false;
  const u32 wait_function = wait_info & 0x7;
  switch (wait_function) {
    case 0x0:
      matched = false;
      break;
    case 0x1:
      matched = (value & mask) < ref;
      break;
    case 0x2:
      matched = (value & mask) <= ref;
      break;
    case 0x3:
      matched = (value & mask) == ref;
      break;
    case 0x4:
      matched = (value & mask) != ref;
      break;
    case 0x5:
      matched = (value & mask) >= ref;
      break;
    case 0x6:
      matched = (value & mask) > ref;
      break;
    case 0x7:
      matched = true;
      break;
    default:
      REX_UNHANDLED_CASE(wait_function);
      break;
  }
  if (matched) {
    if (wait_info & 0x100) {
      auto endianness = static_cast<xenos::Endian>(write_reg_addr & 0x3);
      write_reg_addr &= ~0x3;
      write_data = GPUSwap(write_data, endianness);
      StoreRelease(memory_->TranslatePhysical(write_reg_addr), write_data);
    } else {
      WriteRegister(write_reg_addr, write_data);
    }
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_EVENT_WRITE(memory::RingBuffer* reader,
                                                      [[maybe_unused]] u32 packet, u32 count) {
  u32 initiator = reader->ReadAndSwap<u32>();
  WriteRegister(std::to_underlying(xenos::Register::VGT_EVENT_INITIATOR), initiator & 0x3F);
  if (count == 1) {
  } else {
    REX_ASSERT_ALWAYS();
    reader->AdvanceRead((count - 1) * sizeof(u32));
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_EVENT_WRITE_SHD(memory::RingBuffer* reader,
                                                          [[maybe_unused]] u32 packet,
                                                          [[maybe_unused]] u32 count) {
  u32 initiator = reader->ReadAndSwap<u32>();
  u32 address = reader->ReadAndSwap<u32>();
  u32 value = reader->ReadAndSwap<u32>();

  WriteRegister(std::to_underlying(xenos::Register::VGT_EVENT_INITIATOR), initiator & 0x3F);
  u32 data_value;
  if ((initiator >> 31) & 0x1) {
    data_value = counter_;
  } else {
    data_value = value;
  }
  auto endianness = static_cast<xenos::Endian>(address & 0x3);
  address &= ~0x3;
  data_value = GPUSwap(data_value, endianness);
  StoreRelease(memory_->TranslatePhysical(address), data_value);
  return true;
}

bool CommandProcessor::ExecutePacketType3_EVENT_WRITE_EXT(memory::RingBuffer* reader,
                                                          [[maybe_unused]] u32 packet,
                                                          [[maybe_unused]] u32 count) {
  u32 initiator = reader->ReadAndSwap<u32>();
  u32 address = reader->ReadAndSwap<u32>();
  WriteRegister(std::to_underlying(xenos::Register::VGT_EVENT_INITIATOR), initiator & 0x3F);
  auto endianness = static_cast<xenos::Endian>(address & 0x3);
  address &= ~0x3;

  u16 extents[] = {
      0 >> 3,                                    // min x
      xenos::kTexture2DCubeMaxWidthHeight >> 3,  // max x
      0 >> 3,                                    // min y
      xenos::kTexture2DCubeMaxWidthHeight >> 3,  // max y
      0,                                         // min z
      1,                                         // max z
  };
  REX_ASSERT(endianness == xenos::Endian::k8in16);
  memory::copy_and_swap_16_unaligned(memory_->TranslatePhysical(address), extents,
                                     rex::CountOf(extents));
  return true;
}

void CommandProcessor::WriteSampleCounts(xenos::DepthSampleCounts& counts, u32 samples) {
  counts.Total_A = samples;
  counts.Total_B = 0;
  counts.StencilFail_A = 0;
  counts.StencilFail_B = 0;
  counts.ZFail_B.Store(0, std::memory_order_release);
  counts.ZFail_A.Store(0, std::memory_order_release);
  counts.ZPass_B.Store(0, std::memory_order_release);
  counts.ZPass_A.Store(samples, std::memory_order_release);
}

bool CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(memory::RingBuffer* reader,
                                                          [[maybe_unused]] u32 packet, u32 count) {
  // Set by D3D as BE but struct ABI is LE
  constexpr u32 kQueryFinished = std::byteswap(0xFFFFFEEDu);
  REX_ASSERT(count == 1);
  u32 initiator = reader->ReadAndSwap<u32>();
  WriteRegister(std::to_underlying(xenos::Register::VGT_EVENT_INITIATOR), initiator & 0x3F);

  auto fake_sample_count = REXCVAR_GET(::rex::gpu::cvars::query_occlusion_fake_sample_count);
  if (fake_sample_count >= 0) {
    auto* sample_counts = memory_->TranslatePhysical<DepthSampleCounts*>(
        register_file_->values[std::to_underlying(xenos::Register::RB_SAMPLE_COUNT_ADDR)]);
    if (!sample_counts) {
      return true;
    }
    // D3D writes 0xFFFFFEED to these locations only on D3DISSUE_END to mark a finished query.
    bool is_end_via_z_pass =
        sample_counts->ZPass_A == kQueryFinished || sample_counts->ZPass_B == kQueryFinished;
    // Older D3D versions also check ZFail.
    bool is_end_via_z_fail =
        sample_counts->ZFail_A == kQueryFinished || sample_counts->ZFail_B == kQueryFinished;
    WriteSampleCounts(*sample_counts, is_end_via_z_pass || is_end_via_z_fail
                                          ? static_cast<u32>(fake_sample_count)
                                          : 0);
  }

  return true;
}

bool CommandProcessor::ExecutePacketType3Draw(memory::RingBuffer* reader,
                                              [[maybe_unused]] u32 packet, const char* opcode_name,
                                              [[maybe_unused]] u32 viz_query_condition,
                                              u32 count_remaining) {
  REX_ASSERT(count_remaining != 0);
  if (!count_remaining) {
    REX_FAIL(gpu, false, "{}: Packet too small, can't read VGT_DRAW_INITIATOR", opcode_name);
  }
  xenos::reg::VGT_DRAW_INITIATOR vgt_draw_initiator;
  vgt_draw_initiator.value = reader->ReadAndSwap<u32>();
  --count_remaining;
  WriteRegister(std::to_underlying(xenos::Register::VGT_DRAW_INITIATOR), vgt_draw_initiator.value);

  bool draw_succeeded = true;
  bool is_indexed = false;
  IndexBufferInfo index_buffer_info;
  switch (vgt_draw_initiator.source_select) {
    case xenos::SourceSelect::kDMA: {
      is_indexed = true;

      // Check bounds separately so a single missing value skips 1 word, not 2.
      REX_ASSERT(count_remaining != 0);
      if (!count_remaining) {
        REX_FAIL(gpu, false, "{}: Packet too small, can't read VGT_DMA_BASE", opcode_name);
      }
      u32 vgt_dma_base = reader->ReadAndSwap<u32>();
      --count_remaining;
      WriteRegister(std::to_underlying(xenos::Register::VGT_DMA_BASE), vgt_dma_base);
      xenos::reg::VGT_DMA_SIZE vgt_dma_size;
      REX_ASSERT(count_remaining != 0);
      if (!count_remaining) {
        REX_FAIL(gpu, false, "{}: Packet too small, can't read VGT_DMA_SIZE", opcode_name);
      }
      vgt_dma_size.value = reader->ReadAndSwap<u32>();
      --count_remaining;
      WriteRegister(std::to_underlying(xenos::Register::VGT_DMA_SIZE), vgt_dma_size.value);

      u32 index_size_bytes =
          vgt_draw_initiator.index_size == xenos::IndexFormat::kInt16 ? sizeof(u16) : sizeof(u32);
      // The base address is word aligned per R6xx, masked for safety.
      index_buffer_info.guest_base = vgt_dma_base & ~(index_size_bytes - 1);
      index_buffer_info.endianness = vgt_dma_size.swap_mode;
      index_buffer_info.format = vgt_draw_initiator.index_size;
      index_buffer_info.length = static_cast<size_t>(vgt_dma_size.num_words) * index_size_bytes;
      index_buffer_info.count = vgt_draw_initiator.num_indices;
    } break;
    case xenos::SourceSelect::kImmediate: {
      REXLOG_ERROR(gpu,
                   "{}: Using immediate vertex indices, which are not supported yet. "
                   "Report the title to ReXGlue developers!",
                   opcode_name, static_cast<u32>(vgt_draw_initiator.source_select));
      draw_succeeded = false;
      REX_ASSERT_ALWAYS();
    } break;
    case xenos::SourceSelect::kAutoIndex: {
      index_buffer_info.guest_base = 0;
      index_buffer_info.length = 0;
    } break;
    default: {
      draw_succeeded = false;
      REX_UNHANDLED_CASE(vgt_draw_initiator.source_select);
    } break;
  }

  reader->AdvanceRead(count_remaining * sizeof(u32));

  if (draw_succeeded) {
    auto viz_query = register_file_->Get<xenos::reg::PA_SC_VIZ_QUERY>();
    if (!(viz_query.viz_query_ena && viz_query.kill_pix_post_hi_z)) {
      bool major_mode_explicit =
          xenos::IsMajorModeExplicit(vgt_draw_initiator.major_mode, vgt_draw_initiator.prim_type);
      draw_succeeded = IssueDraw(vgt_draw_initiator.prim_type, vgt_draw_initiator.num_indices,
                                 is_indexed ? &index_buffer_info : nullptr, major_mode_explicit);
      if (!draw_succeeded) {
        auto vgt_output_path_cntl = register_file_->Get<xenos::reg::VGT_OUTPUT_PATH_CNTL>();
        auto vgt_hos_cntl = register_file_->Get<xenos::reg::VGT_HOS_CNTL>();
        auto rb_modecontrol = register_file_->Get<xenos::reg::RB_MODECONTROL>();
        REXLOG_ERROR(
            gpu,
            "{}({}, {}, {}): Failed in backend "
            "(major_mode={}, explicit_major={}, path_select={}, tess_mode={}, edram_mode={})",
            opcode_name, static_cast<u32>(vgt_draw_initiator.num_indices),
            static_cast<u32>(vgt_draw_initiator.prim_type),
            static_cast<u32>(vgt_draw_initiator.source_select),
            static_cast<u32>(vgt_draw_initiator.major_mode), static_cast<u32>(major_mode_explicit),
            static_cast<u32>(vgt_output_path_cntl.path_select),
            static_cast<u32>(vgt_hos_cntl.tess_mode), static_cast<u32>(rb_modecontrol.edram_mode));
      }
    }
  }

  // Keep processing after a dropped draw, as ending the buffer is more inconsistent.
  return true;
}

bool CommandProcessor::ExecutePacketType3_DRAW_INDX(memory::RingBuffer* reader, u32 packet,
                                                    u32 count) {
  u32 count_remaining = count;
  REX_ASSERT(count_remaining != 0);
  if (!count_remaining) {
    REX_FAIL(gpu, false, "PM4_DRAW_INDX: Packet too small, can't read the viz query token");
  }
  u32 viz_query_condition = reader->ReadAndSwap<u32>();
  --count_remaining;
  return ExecutePacketType3Draw(reader, packet, "PM4_DRAW_INDX", viz_query_condition,
                                count_remaining);
}

bool CommandProcessor::ExecutePacketType3_DRAW_INDX_2(memory::RingBuffer* reader, u32 packet,
                                                      u32 count) {
  return ExecutePacketType3Draw(reader, packet, "PM4_DRAW_INDX_2", 0, count);
}

bool CommandProcessor::ExecutePacketType3_SET_CONSTANT(memory::RingBuffer* reader,
                                                       [[maybe_unused]] u32 packet, u32 count) {
  u32 offset_type = reader->ReadAndSwap<u32>();
  u32 index = offset_type & 0x7FF;
  u32 type = (offset_type >> 16) & 0xFF;
  u32 count_registers = count - 1;
  switch (type) {
    case 0:
      WriteALURangeFromRing(reader, index, count_registers);
      break;
    case 1:
      WriteFetchRangeFromRing(reader, index, count_registers);
      break;
    case 2:
      WriteBoolRangeFromRing(reader, index, count_registers);
      break;
    case 3:
      WriteLoopRangeFromRing(reader, index, count_registers);
      break;
    case 4:
      WriteREGISTERSRangeFromRing(reader, index, count_registers);
      break;
    default:
      REX_ASSERT_ALWAYS();
      reader->AdvanceRead((count - 1) * sizeof(u32));
      return true;
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_SET_CONSTANT2(memory::RingBuffer* reader,
                                                        [[maybe_unused]] u32 packet, u32 count) {
  u32 offset_type = reader->ReadAndSwap<u32>();
  u32 index = offset_type & 0xFFFF;
  WriteRegisterRangeFromRing(reader, index, count - 1);
  return true;
}

bool CommandProcessor::ExecutePacketType3_LOAD_ALU_CONSTANT(memory::RingBuffer* reader,
                                                            [[maybe_unused]] u32 packet,
                                                            [[maybe_unused]] u32 count) {
  u32 address = reader->ReadAndSwap<u32>();
  address &= 0x3FFFFFFF;
  u32 offset_type = reader->ReadAndSwap<u32>();
  u32 index = offset_type & 0x7FF;
  u32 size_dwords = reader->ReadAndSwap<u32>();
  size_dwords &= 0xFFF;
  u32 type = (offset_type >> 16) & 0xFF;
  u32* xlat_address = memory_->TranslatePhysical<u32*>(address);
  switch (type) {
    case 0:
      WriteALURangeFromMem(index, xlat_address, size_dwords);
      break;
    case 1:
      WriteFetchRangeFromMem(index, xlat_address, size_dwords);
      break;
    case 2:
      WriteBoolRangeFromMem(index, xlat_address, size_dwords);
      break;
    case 3:
      WriteLoopRangeFromMem(index, xlat_address, size_dwords);
      break;
    case 4:
      WriteREGISTERSRangeFromMem(index, xlat_address, size_dwords);
      break;
    default:
      REX_ASSERT_ALWAYS();
      return true;
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_SET_SHADER_CONSTANTS(memory::RingBuffer* reader,
                                                               [[maybe_unused]] u32 packet,
                                                               u32 count) {
  u32 offset_type = reader->ReadAndSwap<u32>();
  u32 index = offset_type & 0xFFFF;
  WriteRegisterRangeFromRing(reader, index, count - 1);
  return true;
}

bool CommandProcessor::ExecutePacketType3_IM_LOAD(memory::RingBuffer* reader,
                                                  [[maybe_unused]] u32 packet,
                                                  [[maybe_unused]] u32 count) {
  REX_PROFILE_SCOPE("gpu");

  u32 addr_type = reader->ReadAndSwap<u32>();
  auto shader_type = static_cast<xenos::ShaderType>(addr_type & 0x3);
  u32 addr = addr_type & ~0x3;
  u32 start_size = reader->ReadAndSwap<u32>();
  u32 start = start_size >> 16;
  u32 size_dwords = start_size & 0xFFFF;
  REX_ASSERT(start == 0);

  auto shader = LoadShader(shader_type, addr, memory_->TranslatePhysical<u32*>(addr), size_dwords);
  switch (shader_type) {
    case xenos::ShaderType::kVertex:
      active_vertex_shader_ = shader;
      break;
    case xenos::ShaderType::kPixel:
      active_pixel_shader_ = shader;
      break;
    default:
      REX_UNHANDLED_CASE(shader_type);
      return false;
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_IM_LOAD_IMMEDIATE(memory::RingBuffer* reader,
                                                            [[maybe_unused]] u32 packet,
                                                            u32 count) {
  REX_PROFILE_SCOPE("gpu");

  u32 dword0 = reader->ReadAndSwap<u32>();
  u32 dword1 = reader->ReadAndSwap<u32>();
  auto shader_type = static_cast<xenos::ShaderType>(dword0);
  u32 start_size = dword1;
  u32 start = start_size >> 16;
  u32 size_dwords = start_size & 0xFFFF;
  REX_ASSERT(start == 0);
  REX_ASSERT(reader->GetReadCount() >= size_dwords * 4);
  REX_ASSERT(count - 2 >= size_dwords);
  auto shader = LoadShader(shader_type, static_cast<u32>(reader->GetReadPtr()),
                           reinterpret_cast<u32*>(reader->GetBuffer() + reader->GetReadOffset()),
                           size_dwords);
  switch (shader_type) {
    case xenos::ShaderType::kVertex:
      active_vertex_shader_ = shader;
      break;
    case xenos::ShaderType::kPixel:
      active_pixel_shader_ = shader;
      break;
    default:
      REX_UNHANDLED_CASE(shader_type);
      return false;
  }
  reader->AdvanceRead(size_dwords * sizeof(u32));
  return true;
}

bool CommandProcessor::ExecutePacketType3_INVALIDATE_STATE(memory::RingBuffer* reader,
                                                           [[maybe_unused]] u32 packet,
                                                           [[maybe_unused]] u32 count) {
  /*uint32_t mask =*/reader->ReadAndSwap<u32>();
  return true;
}

bool CommandProcessor::ExecutePacketType3_VIZ_QUERY(memory::RingBuffer* reader,
                                                    [[maybe_unused]] u32 packet, u32 count) {
  REX_ASSERT(count == 1);

  u32 dword0 = reader->ReadAndSwap<u32>();

  u32 id = dword0 & 0x3F;
  u32 end = dword0 & 0x100;
  if (!end) {
    WriteRegister(std::to_underlying(xenos::Register::VGT_EVENT_INITIATOR), VIZQUERY_START);
    REXLOG_TRACE(gpu, "Begin viz query ID {:02X}", id);
  } else {
    WriteRegister(std::to_underlying(xenos::Register::VGT_EVENT_INITIATOR), VIZQUERY_END);
    REXLOG_TRACE(gpu, "End viz query ID {:02X}", id);
    if (id < 32) {
      register_file_->values[std::to_underlying(xenos::Register::PA_SC_VIZ_QUERY_STATUS_0)] |=
          static_cast<u32>(1) << id;
    } else {
      register_file_->values[std::to_underlying(xenos::Register::PA_SC_VIZ_QUERY_STATUS_1)] |=
          static_cast<u32>(1) << (id - 32);
    }
  }

  return true;
}

}  // namespace rex::gpu
