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
#include <cstring>

#include <rex/memory/mapped_ptr.h>
#include <rex/kernel/object/kernel_thread.h>
#include <rex/assert.h>
#include <rex/logging/log.h>
#include "kernel/categories.h"
#include <rex/ppc/hook.h>
#include <rex/types.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/types/status.h>

#include "kernel/apu.h"
#include "kernel/xma/decoder.h"
#include "kernel/title_state.h"
#include "kernel/kernel.h"

namespace rex::kernel::xboxkrnl {
using namespace rex::kernel;

using rex::kernel::XMA_CONTEXT_DATA;

namespace {

kernel::XMADecoder* GetXMADecoder(TitleRuntime& runtime) {
  kernel::APU* apu = StateOf(runtime).GetAPU();
  return apu ? &apu->GetXMADecoder() : nullptr;
}

}  // namespace

u32 XMACreateContext_entry(MappedU32 context_out_ptr) {
  REXKRNL_DEV("XMACreateContext called!");
  auto* xma_decoder = GetXMADecoder(kernel::GetCurrent());
  if (!xma_decoder) {
    return X_STATUS_NO_MEMORY;
  }
  u32 context_ptr = xma_decoder->AllocateContext();
  *context_out_ptr = context_ptr;
  if (!context_ptr) {
    return X_STATUS_NO_MEMORY;
  }
  return X_STATUS_SUCCESS;
}

u32 XMAReleaseContext_entry(MappedVoid context_ptr) {
  auto* xma_decoder = GetXMADecoder(kernel::GetCurrent());
  if (!xma_decoder) {
    return 0;
  }
  xma_decoder->ReleaseContext(context_ptr.GetGuestAddress());
  return 0;
}

void StoreXMAContextIndexedRegister(TitleRuntime& runtime, u32 base_reg, u32 context_ptr) {
  u32 context_physical_address = StateOf(runtime).GetMemory()->GetPhysicalAddress(context_ptr);
  auto* xma_decoder = GetXMADecoder(runtime);
  if (xma_decoder) {
    xma_decoder->SignalContext(base_reg, context_physical_address);
  }
}

struct XMA_LOOP_DATA {
  be_u32 loop_start;
  be_u32 loop_end;
  u8 loop_count;
  u8 loop_subframe_end;
  u8 loop_subframe_skip;
};
REX_STATIC_ASSERT_SIZE(XMA_LOOP_DATA, 12);

struct XMA_CONTEXT_INIT {
  be_u32 input_buffer_0_ptr;
  be_u32 input_buffer_0_packet_count;
  be_u32 input_buffer_1_ptr;
  be_u32 input_buffer_1_packet_count;
  be_u32 input_buffer_read_offset;
  be_u32 output_buffer_ptr;
  be_u32 output_buffer_block_count;
  be_u32 work_buffer_ptr;
  be_u32 subframe_decode_count;
  be_u32 channel_count;
  be_u32 sample_rate;
  XMA_LOOP_DATA loop_data;
};
REX_STATIC_ASSERT_SIZE(XMA_CONTEXT_INIT, 56);

u32 XMAInitializeContext_entry(MappedVoid context_ptr, GuestPtrT<XMA_CONTEXT_INIT> context_init) {
  // Input buffers may be null.
  u32 input_buffer_0_guest_ptr = context_init->input_buffer_0_ptr;
  u32 input_buffer_0_physical_address = 0;
  if (input_buffer_0_guest_ptr) {
    input_buffer_0_physical_address =
        kernel::GetCurrentState().GetMemory()->GetPhysicalAddress(input_buffer_0_guest_ptr);
    REX_ASSERT(input_buffer_0_physical_address != UINT32_MAX);
    if (input_buffer_0_physical_address == UINT32_MAX) {
      REXKRNL_FAIL(X_E_FALSE, "XMAInitializeContext: Invalid input buffer 0 virtual address {:08X}",
                   input_buffer_0_guest_ptr);
    }
  }
  u32 input_buffer_1_guest_ptr = context_init->input_buffer_1_ptr;
  u32 input_buffer_1_physical_address = 0;
  if (input_buffer_1_guest_ptr) {
    input_buffer_1_physical_address =
        kernel::GetCurrentState().GetMemory()->GetPhysicalAddress(input_buffer_1_guest_ptr);
    REX_ASSERT(input_buffer_1_physical_address != UINT32_MAX);
    if (input_buffer_1_physical_address == UINT32_MAX) {
      REXKRNL_FAIL(X_E_FALSE, "XMAInitializeContext: Invalid input buffer 1 virtual address {:08X}",
                   input_buffer_1_guest_ptr);
    }
  }
  u32 output_buffer_guest_ptr = context_init->output_buffer_ptr;
  REX_ASSERT(output_buffer_guest_ptr != 0);
  u32 output_buffer_physical_address =
      kernel::GetCurrentState().GetMemory()->GetPhysicalAddress(output_buffer_guest_ptr);
  REX_ASSERT(output_buffer_physical_address != UINT32_MAX);
  if (output_buffer_physical_address == UINT32_MAX) {
    REXKRNL_FAIL(X_E_FALSE, "XMAInitializeContext: Invalid output buffer virtual address {:08X}",
                 output_buffer_guest_ptr);
  }

  std::memset(context_ptr, 0, sizeof(XMA_CONTEXT_DATA));

  XMA_CONTEXT_DATA context(context_ptr);

  context.input_buffer_0_ptr = input_buffer_0_physical_address;
  context.input_buffer_0_packet_count = context_init->input_buffer_0_packet_count;
  context.input_buffer_1_ptr = input_buffer_1_physical_address;
  context.input_buffer_1_packet_count = context_init->input_buffer_1_packet_count;
  context.input_buffer_read_offset = context_init->input_buffer_read_offset;
  context.output_buffer_ptr = output_buffer_physical_address;
  context.output_buffer_block_count = context_init->output_buffer_block_count;

  context.subframe_decode_count = context_init->subframe_decode_count;
  context.is_stereo = context_init->channel_count >= 1;
  context.sample_rate = context_init->sample_rate;

  context.loop_start = context_init->loop_data.loop_start;
  context.loop_end = context_init->loop_data.loop_end;
  context.loop_count = context_init->loop_data.loop_count;
  context.loop_subframe_end = context_init->loop_data.loop_subframe_end;
  context.loop_subframe_skip = context_init->loop_data.loop_subframe_skip;

  context.Store(context_ptr);

  StoreXMAContextIndexedRegister(kernel::GetCurrent(), 0x1A80, context_ptr.GetGuestAddress());

  return 0;
}

u32 XMASetLoopData_entry(MappedVoid context_ptr, GuestPtrT<XMA_CONTEXT_DATA> loop_data) {
  XMA_CONTEXT_DATA context(context_ptr);

  context.loop_start = loop_data->loop_start;
  context.loop_end = loop_data->loop_end;
  context.loop_count = loop_data->loop_count;
  context.loop_subframe_end = loop_data->loop_subframe_end;
  context.loop_subframe_skip = loop_data->loop_subframe_skip;

  context.Store(context_ptr);

  return 0;
}

u32 XMAGetInputBufferReadOffset_entry(MappedVoid context_ptr) {
  XMA_CONTEXT_DATA context(context_ptr);
  return context.input_buffer_read_offset;
}

u32 XMASetInputBufferReadOffset_entry(MappedVoid context_ptr, u32 value) {
  XMA_CONTEXT_DATA context(context_ptr);
  context.input_buffer_read_offset = value;
  context.Store(context_ptr);

  return 0;
}

u32 XMASetInputBuffer0_entry(MappedVoid context_ptr, MappedVoid buffer, u32 packet_count) {
  u32 buffer_physical_address =
      kernel::GetCurrentState().GetMemory()->GetPhysicalAddress(buffer.GetGuestAddress());
  REX_ASSERT(buffer_physical_address != UINT32_MAX);
  if (buffer_physical_address == UINT32_MAX) {
    REXKRNL_FAIL(X_E_FALSE, "XMASetInputBuffer0: Invalid buffer virtual address {:08X}",
                 buffer.GetGuestAddress());
  }

  XMA_CONTEXT_DATA context(context_ptr);

  context.input_buffer_0_ptr = buffer_physical_address;
  context.input_buffer_0_packet_count = packet_count;

  context.Store(context_ptr);

  return 0;
}

u32 XMAIsInputBuffer0Valid_entry(MappedVoid context_ptr) {
  XMA_CONTEXT_DATA context(context_ptr);
  return context.input_buffer_0_valid;
}

u32 XMASetInputBuffer0Valid_entry(MappedVoid context_ptr) {
  XMA_CONTEXT_DATA context(context_ptr);
  context.input_buffer_0_valid = 1;
  context.Store(context_ptr);

  return 0;
}

u32 XMASetInputBuffer1_entry(MappedVoid context_ptr, MappedVoid buffer, u32 packet_count) {
  u32 buffer_physical_address =
      kernel::GetCurrentState().GetMemory()->GetPhysicalAddress(buffer.GetGuestAddress());
  REX_ASSERT(buffer_physical_address != UINT32_MAX);
  if (buffer_physical_address == UINT32_MAX) {
    REXKRNL_FAIL(X_E_FALSE, "XMASetInputBuffer1: Invalid buffer virtual address {:08X}",
                 buffer.GetGuestAddress());
  }

  XMA_CONTEXT_DATA context(context_ptr);

  context.input_buffer_1_ptr = buffer_physical_address;
  context.input_buffer_1_packet_count = packet_count;

  context.Store(context_ptr);

  return 0;
}

u32 XMAIsInputBuffer1Valid_entry(MappedVoid context_ptr) {
  XMA_CONTEXT_DATA context(context_ptr);
  return context.input_buffer_1_valid;
}

u32 XMASetInputBuffer1Valid_entry(MappedVoid context_ptr) {
  XMA_CONTEXT_DATA context(context_ptr);
  context.input_buffer_1_valid = 1;
  context.Store(context_ptr);

  return 0;
}

u32 XMAIsOutputBufferValid_entry(MappedVoid context_ptr) {
  XMA_CONTEXT_DATA context(context_ptr);
  return context.output_buffer_valid;
}

u32 XMASetOutputBufferValid_entry(MappedVoid context_ptr) {
  XMA_CONTEXT_DATA context(context_ptr);
  context.output_buffer_valid = 1;
  context.Store(context_ptr);

  return 0;
}

u32 XMAGetOutputBufferReadOffset_entry(MappedVoid context_ptr) {
  XMA_CONTEXT_DATA context(context_ptr);
  return context.output_buffer_read_offset;
}

u32 XMASetOutputBufferReadOffset_entry(MappedVoid context_ptr, u32 value) {
  XMA_CONTEXT_DATA context(context_ptr);
  context.output_buffer_read_offset = value;
  context.Store(context_ptr);

  return 0;
}

u32 XMAGetOutputBufferWriteOffset_entry(MappedVoid context_ptr) {
  XMA_CONTEXT_DATA context(context_ptr);
  return context.output_buffer_write_offset;
}

u32 XMAGetPacketMetadata_entry(MappedVoid context_ptr) {
  XMA_CONTEXT_DATA context(context_ptr);
  return context.packet_metadata;
}

u32 XMAEnableContext_entry(MappedVoid context_ptr) {
  StoreXMAContextIndexedRegister(kernel::GetCurrent(), 0x1940, context_ptr.GetGuestAddress());
  return 0;
}

u32 XMADisableContext_entry(MappedVoid context_ptr, u32 wait) {
  X_HRESULT result = X_E_SUCCESS;
  StoreXMAContextIndexedRegister(kernel::GetCurrent(), 0x1A40, context_ptr.GetGuestAddress());
  auto* xma_decoder = GetXMADecoder(kernel::GetCurrent());
  if (xma_decoder && !xma_decoder->BlockOnContext(context_ptr.GetGuestAddress(), !wait)) {
    result = X_E_FALSE;
  }
  return result;
}

u32 XMABlockWhileInUse_entry(MappedVoid context_ptr) {
  do {
    XMA_CONTEXT_DATA context(context_ptr);
    if (!context.input_buffer_0_valid && !context.input_buffer_1_valid) {
      break;
    }
    if (!context.work_buffer_ptr) {
      break;
    }
    rex::kernel::KernelThread::SleepFor(std::chrono::milliseconds(1));
  } while (true);
  return 0;
}

}  // namespace rex::kernel::xboxkrnl

REX_EXPORT(__imp__XMACreateContext, rex::kernel::xboxkrnl::XMACreateContext_entry)
REX_EXPORT(__imp__XMAReleaseContext, rex::kernel::xboxkrnl::XMAReleaseContext_entry)
REX_EXPORT(__imp__XMAInitializeContext, rex::kernel::xboxkrnl::XMAInitializeContext_entry)
REX_EXPORT(__imp__XMASetLoopData, rex::kernel::xboxkrnl::XMASetLoopData_entry)
REX_EXPORT(__imp__XMAGetInputBufferReadOffset,
           rex::kernel::xboxkrnl::XMAGetInputBufferReadOffset_entry)
REX_EXPORT(__imp__XMASetInputBufferReadOffset,
           rex::kernel::xboxkrnl::XMASetInputBufferReadOffset_entry)
REX_EXPORT(__imp__XMASetInputBuffer0, rex::kernel::xboxkrnl::XMASetInputBuffer0_entry)
REX_EXPORT(__imp__XMAIsInputBuffer0Valid, rex::kernel::xboxkrnl::XMAIsInputBuffer0Valid_entry)
REX_EXPORT(__imp__XMASetInputBuffer0Valid, rex::kernel::xboxkrnl::XMASetInputBuffer0Valid_entry)
REX_EXPORT(__imp__XMASetInputBuffer1, rex::kernel::xboxkrnl::XMASetInputBuffer1_entry)
REX_EXPORT(__imp__XMAIsInputBuffer1Valid, rex::kernel::xboxkrnl::XMAIsInputBuffer1Valid_entry)
REX_EXPORT(__imp__XMASetInputBuffer1Valid, rex::kernel::xboxkrnl::XMASetInputBuffer1Valid_entry)
REX_EXPORT(__imp__XMAIsOutputBufferValid, rex::kernel::xboxkrnl::XMAIsOutputBufferValid_entry)
REX_EXPORT(__imp__XMASetOutputBufferValid, rex::kernel::xboxkrnl::XMASetOutputBufferValid_entry)
REX_EXPORT(__imp__XMAGetOutputBufferReadOffset,
           rex::kernel::xboxkrnl::XMAGetOutputBufferReadOffset_entry)
REX_EXPORT(__imp__XMASetOutputBufferReadOffset,
           rex::kernel::xboxkrnl::XMASetOutputBufferReadOffset_entry)
REX_EXPORT(__imp__XMAGetOutputBufferWriteOffset,
           rex::kernel::xboxkrnl::XMAGetOutputBufferWriteOffset_entry)
REX_EXPORT(__imp__XMAGetPacketMetadata, rex::kernel::xboxkrnl::XMAGetPacketMetadata_entry)
REX_EXPORT(__imp__XMAEnableContext, rex::kernel::xboxkrnl::XMAEnableContext_entry)
REX_EXPORT(__imp__XMADisableContext, rex::kernel::xboxkrnl::XMADisableContext_entry)
REX_EXPORT(__imp__XMABlockWhileInUse, rex::kernel::xboxkrnl::XMABlockWhileInUse_entry)
