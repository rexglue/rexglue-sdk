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

#include "runtime/xma/context.h"
#include "runtime/xma/decoder.h"
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <rex/math.h>
#include <rex/mmio.h>
#include <rex/memory/ring_buffer.h>
#include <rex/string/buffer.h>
#include <rex/runtime/function_dispatcher.h>
#include "runtime/cvar_table.h"

extern "C" {
#include "libavutil/log.h"
}  // extern "C"

// As with normal Microsoft, there are like twelve different ways to access
// the audio APIs. Early games use XMA*() methods almost exclusively to touch
// decoders. Later games use XAudio*() and direct memory writes to the XMA
// structures (as opposed to the XMA* calls), meaning that we have to support
// both.
//
// The XMA*() functions manipulate the audio system in the guest context
// and let the normal XMADecoder handling take it, to prevent duplicate
// implementations. They can be found in xboxkrnl_audio_xma.cc
//
// XMA details:
// https://devel.nuclex.org/external/svn/directx/trunk/include/xma2defs.h
// https://github.com/gdawg/fsbext/blob/master/src/xma_header.h
//
// XAudio2 uses XMA under the covers, and seems to map with the same
// restrictions of frame and subframe:
// https://msdn.microsoft.com/en-us/library/windows/desktop/microsoft.directx_sdk.xaudio2.xaudio2_buffer(v=vs.85).aspx
//
// XMA contexts are 64b in size and tight bitfields. They are in physical
// memory not usually available to games. Games will use MmMapIoSpace to get
// the 64b pointer in user memory so they can party on it. If the game doesn't
// do this, it's likely they are either passing the context to XAudio or
// using the XMA* functions.

namespace rex::runtime {

XMADecoder::XMADecoder(runtime::FunctionDispatcher* function_dispatcher)
    : memory_(function_dispatcher->memory()), function_dispatcher_(function_dispatcher) {}

XMADecoder::~XMADecoder() {
  Shutdown();
}

void av_log_callback(void* avcl, int level, const char* fmt, va_list va) {
  if (!REXCVAR_GET(::rex::runtime::cvars::ffmpeg_verbose) && level > AV_LOG_WARNING) {
    return;
  }

  StringBuffer buff;
  buff.AppendVarargs(fmt, va);
  auto msg = buff.to_string_view();

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
  // Setup ffmpeg logging callback
  av_log_set_callback(av_log_callback);

  rex::AttachMMIO(rex::MMIOBlock::APU, {this, MMIOReadRegisterThunk, MMIOWriteRegisterThunk});

  // Setup XMA context data.
  // The console kernel allocates the contexts with X_PAGE_NOCACHE |
  // X_PAGE_READWRITE and writes MmGetPhysicalAddress for the address to the
  // register.
  context_data_first_ptr_ = memory()->SystemHeapAlloc(sizeof(XMA_CONTEXT_DATA) * kContextCount, 256,
                                                      runtime::kSystemHeapPhysical);
  context_data_last_ptr_ = context_data_first_ptr_ + (sizeof(XMA_CONTEXT_DATA) * kContextCount - 1);
  register_file_[XMARegister::ContextArrayAddress] =
      memory()->GetPhysicalAddress(context_data_first_ptr_);

  // Setup XMA contexts.
  for (size_t i = 0; i < kContextCount; ++i) {
    uint32_t guest_ptr = context_data_first_ptr_ + i * sizeof(XMA_CONTEXT_DATA);
    XMAContext& context = contexts_[i];
    if (context.Setup(i, memory(), guest_ptr)) {
      REX_ASSERT_ALWAYS();
    }
  }
  register_file_[XMARegister::NextContextIndex] = 1;
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
    // Okay, let's loop through XMA contexts to find ones we need to decode!
    bool did_work = false;
    for (uint32_t n = 0; n < kContextCount && !stop.stop_requested(); n++) {
      XMAContext& context = contexts_[n];
      bool worked = context.Work();
      if (worked) {
        context.SignalWorkDone();
        REX_PERF_INC(kXMAFramesDecoded);
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
    // No work done this iteration, block until signaled.
    rex::platform::Wait(work_event_.get(), false);
  }
}

void XMADecoder::Shutdown() {
  rex::DetachMMIO(rex::MMIOBlock::APU, this);
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
    memory()->SystemHeapFree(context_data_first_ptr_);
  }

  context_data_first_ptr_ = 0;
  context_data_last_ptr_ = 0;
}

int XMADecoder::GetContextId(uint32_t guest_ptr) const {
  REX_STATIC_ASSERT_SIZE(XMA_CONTEXT_DATA, 64);
  if (guest_ptr < context_data_first_ptr_ || guest_ptr > context_data_last_ptr_) {
    return -1;
  }
  REX_ASSERT((guest_ptr & 0x3F) == 0);
  return (guest_ptr - context_data_first_ptr_) >> 6;
}

uint32_t XMADecoder::AllocateContext() {
  size_t index = context_bitmap_.Acquire();
  if (index == -1) {
    // Out of contexts.
    return 0;
  }

  XMAContext& context = contexts_[index];
  REX_ASSERT(!context.is_allocated());
  context.set_is_allocated(true);
  return context.guest_ptr();
}

void XMADecoder::ReleaseContext(uint32_t guest_ptr) {
  auto context_id = GetContextId(guest_ptr);
  REX_ASSERT(context_id >= 0);

  XMAContext& context = contexts_[context_id];
  REX_ASSERT(context.is_allocated());
  context.Release();
  context_bitmap_.Release(context_id);
}

bool XMADecoder::BlockOnContext(uint32_t guest_ptr, bool poll) {
  auto context_id = GetContextId(guest_ptr);
  REX_ASSERT(context_id >= 0);

  XMAContext& context = contexts_[context_id];
  return context.Block(poll);
}

uint32_t XMADecoder::ReadRegister(uint32_t addr) {
  auto r = (addr & 0xFFFF) / 4;

  REX_ASSERT(r < XMARegisterFile::kRegisterCount);

  switch (r) {
    case XMARegister::ContextArrayAddress:
      break;
    case XMARegister::CurrentContextIndex: {
      // 0606h (1818h) is rotating context processing # set to hardware ID of
      // context being processed.
      // If bit 200h is set, the locking code will possibly collide on hardware
      // IDs and error out, so we should never set it (I think?).
      uint32_t& current_context_index = register_file_[XMARegister::CurrentContextIndex];
      uint32_t& next_context_index = register_file_[XMARegister::NextContextIndex];
      // To prevent games from seeing a stuck XMA context, return a rotating
      // number.
      current_context_index = next_context_index;
      next_context_index = (next_context_index + 1) % kContextCount;
      break;
    }
    default:
      const auto register_info = register_file_.GetRegisterInfo(r);
      if (register_info) {
        REXLOG_DEBUG(apu, "XMA: Read from unhandled register ({:04X}, {})", r, register_info->name);
      } else {
        REXLOG_DEBUG(apu, "XMA: Read from unknown register ({:04X})", r);
      }
      break;
  }

  return std::byteswap(register_file_[r]);
}

void XMADecoder::WriteRegister(uint32_t addr, uint32_t value) {
  REX_PROFILE_SCOPE("apu");

  uint32_t r = (addr & 0xFFFF) / 4;
  value = std::byteswap(value);

  REX_ASSERT(r < XMARegisterFile::kRegisterCount);
  register_file_[r] = value;

  if (r >= XMARegister::Context0Kick && r <= XMARegister::Context9Kick) {
    // Context kick command.
    // This will kick off the given hardware contexts.
    // This kicks the SPU and says "hey, decode that audio!"
    // XMAEnableContext

    // The context ID is a bit in the range of the entire context array.
    uint32_t base_context_id = (r - XMARegister::Context0Kick) * 32;
    uint32_t kicked_value = value;
    for (int i = 0; value && i < 32; ++i, value >>= 1) {
      if (value & 1) {
        uint32_t context_id = base_context_id + i;
        auto& context = contexts_[context_id];
        context.Enable();
      }
    }
    // Decode inline. waiting on the worker sweep stalls the realtime
    // audio thread 50-100ms during kick bursts.
    for (int i = 0; kicked_value && i < 32; ++i, kicked_value >>= 1) {
      if (kicked_value & 1) {
        uint32_t context_id = base_context_id + i;
        auto& context = contexts_[context_id];
        if (context.Work()) {
          context.SignalWorkDone();
        }
      }
    }
    work_event_->Set();
  } else if (r >= XMARegister::Context0Lock && r <= XMARegister::Context9Lock) {
    // Context lock command.
    // This requests a lock by flagging the context.
    // XMADisableContext
    uint32_t base_context_id = (r - XMARegister::Context0Lock) * 32;
    for (int i = 0; value && i < 32; ++i, value >>= 1) {
      if (value & 1) {
        uint32_t context_id = base_context_id + i;
        auto& context = contexts_[context_id];
        context.Disable();
        // [XMA fix] Added Block(false) after Disable(). Without this, the game
        // could call XMADisableContext and start modifying the context struct
        // while a decode was still in progress on the worker thread. Block()
        // waits for the context mutex to be free (poll=false means wait, not spin).
        context.Block(false);
      }
    }
    // Signal the decoder thread to start processing.
    // work_event_->Set();
  } else if (r >= XMARegister::Context0Clear && r <= XMARegister::Context9Clear) {
    // Context clear command.
    // This will reset the given hardware contexts.
    uint32_t base_context_id = (r - XMARegister::Context0Clear) * 32;
    for (int i = 0; value && i < 32; ++i, value >>= 1) {
      if (value & 1) {
        uint32_t context_id = base_context_id + i;
        XMAContext& context = contexts_[context_id];
        context.Clear();
      }
    }
  } else {
    // 0601h (1804h) is written to with 0x02000000 and 0x03000000 around a lock
    // operation
    switch (r) {
      default: {
        const auto register_info = register_file_.GetRegisterInfo(r);
        if (register_info) {
          REXLOG_DEBUG(apu, "XMA: Write to unhandled register ({:04X}, {}): {:08X}", r,
                       register_info->name, value);
        } else {
          REXLOG_DEBUG(apu, "XMA: Write to unknown register ({:04X}): {:08X}", r, value);
        }
        break;
      }
#pragma warning(suppress : 4065)
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

}  // namespace rex::runtime
