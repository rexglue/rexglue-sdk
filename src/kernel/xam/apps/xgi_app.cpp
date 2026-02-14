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

#include <rex/kernel/xam/apps/xgi_app.h>
#include <rex/logging.h>
#include <rex/thread.h>

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;
namespace apps {
using namespace rex::system;

XgiApp::XgiApp(KernelState* kernel_state) : App(kernel_state, 0xFB) {}

// http://mb.mirage.org/bugzilla/xliveless/main.c

X_HRESULT XgiApp::DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                      uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  switch (message) {
    case 0x000B0006: {
      assert_true(!buffer_length || buffer_length == 24);
      // dword r3 user index
      // dword (unwritten?)
      // qword 0
      // dword r4 context enum
      // dword r5 value
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t context_id = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t context_value = memory::load_and_swap<uint32_t>(buffer + 20);
      REXKRNL_DEBUG("XGIUserSetContextEx({:08X}, {:08X}, {:08X})", user_index, context_id,
                    context_value);
      return X_E_SUCCESS;
    }
    case 0x000B0007: {
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t property_id = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t value_size = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t value_ptr = memory::load_and_swap<uint32_t>(buffer + 24);
      REXKRNL_DEBUG("XGIUserSetPropertyEx({:08X}, {:08X}, {}, {:08X})", user_index, property_id,
                    value_size, value_ptr);
      return X_E_SUCCESS;
    }
    case 0x000B0008: {
      assert_true(!buffer_length || buffer_length == 8);
      uint32_t achievement_count = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t achievements_ptr = memory::load_and_swap<uint32_t>(buffer + 4);
      REXKRNL_DEBUG("XGIUserWriteAchievements({:08X}, {:08X})", achievement_count,
                    achievements_ptr);
      return X_E_SUCCESS;
    }
    case 0x000B0010: {
      assert_true(!buffer_length || buffer_length == 28);
      // Sequence:
      // - XamSessionCreateHandle
      // - XamSessionRefObjByHandle
      // - [this]
      // - CloseHandle
      uint32_t session_ptr = memory::load_and_swap<uint32_t>(buffer + 0x0);
      uint32_t flags = memory::load_and_swap<uint32_t>(buffer + 0x4);
      uint32_t num_slots_public = memory::load_and_swap<uint32_t>(buffer + 0x8);
      uint32_t num_slots_private = memory::load_and_swap<uint32_t>(buffer + 0xC);
      uint32_t user_xuid = memory::load_and_swap<uint32_t>(buffer + 0x10);
      uint32_t session_info_ptr = memory::load_and_swap<uint32_t>(buffer + 0x14);
      uint32_t nonce_ptr = memory::load_and_swap<uint32_t>(buffer + 0x18);

      REXKRNL_DEBUG(
          "XGISessionCreateImpl({:08X}, {:08X}, {}, {}, {:08X}, {:08X}, "
          "{:08X})",
          session_ptr, flags, num_slots_public, num_slots_private, user_xuid, session_info_ptr,
          nonce_ptr);
      return X_E_SUCCESS;
    }
    case 0x000B0011: {
      // TODO(PermaNull): reverse buffer contents.
      REXKRNL_DEBUG("XGISessionDelete");
      return X_STATUS_SUCCESS;
    }
    case 0x000B0012: {
      assert_true(buffer_length == 0x14);
      uint32_t session_ptr = memory::load_and_swap<uint32_t>(buffer + 0x0);
      uint32_t user_count = memory::load_and_swap<uint32_t>(buffer + 0x4);
      uint32_t unk_0 = memory::load_and_swap<uint32_t>(buffer + 0x8);
      uint32_t user_index_array = memory::load_and_swap<uint32_t>(buffer + 0xC);
      uint32_t private_slots_array = memory::load_and_swap<uint32_t>(buffer + 0x10);

      assert_zero(unk_0);
      REXKRNL_DEBUG("XGISessionJoinLocal({:08X}, {}, {}, {:08X}, {:08X})", session_ptr, user_count,
                    unk_0, user_index_array, private_slots_array);
      return X_E_SUCCESS;
    }
    case 0x000B0014: {
      // Gets 584107FB in game.
      // get high score table?
      REXKRNL_DEBUG("XGI_unknown");
      return X_STATUS_SUCCESS;
    }
    case 0x000B0015: {
      // send high scores?
      REXKRNL_DEBUG("XGI_unknown");
      return X_STATUS_SUCCESS;
    }
    case 0x000B0041: {
      assert_true(!buffer_length || buffer_length == 32);
      // 00000000 2789fecc 00000000 00000000 200491e0 00000000 200491f0 20049340
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t context_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      auto context = context_ptr ? memory_->TranslateVirtual(context_ptr) : nullptr;
      uint32_t context_id = context ? memory::load_and_swap<uint32_t>(context + 0) : 0;
      REXKRNL_DEBUG("XGIUserGetContext({:08X}, {:08X}{:08X}))", user_index, context_ptr,
                    context_id);
      uint32_t value = 0;
      if (context) {
        memory::store_and_swap<uint32_t>(context + 4, value);
      }
      return X_E_FAIL;
    }
    case 0x000B0071: {
      REXKRNL_DEBUG("XGI 0x000B0071, unimplemented");
      return X_E_SUCCESS;
    }
  }
  REXKRNL_ERROR(
      "Unimplemented XGI message app={:08X}, msg={:08X}, arg1={:08X}, "
      "arg2={:08X}",
      app_id(), message, buffer_ptr, buffer_length);
  return X_E_FAIL;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace rex
