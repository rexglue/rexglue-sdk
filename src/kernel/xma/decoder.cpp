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

#include "kernel/xma/context.h"
#include "kernel/xma/decoder.h"
#include <rex/platform/perf.h>
#include <rex/logging/log.h>
#include <rex/apu/audio_driver.h>
#include <rex/math.h>
#include <rex/ppc/mmio.h>
#include <rex/memory/stream.h>
#include <rex/string/text.h>
#include <rex/kernel/function_dispatcher.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/runtime_settings.h>
#include <rex/platform/sync.h>
#include "kernel/settings_access.h"

extern "C" {
#include "libavutil/log.h"
}  // extern "C"

namespace rex::kernel {

XMADecoder::XMADecoder(kernel::FunctionDispatcher* function_dispatcher)
    : memory_(function_dispatcher->GetMemory()), function_dispatcher_(function_dispatcher) {}

XMADecoder::~XMADecoder() {
  Shutdown();
}

void AVLogCallback([[maybe_unused]] void* avcl, int level, const char* fmt, va_list va) {
  if (!GetRuntimeSettings().ffmpeg_verbose.Get() && level > AV_LOG_WARNING) {
    return;
  }

  string::StringBuffer buff;
  buff.AppendVarargs(fmt, va);
  auto msg = buff.ToStringView();

  switch (level) {
    case AV_LOG_ERROR:
      REXLOG_ERROR(apu, "ffmpeg: {}", msg);
      break;
    case AV_LOG_WARNING:
      REXLOG_WARN(apu, "ffmpeg: {}", msg);
      break;
    case AV_LOG_INFO:
      REXLOG_INFO(apu, "ffmpeg: {}", msg);
      break;
    case AV_LOG_VERBOSE:
    case AV_LOG_DEBUG:
    default:
      REXLOG_DEBUG(apu, "ffmpeg: {}", msg);
      break;
  }
}

X_STATUS XMADecoder::Setup() {
  av_log_set_callback(AVLogCallback);

  rex::ppc::AttachMMIO(rex::ppc::MMIOBlock::kAPU,
                       {this, MMIOReadRegisterThunk, MMIOWriteRegisterThunk});

  context_data_first_ptr_ = GetMemory()->SystemHeapAlloc(sizeof(XMA_CONTEXT_DATA) * kContextCount,
                                                         256, kernel::kSystemHeapPhysical);
  context_data_last_ptr_ = context_data_first_ptr_ + (sizeof(XMA_CONTEXT_DATA) * kContextCount - 1);
  register_file_[XMARegister::kContextArrayAddress] =
      GetMemory()->GetPhysicalAddress(context_data_first_ptr_);

  for (size_t i = 0; i < kContextCount; ++i) {
    u32 context_ptr = context_data_first_ptr_ + i * sizeof(XMA_CONTEXT_DATA);
    XMAContext& context = contexts_[i];
    if (context.Setup(i, GetMemory(), context_ptr)) {
      REX_ASSERT_ALWAYS();
    }
  }
  register_file_[XMARegister::kNextContextIndex] = 1;
  context_bitmap_.Resize(kContextCount);

  work_event_ = rex::platform::Event::CreateAutoResetEvent(false);
  REX_ASSERT(work_event_ != nullptr);
  worker_exited_ = rex::platform::Event::CreateManualResetEvent(false);
  worker_ = rex::platform::Spawn("XMA Decoder", [this](const std::stop_token& stop) {
    WorkerThreadMain(stop);
    worker_exited_->Set();
  });

  return X_STATUS_SUCCESS;
}

void XMADecoder::WorkerThreadMain(const std::stop_token& stop) {
  while (!stop.stop_requested()) {
    bool did_work = false;
    for (u32 n = 0; n < kContextCount && !stop.stop_requested(); n++) {
      XMAContext& context = contexts_[n];
      bool worked = context.Work();
      if (worked) {
        context.SignalWorkDone();
        REX_PERF_INC("xma_frames_decoded");
      }
      did_work = did_work || worked;
    }

    if (paused_) {
      pause_fence_.Signal();
      resume_fence_.Wait();
    }

    if (did_work) {
      continue;
    }
    rex::platform::Wait(work_event_.get(), false);
  }
}

void XMADecoder::Shutdown() {
  rex::ppc::DetachMMIO(rex::ppc::MMIOBlock::kAPU, this);
  if (!worker_.joinable()) {
    return;
  }

  worker_.request_stop();
  work_event_->Set();
  if (paused_) {
    Resume();
  }

  if (rex::platform::Wait(worker_exited_.get(), false, std::chrono::milliseconds(2000)) ==
      rex::platform::WaitResult::kTimeout) {
    REXLOG_WARN(apu, "XMA: Worker thread did not exit within 2s, waiting for it");
  }
  worker_.join();

  if (context_data_first_ptr_) {
    GetMemory()->SystemHeapFree(context_data_first_ptr_);
  }

  context_data_first_ptr_ = 0;
  context_data_last_ptr_ = 0;
}

int XMADecoder::GetContextID(u32 context_ptr) const {
  REX_STATIC_ASSERT_SIZE(XMA_CONTEXT_DATA, 64);
  if (context_ptr < context_data_first_ptr_ || context_ptr > context_data_last_ptr_) {
    return -1;
  }
  REX_ASSERT((context_ptr & 0x3F) == 0);
  return static_cast<int>((context_ptr - context_data_first_ptr_) >> 6);
}

u32 XMADecoder::AllocateContext() {
  size_t index = context_bitmap_.AcquireEntry();
  if (index == static_cast<size_t>(-1)) {
    return 0;
  }

  XMAContext& context = contexts_[index];
  REX_ASSERT(!context.IsAllocated());
  context.SetAllocated(true);
  return context.GetGuestPtr();
}

void XMADecoder::ReleaseContext(u32 context_ptr) {
  auto context_id = GetContextID(context_ptr);
  if (context_id < 0) {
    return;
  }

  XMAContext& context = contexts_[context_id];
  if (!context.IsAllocated()) {
    return;
  }
  context.Release();
  context_bitmap_.ReleaseEntry(context_id);
}

bool XMADecoder::BlockOnContext(u32 context_ptr, bool poll) {
  auto context_id = GetContextID(context_ptr);
  if (context_id < 0) {
    return true;
  }

  XMAContext& context = contexts_[context_id];
  return context.WaitIdle(poll);
}

bool XMADecoder::SignalContext(u32 base_register, u32 context_physical_address) {
  const u32 array_base = GetContextArrayAddress();
  if (context_physical_address < array_base) {
    return false;
  }
  const u32 hw_index = (context_physical_address - array_base) / sizeof(XMA_CONTEXT_DATA);
  if (hw_index >= kContextCount) {
    return false;
  }
  WriteRegister(base_register + (hw_index >> 5) * 4, std::byteswap(1u << (hw_index & 0x1F)));
  return true;
}

u32 XMADecoder::ReadRegister(u32 addr) {
  auto r = (addr & 0xFFFF) / 4;

  REX_ASSERT(r < XMARegisterFile::kRegisterCount);

  switch (r) {
    case XMARegister::kContextArrayAddress:
      break;
    case XMARegister::kCurrentContextIndex: {
      u32& current_context_index = register_file_[XMARegister::kCurrentContextIndex];
      u32& next_context_index = register_file_[XMARegister::kNextContextIndex];
      current_context_index = next_context_index;
      next_context_index = (next_context_index + 1) % kContextCount;
      break;
    }
    default:
      const auto register_info = register_file_.GetRegisterInfo(r);
      if (register_info) {
        REXLOG_WARN_ONCE(apu, unhandled_read_once_, r,
                         "XMA: Read from unhandled register ({:04X}, {})", r, register_info->name);
      } else {
        REXLOG_WARN_ONCE(apu, unhandled_read_once_, r, "XMA: Read from unknown register ({:04X})",
                         r);
      }
      break;
  }

  return std::byteswap(register_file_[r]);
}

void XMADecoder::WriteRegister(u32 addr, u32 value) {
  REX_PROFILE_SCOPE("apu");

  u32 r = (addr & 0xFFFF) / 4;
  value = std::byteswap(value);

  REX_ASSERT(r < XMARegisterFile::kRegisterCount);
  register_file_[r] = value;

  if (r >= XMARegister::Context0Kick && r <= XMARegister::Context9Kick) {
    // XMAEnableContext writes the kick registers.
    u32 base_context_id = (r - XMARegister::Context0Kick) * 32;
    u32 kicked_value = value;
    for (int i = 0; value && i < 32; ++i, value >>= 1) {
      if (value & 1) {
        u32 context_id = base_context_id + i;
        auto& context = contexts_[context_id];
        context.Enable();
      }
    }
    // Decode inline, a worker sweep would stall the audio thread 50 to 100ms in kick bursts.
    for (int i = 0; kicked_value && i < 32; ++i, kicked_value >>= 1) {
      if (kicked_value & 1) {
        u32 context_id = base_context_id + i;
        auto& context = contexts_[context_id];
        if (context.Work()) {
          context.SignalWorkDone();
        }
      }
    }
    work_event_->Set();
  } else if (r >= XMARegister::Context0Lock && r <= XMARegister::Context9Lock) {
    // XMADisableContext writes the lock registers.
    u32 base_context_id = (r - XMARegister::Context0Lock) * 32;
    for (int i = 0; value && i < 32; ++i, value >>= 1) {
      if (value & 1) {
        u32 context_id = base_context_id + i;
        auto& context = contexts_[context_id];
        context.Disable();
        // Block until an in flight decode finishes before the guest may modify the context.
        context.WaitIdle(false);
      }
    }
  } else if (r >= XMARegister::Context0Clear && r <= XMARegister::Context9Clear) {
    u32 base_context_id = (r - XMARegister::Context0Clear) * 32;
    for (int i = 0; value && i < 32; ++i, value >>= 1) {
      if (value & 1) {
        u32 context_id = base_context_id + i;
        XMAContext& context = contexts_[context_id];
        context.Clear();
      }
    }
  } else {
    // Register 0x0601 receives 0x02000000 and 0x03000000 around a lock operation.
    const auto register_info = register_file_.GetRegisterInfo(r);
    if (register_info) {
      REXLOG_WARN_ONCE(apu, unhandled_write_once_, r,
                       "XMA: Write to unhandled register ({:04X}, {}) value {:08X}", r,
                       register_info->name, value);
    } else {
      REXLOG_WARN_ONCE(apu, unhandled_write_once_, r,
                       "XMA: Write to unknown register ({:04X}) value {:08X}", r, value);
    }
  }
}

void XMADecoder::Pause() {
  if (paused_) {
    return;
  }
  paused_ = true;

  if (work_event_) {
    work_event_->Set();
  }
  pause_fence_.Wait();
}

void XMADecoder::Resume() {
  if (!paused_) {
    return;
  }
  paused_ = false;

  resume_fence_.Signal();
}

}  // namespace rex::kernel
