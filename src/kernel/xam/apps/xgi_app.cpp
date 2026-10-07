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
#include <rex/platform/thread.h>
#include "kernel/kernel_macros.h"

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::runtime;
using namespace rex::runtime::xam;
namespace apps {
using namespace rex::runtime;

XgiApp::XgiApp(Runtime& runtime) : App(runtime, 0xFB) {}

// http://mb.mirage.org/bugzilla/xliveless/main.c

X_HRESULT XgiApp::DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                      uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
  switch (message) {
    case 0x000B0006: {
      REX_ASSERT(!buffer_length || buffer_length == 24);
      // dword r3 user index
      // dword (unwritten?)
      // qword 0
      // dword r4 context enum
      // dword r5 value
      struct {
        be_u32 user_index;
        u8 pad_04[12];
        be_u32 context_id;
        be_u32 context_value;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 24);
      uint32_t user_index = args->user_index;
      uint32_t context_id = args->context_id;
      uint32_t context_value = args->context_value;
      REXLOG_DEBUG(kernel, "XGIUserSetContextEx({:08X}, {:08X}, {:08X})", user_index, context_id,
                   context_value);
      return X_E_SUCCESS;
    }
    case 0x000B0007: {
      struct {
        be_u32 user_index;
        u8 pad_04[12];
        be_u32 property_id;
        be_u32 value_size;
        be_u32 value_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 28);
      uint32_t user_index = args->user_index;
      uint32_t property_id = args->property_id;
      uint32_t value_size = args->value_size;
      uint32_t value_ptr = args->value_ptr;
      REXLOG_DEBUG(kernel, "XGIUserSetPropertyEx({:08X}, {:08X}, {}, {:08X})", user_index,
                   property_id, value_size, value_ptr);
      return X_E_SUCCESS;
    }
    case 0x000B0008: {
      // Raw dump so we can confirm the actual buffer layout the game sends.
      struct {
        be_u32 count;
        be_u32 achievements_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 8);
      uint32_t raw0 = buffer_length >= 4 ? u32{args->count} : 0;
      uint32_t raw4 = buffer_length >= 8 ? u32{args->achievements_ptr} : 0;
      REXLOG_DEBUG(kernel,
                   "XGIUserWriteAchievements called: buf_len={} raw[0]={:08X} raw[4]={:08X}",
                   buffer_length, raw0, raw4);

      REX_ASSERT(!buffer_length || buffer_length == 8);
      uint32_t achievement_count = raw0;
      uint32_t achievements_ptr = raw4;

      // Empirically confirmed from log: each entry is {u32 padding/user_index, u32 id, ...}.
      // The achievement ID sits at offset 4, not 0. Stride 8 covers the observed fields.
      struct Entry {
        be_u32 user_index;
        be_u32 id;
      };
      REX_STATIC_ASSERT_SIZE(Entry, 8);
      static_assert(offsetof(Entry, id) == 4);
      constexpr uint32_t kMaxAchievements = 1000;

      if (achievements_ptr && achievement_count > 0) {
        if (achievement_count > kMaxAchievements) {
          REXLOG_WARN(kernel, "XGIUserWriteAchievements: count={} unreasonable, ignoring",
                      achievement_count);
          return X_E_FAIL;
        }
        uint32_t span_end = achievements_ptr + achievement_count * sizeof(Entry) - 1;
        if (!memory_->LookupHeap(achievements_ptr) || !memory_->LookupHeap(span_end)) {
          REXLOG_WARN(kernel, "XGIUserWriteAchievements: ptr {:08X} OOB", achievements_ptr);
          return X_E_FAIL;
        }
        const auto* entries = memory_->TranslateVirtual<const Entry*>(achievements_ptr);
        for (uint32_t i = 0; i < achievement_count; ++i) {
          uint32_t id = entries[i].id;
          REXLOG_DEBUG(kernel, "XGIUserWriteAchievements: id={} ({})", id, i);
          (void)runtime_.achievement_manager().UnlockAchievement(id,
                                                                 AchievementNotification::kShow);
        }
      } else {
        REXLOG_DEBUG(kernel, "XGIUserWriteAchievements: skipped (count={} ptr={:08X})",
                     achievement_count, achievements_ptr);
      }
      return X_E_SUCCESS;
    }
    case 0x000B0010: {
      REX_ASSERT(!buffer_length || buffer_length == 28);
      // Sequence:
      // - XamSessionCreateHandle
      // - XamSessionRefObjByHandle
      // - [this]
      // - CloseHandle
      struct {
        be_u32 session_ptr;
        be_u32 flags;
        be_u32 num_slots_public;
        be_u32 num_slots_private;
        be_u32 user_xuid;
        be_u32 session_info_ptr;
        be_u32 nonce_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 28);
      uint32_t session_ptr = args->session_ptr;
      uint32_t flags = args->flags;
      uint32_t num_slots_public = args->num_slots_public;
      uint32_t num_slots_private = args->num_slots_private;
      uint32_t user_xuid = args->user_xuid;
      uint32_t session_info_ptr = args->session_info_ptr;
      uint32_t nonce_ptr = args->nonce_ptr;

      REXLOG_DEBUG(kernel,
                   "XGISessionCreateImpl({:08X}, {:08X}, {}, {}, {:08X}, {:08X}, "
                   "{:08X})",
                   session_ptr, flags, num_slots_public, num_slots_private, user_xuid,
                   session_info_ptr, nonce_ptr);
      return X_E_SUCCESS;
    }
    case 0x000B0011: {
      REX_ASSERT(!buffer_length || buffer_length == 16);

#pragma pack(push, 4)
      struct {
        be_u32 obj_ptr;
        be_u32 flags;
        be_u64 session_nonce;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
#pragma pack(pop)
      REX_STATIC_ASSERT_SIZE(decltype(*args), 16);
      uint32_t obj_ptr = args->obj_ptr;
      uint32_t flags = args->flags;
      uint64_t session_nonce = args->session_nonce;

      REXLOG_DEBUG(kernel, "XGISessionDelete({:08X}, {:08X}, {:016X})", obj_ptr, flags,
                   session_nonce);

      return X_E_SUCCESS;
    }
    case 0x000B0012: {
      REX_ASSERT(!buffer_length || buffer_length == 20);
      struct {
        be_u32 session_ptr;
        be_u32 user_count;
        be_u32 xuid_array_ptr;
        be_u32 user_index_array;
        be_u32 private_slots_array;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 20);
      uint32_t session_ptr = args->session_ptr;
      uint32_t user_count = args->user_count;
      uint32_t xuid_array_ptr = args->xuid_array_ptr;
      uint32_t user_index_array = args->user_index_array;
      uint32_t private_slots_array = args->private_slots_array;
      bool is_local = xuid_array_ptr == 0;

      REXLOG_DEBUG(kernel, "{}({:08X}, {}, {}, {:08X}, {:08X})",
                   is_local ? "XGISessionJoinLocal" : "XGISessionJoinRemote", session_ptr,
                   user_count, xuid_array_ptr, user_index_array, private_slots_array);
      return X_E_SUCCESS;
    }
    case 0x000B0014: {
      REX_ASSERT(!buffer_length || buffer_length == 16);

#pragma pack(push, 4)
      struct {
        be_u32 obj_ptr;
        be_u32 flags;
        be_u64 session_nonce;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
#pragma pack(pop)
      REX_STATIC_ASSERT_SIZE(decltype(*args), 16);
      uint32_t obj_ptr = args->obj_ptr;
      uint32_t flags = args->flags;
      uint64_t session_nonce = args->session_nonce;

      REXLOG_DEBUG(kernel, "XSessionStart({:08X}, {:08X}, {:016X})", obj_ptr, flags, session_nonce);

      return X_STATUS_SUCCESS;
    }
    case 0x000B0015: {
      // send high scores?
      REX_ASSERT(!buffer_length || buffer_length == 16);

#pragma pack(push, 4)
      struct {
        be_u32 obj_ptr;
        be_u32 flags;
        be_u64 session_nonce;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
#pragma pack(pop)
      REX_STATIC_ASSERT_SIZE(decltype(*args), 16);
      uint32_t obj_ptr = args->obj_ptr;
      uint32_t flags = args->flags;
      uint64_t session_nonce = args->session_nonce;

      REXLOG_DEBUG(kernel, "XSessionEnd({:08X}, {:08X}, {:016X})", obj_ptr, flags, session_nonce);

      return X_E_SUCCESS;
    }
    case 0x000B0016: {
      REX_ASSERT(!buffer_length || buffer_length == 32);

      struct {
        be_u32 proc_index;
        be_u32 user_index;
        be_u32 num_results;
        be_u16 num_props;
        be_u16 num_ctx;
        be_u32 props_ptr;
        be_u32 ctx_ptr;
        be_u32 results_buffer_size;
        be_u32 search_results_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 32);
      uint32_t proc_index = args->proc_index;
      uint32_t user_index = args->user_index;
      uint32_t num_results = args->num_results;
      uint16_t num_props = args->num_props;
      uint16_t num_ctx = args->num_ctx;
      uint32_t props_ptr = args->props_ptr;
      uint32_t ctx_ptr = args->ctx_ptr;
      uint32_t results_buffer_size = args->results_buffer_size;
      uint32_t search_results_ptr = args->search_results_ptr;

      REXLOG_DEBUG(kernel, "XSessionSearch({}, {}, {}, {}, {}, {:08X}, {:08X}, {}, {:08X})",
                   proc_index, user_index, num_results, num_props, num_ctx, props_ptr, ctx_ptr,
                   results_buffer_size, search_results_ptr);
      return X_E_SUCCESS;
    }
    case 0x000B0018: {
      REX_ASSERT(!buffer_length || buffer_length == 16);

      struct {
        be_u32 obj_ptr;
        be_u32 flags;
        be_u32 maxPublicSlots;
        be_u16 maxPrivateSlots;
        u8 pad_0E[2];
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 16);
      uint32_t obj_ptr = args->obj_ptr;
      uint32_t flags = args->flags;
      uint32_t maxPublicSlots = args->maxPublicSlots;
      uint16_t maxPrivateSlots = args->maxPrivateSlots;

      REXLOG_DEBUG(kernel, "XSessionModify({:08X}, {:08X}, {:08X}, {:08X})", obj_ptr, flags,
                   maxPublicSlots, maxPrivateSlots);

      return X_E_SUCCESS;
    }
    case 0x000B001C: {
      REX_ASSERT(!buffer_length || buffer_length == 36);

      // session_search
      struct {
        be_u32 proc_index;
        be_u32 user_index;
        be_u32 num_results;
        be_u16 num_props;
        be_u16 num_ctx;
        be_u32 props_ptr;
        be_u32 ctx_ptr;
        be_u32 results_buffer_size;
        be_u32 search_results_ptr;
        be_u32 num_users;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 36);
      uint32_t proc_index = args->proc_index;
      uint32_t user_index = args->user_index;
      uint32_t num_results = args->num_results;
      uint16_t num_props = args->num_props;
      uint16_t num_ctx = args->num_ctx;
      uint32_t props_ptr = args->props_ptr;
      uint32_t ctx_ptr = args->ctx_ptr;
      uint32_t results_buffer_size = args->results_buffer_size;
      uint32_t search_results_ptr = args->search_results_ptr;
      //
      uint32_t num_users = args->num_users;

      REXLOG_DEBUG(kernel, "XSessionSearchEx({}, {}, {}, {}, {}, {:08X}, {:08X}, {}, {:08X}, {})",
                   proc_index, user_index, num_results, num_props, num_ctx, props_ptr, ctx_ptr,
                   results_buffer_size, search_results_ptr, num_users);

      return X_E_SUCCESS;
    }
    case 0x000B001D: {
      REX_ASSERT(!buffer_length || buffer_length == 24);

      struct {
        be_u32 obj_ptr;
        be_u32 details_buffer_size;
        be_u32 session_details_ptr;
        be_u32 reserved1;
        be_u32 reserved2;
        be_u32 reserved3;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 24);
      uint32_t obj_ptr = args->obj_ptr;
      uint32_t details_buffer_size = args->details_buffer_size;
      uint32_t session_details_ptr = args->session_details_ptr;
      uint32_t reserved1 = args->reserved1;
      uint32_t reserved2 = args->reserved2;
      uint32_t reserved3 = args->reserved3;

      REXLOG_DEBUG(kernel, "XSessionGetDetails({:08X}, {}, {:08X}, {}, {}, {})", obj_ptr,
                   details_buffer_size, session_details_ptr, reserved1, reserved2, reserved3);

      return X_E_SUCCESS;
    }
    case 0x000B001E: {
      REX_ASSERT(!buffer_length || buffer_length == 24);

      struct {
        be_u32 obj_ptr;
        be_u32 session_info_ptr;
        be_u32 user_index;
        be_u32 reserved1;
        be_u32 reserved2;
        be_u32 reserved3;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 24);
      uint32_t obj_ptr = args->obj_ptr;
      uint32_t session_info_ptr = args->session_info_ptr;
      uint32_t user_index = args->user_index;
      uint32_t reserved1 = args->reserved1;
      uint32_t reserved2 = args->reserved2;
      uint32_t reserved3 = args->reserved3;

      REXLOG_DEBUG(kernel, "XSessionMigrateHost({:08X}, {:08X}, {}, {}, {}, {})", obj_ptr,
                   session_info_ptr, user_index, reserved1, reserved2, reserved3);

      return X_E_SUCCESS;
    }
    case 0x000B0019: {
      REX_ASSERT(!buffer_length || buffer_length == 8);

      struct {
        be_u32 user_index;
        be_u32 session_info_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 8);
      uint32_t user_index = args->user_index;
      uint32_t session_info_ptr = args->session_info_ptr;

      REX_KERNEL_STUB("XSessionGetInvitationData({}, {:08X})", user_index, session_info_ptr);

      return X_E_SUCCESS;
    }
    case 0x000B001A: {
      REX_ASSERT(!buffer_length || buffer_length == 28);

#pragma pack(push, 4)
      struct {
        be_u32 obj_ptr;
        be_u32 flags;
        be_u64 session_nonce;
        be_u32 session_duration_sec;
        be_u32 results_buffer_size;
        be_u32 results_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
#pragma pack(pop)
      REX_STATIC_ASSERT_SIZE(decltype(*args), 28);
      uint32_t obj_ptr = args->obj_ptr;
      uint32_t flags = args->flags;
      uint64_t session_nonce = args->session_nonce;
      uint32_t session_duration_sec = args->session_duration_sec;  // 300
      uint32_t results_buffer_size = args->results_buffer_size;
      uint32_t results_ptr = args->results_ptr;

      REXLOG_DEBUG(
          kernel, "XSessionArbitrationRegister({:08X}, {:08X}, {:016X}, {:08X}, {:08X}, {:08X})",
          obj_ptr, flags, session_nonce, session_duration_sec, results_buffer_size, results_ptr);

      return X_E_SUCCESS;
    }
    case 0x000B001B: {
      REX_ASSERT(!buffer_length || buffer_length == 32);

      struct {
        be_u32 user_index;
        be_u32 num_session_ids;
        be_u32 session_ids_ptr;
        be_u32 results_buffer_size;
        be_u32 search_results_ptr;
        be_u32 reserved1;
        be_u32 reserved2;
        be_u32 reserved3;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 32);
      uint32_t user_index = args->user_index;
      uint32_t num_session_ids = args->num_session_ids;
      uint32_t session_ids_ptr = args->session_ids_ptr;
      uint32_t results_buffer_size = args->results_buffer_size;
      uint32_t search_results_ptr = args->search_results_ptr;
      uint32_t reserved1 = args->reserved1;
      uint32_t reserved2 = args->reserved2;
      uint32_t reserved3 = args->reserved3;

      REXLOG_DEBUG(kernel, "XSessionSearchByID({}, {:08X}, {:08X}, {:08X}, {:08X}, {}, {}, {})",
                   user_index, num_session_ids, session_ids_ptr, results_buffer_size,
                   search_results_ptr, reserved1, reserved2, reserved3);

      return X_E_SUCCESS;
    }
    case 0x000B001F: {
      REX_ASSERT(!buffer_length || buffer_length == 24);

      struct {
        be_u32 obj_ptr;
        be_u32 array_count;
        be_u32 xuid_array_ptr;
        be_u32 reserved1;
        be_u32 reserved2;
        be_u32 reserved3;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 24);
      uint32_t obj_ptr = args->obj_ptr;
      uint32_t array_count = args->array_count;
      uint32_t xuid_array_ptr = args->xuid_array_ptr;
      uint32_t reserved1 = args->reserved1;
      uint32_t reserved2 = args->reserved2;
      uint32_t reserved3 = args->reserved3;

      REXLOG_DEBUG(kernel, "XSessionModifySkill({:08X}, {}, {:08X}, {}, {}, {})", obj_ptr,
                   array_count, xuid_array_ptr, reserved1, reserved2, reserved3);

      return X_E_SUCCESS;
    }
    case 0x000B0020: {
      REX_ASSERT(!buffer_length || buffer_length == 8);

      struct {
        be_u32 user_index;
        be_u32 view_id;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 8);
      uint32_t user_index = args->user_index;
      uint32_t view_id = args->view_id;

      REXLOG_DEBUG(kernel, "XUserResetStatsView({:08X}, {})", user_index, view_id);

      return X_E_SUCCESS;
    }
    case 0x000B0021: {
      REX_ASSERT(!buffer_length || buffer_length == 28);

      struct {
        be_u32 title_id;
        be_u32 xuids_count;
        be_u32 xuids_ptr;
        be_u32 specs_count;
        be_u32 specs_ptr;
        be_u32 results_size;
        be_u32 results_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 28);
      uint32_t title_id = args->title_id;
      uint32_t xuids_count = args->xuids_count;
      uint32_t xuids_ptr = args->xuids_ptr;
      uint32_t specs_count = args->specs_count;
      uint32_t specs_ptr = args->specs_ptr;
      uint32_t results_size = args->results_size;
      uint32_t results_ptr = args->results_ptr;

      REXLOG_DEBUG(kernel, "XUserReadStats({}, {}, {:08X}, {}, {:08X}, {}, {:08X})", title_id,
                   xuids_count, xuids_ptr, specs_count, specs_ptr, results_size, results_ptr);

      return X_E_SUCCESS;
    }
    case 0x000B0025: {
      REX_ASSERT(!buffer_length || buffer_length == 20);

#pragma pack(push, 4)
      struct {
        be_u32 obj_ptr;
        be_u64 xuid;
        be_u32 num_views;
        be_u32 views_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
#pragma pack(pop)
      REX_STATIC_ASSERT_SIZE(decltype(*args), 20);
      uint32_t obj_ptr = args->obj_ptr;
      uint64_t xuid = args->xuid;
      uint32_t num_views = args->num_views;
      uint32_t views_ptr = args->views_ptr;

      REXLOG_DEBUG(kernel, "XSessionWriteStats({:08X}, {:016X}, {:08X}, {:08X})", obj_ptr, xuid,
                   num_views, views_ptr);

      return X_E_SUCCESS;
    }
    case 0x000B0026: {
      REX_ASSERT(!buffer_length || buffer_length == 20);

#pragma pack(push, 4)
      struct {
        be_u32 obj_ptr;
        be_u64 xuid;
        be_u32 num_views;
        be_u32 views_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
#pragma pack(pop)
      REX_STATIC_ASSERT_SIZE(decltype(*args), 20);
      uint32_t obj_ptr = args->obj_ptr;
      uint64_t xuid = args->xuid;
      uint32_t num_views = args->num_views;
      uint32_t views_ptr = args->views_ptr;

      REXLOG_DEBUG(kernel, "XSessionFlushStats({:08X}, {:016X}, {:08X}, {:08X})", obj_ptr, xuid,
                   num_views, views_ptr);

      return X_E_SUCCESS;
    }
    case 0x000B0036: {
      // Called after opening Live Arcade and clicking on Live v5759
      // to 5787 and called after clicking Live in the game library from
      // v6683 to v6717
      // Does not get sent a buffer
      REX_KERNEL_STUB("XInvalidateGamerTileCache");
      return X_E_FAIL;
    }
    case 0x000B003D: {
      REX_ASSERT(!buffer_length || buffer_length == 16);

      struct {
        be_u32 user_index;
        be_u32 AnId_buffer_size;
        be_u32 AnId_buffer_ptr;
        be_u32 block;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 16);
      uint32_t user_index = args->user_index;
      uint32_t AnId_buffer_size = args->AnId_buffer_size;
      uint32_t AnId_buffer_ptr = args->AnId_buffer_ptr;
      uint32_t block = args->block;

      REXLOG_DEBUG(kernel, "XUserGetANID({:08X}, {:08X}, {:08X}, {:08X})", user_index,
                   AnId_buffer_size, AnId_buffer_ptr, block);

      return X_E_SUCCESS;
    }
    case 0x000B0041: {
      REX_ASSERT(!buffer_length || buffer_length == 32);
      // 00000000 2789fecc 00000000 00000000 200491e0 00000000 200491f0 20049340
      struct {
        be_u32 user_index;
        u8 pad_04[12];
        be_u32 context_ptr;
        u8 pad_14[12];
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 32);
      struct Context {
        be_u32 id;
        be_u32 value;
      };
      REX_STATIC_ASSERT_SIZE(Context, 8);
      uint32_t user_index = args->user_index;
      uint32_t context_ptr = args->context_ptr;
      auto* context = context_ptr ? memory_->TranslateVirtual<Context*>(context_ptr) : nullptr;
      uint32_t context_id = context ? u32{context->id} : 0;
      REXLOG_DEBUG(kernel, "XGIUserGetContext({:08X}, {:08X}, {:08X}))", user_index, context_ptr,
                   context_id);
      uint32_t value = 0;
      if (context) {
        context->value = value;
      }
      return X_E_FAIL;
    }
    case 0x000B0060: {
      REX_ASSERT(!buffer_length || buffer_length == 32);

      struct {
        be_u32 user_index;
        be_u32 num_session_ids;
        be_u32 session_ids_ptr;
        be_u32 results_buffer_size;
        be_u32 search_results_ptr;
        be_u32 reserved1;
        be_u32 reserved2;
        be_u32 reserved3;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 32);
      uint32_t user_index = args->user_index;
      uint32_t num_session_ids = args->num_session_ids;
      uint32_t session_ids_ptr = args->session_ids_ptr;
      uint32_t results_buffer_size = args->results_buffer_size;
      uint32_t search_results_ptr = args->search_results_ptr;
      uint32_t reserved1 = args->reserved1;
      uint32_t reserved2 = args->reserved2;
      uint32_t reserved3 = args->reserved3;

      REXLOG_DEBUG(kernel,
                   "XSessionSearchByIds({:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {}, {}, {})",
                   user_index, num_session_ids, session_ids_ptr, results_buffer_size,
                   search_results_ptr, reserved1, reserved2, reserved3);

      return X_E_SUCCESS;
    }
    case 0x000B0065: {
      REX_ASSERT(!buffer_length || buffer_length == 52);

      struct {
        be_u32 proc_index;
        be_u32 user_index;
        be_u32 num_results;
        be_u16 num_weighted_properties;
        be_u16 num_weighted_contexts;
        be_u32 weighted_search_properties_ptr;
        be_u32 weighted_search_contexts_ptr;
        be_u16 num_props;
        be_u16 num_ctx;
        be_u32 non_weighted_search_properties_ptr;
        be_u32 non_weighted_search_contexts_ptr;
        be_u32 results_buffer_size;
        be_u32 search_results_ptr;
        be_u32 num_users;
        be_u32 weighted_search;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 52);
      uint32_t proc_index = args->proc_index;
      uint32_t user_index = args->user_index;
      uint32_t num_results = args->num_results;
      uint16_t num_weighted_properties = args->num_weighted_properties;
      uint16_t num_weighted_contexts = args->num_weighted_contexts;
      uint32_t weighted_search_properties_ptr = args->weighted_search_properties_ptr;
      uint32_t weighted_search_contexts_ptr = args->weighted_search_contexts_ptr;
      uint16_t num_props = args->num_props;
      uint16_t num_ctx = args->num_ctx;
      uint32_t non_weighted_search_properties_ptr = args->non_weighted_search_properties_ptr;
      uint32_t non_weighted_search_contexts_ptr = args->non_weighted_search_contexts_ptr;
      uint32_t results_buffer_size = args->results_buffer_size;
      uint32_t search_results_ptr = args->search_results_ptr;
      uint32_t num_users = args->num_users;
      uint32_t weighted_search = args->weighted_search;

      REXLOG_DEBUG(
          kernel,
          "XSessionSearchWeighted({:08X}, {:08X}, {:08X}, {}, {}, {:08X}, {:08X}, {}, {}, {:08X}, "
          "{:08X}, {:08X}, {:08X}, {:08X}, {:08X})",
          proc_index, user_index, num_results, num_weighted_properties, num_weighted_contexts,
          weighted_search_properties_ptr, weighted_search_contexts_ptr, num_props, num_ctx,
          non_weighted_search_properties_ptr, non_weighted_search_contexts_ptr, results_buffer_size,
          search_results_ptr, num_users, weighted_search);

      return X_E_SUCCESS;
    }
    case 0x000B0071: {
      REX_KERNEL_STUB("XGI 0x000B0071");
      return X_E_SUCCESS;
    }
  }
  REX_FAIL(kernel, X_E_FAIL,
           "Unimplemented XGI message app={:08X}, msg={:08X}, arg1={:08X}, "
           "arg2={:08X}",
           app_id(), message, buffer_ptr, buffer_length);
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace rex
