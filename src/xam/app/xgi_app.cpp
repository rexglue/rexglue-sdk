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

#include "xam/app/apps.h"
#include <rex/logging/log.h>
#include "xam/categories.h"
#include <rex/kernel/heap.h>
#include <rex/platform/thread.h>
#include <rex/xam/achievement_manager.h>
#include <rex/xam/app_manager.h>
#include <rex/xam/title_lifecycle.h>
#include "xam/stub_log.h"

namespace rex {
namespace xam {

using namespace rex::kernel;
namespace apps {
using namespace rex::kernel;

XGIApp::XGIApp(const XAMKernelInterfaces& title, rex::xam::IAchievementManager& achievements)
    : rex::xam::XAMApplet(title, 0xFB), achievements_(achievements) {}

kernel::X_HRESULT XGIApp::DispatchMessageSync(u32 message, u32 buffer_ptr, u32 buffer_length) {
  switch (message) {
    case 0x000B0006: {
      REX_ASSERT(!buffer_length || buffer_length == 24);
      struct {
        be_u32 user_index;
        u8 pad_04[12];
        be_u32 context_id;
        be_u32 context_value;
      }* args = membase_.TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 24);
      u32 user_index = args->user_index;
      u32 context_id = args->context_id;
      u32 context_value = args->context_value;
      REXLOG_DEBUG(xam, "XGIUserSetContextEx({:08X}, {:08X}, {:08X})", user_index, context_id,
                   context_value);
      return kernel::X_E_SUCCESS;
    }
    case 0x000B0007: {
      struct {
        be_u32 user_index;
        u8 pad_04[12];
        be_u32 property_id;
        be_u32 value_size;
        be_u32 value_ptr;
      }* args = membase_.TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 28);
      u32 user_index = args->user_index;
      u32 property_id = args->property_id;
      u32 value_size = args->value_size;
      u32 value_ptr = args->value_ptr;
      REXLOG_DEBUG(xam, "XGIUserSetPropertyEx({:08X}, {:08X}, {}, {:08X})", user_index, property_id,
                   value_size, value_ptr);
      return kernel::X_E_SUCCESS;
    }
    case 0x000B0008: {
      struct {
        be_u32 count;
        be_u32 achievements_ptr;
      }* args = membase_.TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 8);
      u32 raw0 = buffer_length >= 4 ? u32{args->count} : 0;
      u32 raw4 = buffer_length >= 8 ? u32{args->achievements_ptr} : 0;
      REXLOG_DEBUG(xam, "XGIUserWriteAchievements called: buf_len={} raw[0]={:08X} raw[4]={:08X}",
                   buffer_length, raw0, raw4);

      REX_ASSERT(!buffer_length || buffer_length == 8);
      u32 achievement_count = raw0;
      u32 achievements_ptr = raw4;

      struct Entry {
        be_u32 user_index;
        be_u32 id;
      };
      REX_STATIC_ASSERT_SIZE(Entry, 8);
      static_assert(offsetof(Entry, id) == 4);
      constexpr u32 kMaxAchievements = 1000;

      if (achievements_ptr && achievement_count > 0) {
        if (achievement_count > kMaxAchievements) {
          REXLOG_WARN(xam, "XGIUserWriteAchievements: count={} unreasonable, ignoring",
                      achievement_count);
          return kernel::X_E_FAIL;
        }
        u32 span_end = achievements_ptr + achievement_count * sizeof(Entry) - 1;
        if (!title_.heap.IsAddressMapped(achievements_ptr) ||
            !title_.heap.IsAddressMapped(span_end)) {
          REXLOG_WARN(xam, "XGIUserWriteAchievements: ptr {:08X} OOB", achievements_ptr);
          return kernel::X_E_FAIL;
        }
        const auto* entries = membase_.TranslateVirtual<const Entry*>(achievements_ptr);
        for (u32 i = 0; i < achievement_count; ++i) {
          u32 id = entries[i].id;
          REXLOG_DEBUG(xam, "XGIUserWriteAchievements: id={} ({})", id, i);
          (void)achievements_.UnlockAchievement(id, rex::xam::AchievementNotification::kShow);
        }
      } else {
        REXLOG_DEBUG(xam, "XGIUserWriteAchievements: skipped (count={} ptr={:08X})",
                     achievement_count, achievements_ptr);
      }
      return kernel::X_E_SUCCESS;
    }
    case 0x000B0010: {
      REX_ASSERT(!buffer_length || buffer_length == 28);
      struct {
        be_u32 session_ptr;
        be_u32 flags;
        be_u32 num_slots_public;
        be_u32 num_slots_private;
        be_u32 user_xuid;
        be_u32 session_info_ptr;
        be_u32 nonce_ptr;
      }* args = membase_.TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 28);
      u32 session_ptr = args->session_ptr;
      u32 flags = args->flags;
      u32 num_slots_public = args->num_slots_public;
      u32 num_slots_private = args->num_slots_private;
      u32 user_xuid = args->user_xuid;
      u32 session_info_ptr = args->session_info_ptr;
      u32 nonce_ptr = args->nonce_ptr;

      REXLOG_DEBUG(xam,
                   "XGISessionCreate({:08X}, {:08X}, {}, {}, {:08X}, {:08X}, "
                   "{:08X})",
                   session_ptr, flags, num_slots_public, num_slots_private, user_xuid,
                   session_info_ptr, nonce_ptr);
      return kernel::X_E_SUCCESS;
    }
    case 0x000B0011: {
      REX_ASSERT(!buffer_length || buffer_length == 16);

#pragma pack(push, 4)
      struct {
        be_u32 obj_ptr;
        be_u32 flags;
        be_u64 session_nonce;
      }* args = membase_.TranslateVirtual<decltype(args)>(buffer_ptr);
#pragma pack(pop)
      REX_STATIC_ASSERT_SIZE(decltype(*args), 16);
      u32 obj_ptr = args->obj_ptr;
      u32 flags = args->flags;
      u64 session_nonce = args->session_nonce;

      REXLOG_DEBUG(xam, "XGISessionDelete({:08X}, {:08X}, {:016X})", obj_ptr, flags, session_nonce);

      return kernel::X_E_SUCCESS;
    }
    case 0x000B0012: {
      REX_ASSERT(!buffer_length || buffer_length == 20);
      struct {
        be_u32 session_ptr;
        be_u32 user_count;
        be_u32 xuid_array_ptr;
        be_u32 user_index_array;
        be_u32 private_slots_array;
      }* args = membase_.TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 20);
      u32 session_ptr = args->session_ptr;
      u32 user_count = args->user_count;
      u32 xuid_array_ptr = args->xuid_array_ptr;
      u32 user_index_array = args->user_index_array;
      u32 private_slots_array = args->private_slots_array;
      bool is_local = xuid_array_ptr == 0;

      REXLOG_DEBUG(xam, "{}({:08X}, {}, {}, {:08X}, {:08X})",
                   is_local ? "XGISessionJoinLocal" : "XGISessionJoinRemote", session_ptr,
                   user_count, xuid_array_ptr, user_index_array, private_slots_array);
      return kernel::X_E_SUCCESS;
    }
    case 0x000B0014: {
      REX_ASSERT(!buffer_length || buffer_length == 16);

#pragma pack(push, 4)
      struct {
        be_u32 obj_ptr;
        be_u32 flags;
        be_u64 session_nonce;
      }* args = membase_.TranslateVirtual<decltype(args)>(buffer_ptr);
#pragma pack(pop)
      REX_STATIC_ASSERT_SIZE(decltype(*args), 16);
      u32 obj_ptr = args->obj_ptr;
      u32 flags = args->flags;
      u64 session_nonce = args->session_nonce;

      REXLOG_DEBUG(xam, "XSessionStart({:08X}, {:08X}, {:016X})", obj_ptr, flags, session_nonce);

      return kernel::X_STATUS_SUCCESS;
    }
    case 0x000B0015: {
      REX_ASSERT(!buffer_length || buffer_length == 16);

#pragma pack(push, 4)
      struct {
        be_u32 obj_ptr;
        be_u32 flags;
        be_u64 session_nonce;
      }* args = membase_.TranslateVirtual<decltype(args)>(buffer_ptr);
#pragma pack(pop)
      REX_STATIC_ASSERT_SIZE(decltype(*args), 16);
      u32 obj_ptr = args->obj_ptr;
      u32 flags = args->flags;
      u64 session_nonce = args->session_nonce;

      REXLOG_DEBUG(xam, "XSessionEnd({:08X}, {:08X}, {:016X})", obj_ptr, flags, session_nonce);

      return kernel::X_E_SUCCESS;
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
      }* args = membase_.TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 32);
      u32 proc_index = args->proc_index;
      u32 user_index = args->user_index;
      u32 num_results = args->num_results;
      u16 num_props = args->num_props;
      u16 num_ctx = args->num_ctx;
      u32 props_ptr = args->props_ptr;
      u32 ctx_ptr = args->ctx_ptr;
      u32 results_buffer_size = args->results_buffer_size;
      u32 search_results_ptr = args->search_results_ptr;

      REXLOG_DEBUG(xam, "XSessionSearch({}, {}, {}, {}, {}, {:08X}, {:08X}, {}, {:08X})",
                   proc_index, user_index, num_results, num_props, num_ctx, props_ptr, ctx_ptr,
                   results_buffer_size, search_results_ptr);
      return kernel::X_E_SUCCESS;
    }
    case 0x000B0018: {
      REX_ASSERT(!buffer_length || buffer_length == 16);

      struct {
        be_u32 obj_ptr;
        be_u32 flags;
        be_u32 max_public_slots;
        be_u16 max_private_slots;
        u8 pad_0e[2];
      }* args = membase_.TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 16);
      u32 obj_ptr = args->obj_ptr;
      u32 flags = args->flags;
      u32 max_public_slots = args->max_public_slots;
      u16 max_private_slots = args->max_private_slots;

      REXLOG_DEBUG(xam, "XSessionModify({:08X}, {:08X}, {:08X}, {:08X})", obj_ptr, flags,
                   max_public_slots, max_private_slots);

      return kernel::X_E_SUCCESS;
    }
    case 0x000B001C: {
      REX_ASSERT(!buffer_length || buffer_length == 36);

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
      }* args = membase_.TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 36);
      u32 proc_index = args->proc_index;
      u32 user_index = args->user_index;
      u32 num_results = args->num_results;
      u16 num_props = args->num_props;
      u16 num_ctx = args->num_ctx;
      u32 props_ptr = args->props_ptr;
      u32 ctx_ptr = args->ctx_ptr;
      u32 results_buffer_size = args->results_buffer_size;
      u32 search_results_ptr = args->search_results_ptr;
      u32 num_users = args->num_users;

      REXLOG_DEBUG(xam, "XSessionSearchEx({}, {}, {}, {}, {}, {:08X}, {:08X}, {}, {:08X}, {})",
                   proc_index, user_index, num_results, num_props, num_ctx, props_ptr, ctx_ptr,
                   results_buffer_size, search_results_ptr, num_users);

      return kernel::X_E_SUCCESS;
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
      }* args = membase_.TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 24);
      u32 obj_ptr = args->obj_ptr;
      u32 details_buffer_size = args->details_buffer_size;
      u32 session_details_ptr = args->session_details_ptr;
      u32 reserved1 = args->reserved1;
      u32 reserved2 = args->reserved2;
      u32 reserved3 = args->reserved3;

      REXLOG_DEBUG(xam, "XSessionGetDetails({:08X}, {}, {:08X}, {}, {}, {})", obj_ptr,
                   details_buffer_size, session_details_ptr, reserved1, reserved2, reserved3);

      return kernel::X_E_SUCCESS;
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
      }* args = membase_.TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 24);
      u32 obj_ptr = args->obj_ptr;
      u32 session_info_ptr = args->session_info_ptr;
      u32 user_index = args->user_index;
      u32 reserved1 = args->reserved1;
      u32 reserved2 = args->reserved2;
      u32 reserved3 = args->reserved3;

      REXLOG_DEBUG(xam, "XSessionMigrateHost({:08X}, {:08X}, {}, {}, {}, {})", obj_ptr,
                   session_info_ptr, user_index, reserved1, reserved2, reserved3);

      return kernel::X_E_SUCCESS;
    }
    case 0x000B0019: {
      REX_ASSERT(!buffer_length || buffer_length == 8);

      struct {
        be_u32 user_index;
        be_u32 session_info_ptr;
      }* args = membase_.TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 8);
      u32 user_index = args->user_index;
      u32 session_info_ptr = args->session_info_ptr;

      REX_XAM_STUB("XSessionGetInvitationData({}, {:08X})", user_index, session_info_ptr);

      return kernel::X_E_SUCCESS;
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
      }* args = membase_.TranslateVirtual<decltype(args)>(buffer_ptr);
#pragma pack(pop)
      REX_STATIC_ASSERT_SIZE(decltype(*args), 28);
      u32 obj_ptr = args->obj_ptr;
      u32 flags = args->flags;
      u64 session_nonce = args->session_nonce;
      u32 session_duration_sec = args->session_duration_sec;
      u32 results_buffer_size = args->results_buffer_size;
      u32 results_ptr = args->results_ptr;

      REXLOG_DEBUG(
          xam, "XSessionArbitrationRegister({:08X}, {:08X}, {:016X}, {:08X}, {:08X}, {:08X})",
          obj_ptr, flags, session_nonce, session_duration_sec, results_buffer_size, results_ptr);

      return kernel::X_E_SUCCESS;
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
      }* args = membase_.TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 32);
      u32 user_index = args->user_index;
      u32 num_session_ids = args->num_session_ids;
      u32 session_ids_ptr = args->session_ids_ptr;
      u32 results_buffer_size = args->results_buffer_size;
      u32 search_results_ptr = args->search_results_ptr;
      u32 reserved1 = args->reserved1;
      u32 reserved2 = args->reserved2;
      u32 reserved3 = args->reserved3;

      REXLOG_DEBUG(xam, "XSessionSearchByID({}, {:08X}, {:08X}, {:08X}, {:08X}, {}, {}, {})",
                   user_index, num_session_ids, session_ids_ptr, results_buffer_size,
                   search_results_ptr, reserved1, reserved2, reserved3);

      return kernel::X_E_SUCCESS;
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
      }* args = membase_.TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 24);
      u32 obj_ptr = args->obj_ptr;
      u32 array_count = args->array_count;
      u32 xuid_array_ptr = args->xuid_array_ptr;
      u32 reserved1 = args->reserved1;
      u32 reserved2 = args->reserved2;
      u32 reserved3 = args->reserved3;

      REXLOG_DEBUG(xam, "XSessionModifySkill({:08X}, {}, {:08X}, {}, {}, {})", obj_ptr, array_count,
                   xuid_array_ptr, reserved1, reserved2, reserved3);

      return kernel::X_E_SUCCESS;
    }
    case 0x000B0020: {
      REX_ASSERT(!buffer_length || buffer_length == 8);

      struct {
        be_u32 user_index;
        be_u32 view_id;
      }* args = membase_.TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 8);
      u32 user_index = args->user_index;
      u32 view_id = args->view_id;

      REXLOG_DEBUG(xam, "XUserResetStatsView({:08X}, {})", user_index, view_id);

      return kernel::X_E_SUCCESS;
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
      }* args = membase_.TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 28);
      u32 title_id = args->title_id;
      u32 xuids_count = args->xuids_count;
      u32 xuids_ptr = args->xuids_ptr;
      u32 specs_count = args->specs_count;
      u32 specs_ptr = args->specs_ptr;
      u32 results_size = args->results_size;
      u32 results_ptr = args->results_ptr;

      REXLOG_DEBUG(xam, "XUserReadStats({}, {}, {:08X}, {}, {:08X}, {}, {:08X})", title_id,
                   xuids_count, xuids_ptr, specs_count, specs_ptr, results_size, results_ptr);

      return kernel::X_E_SUCCESS;
    }
    case 0x000B0025: {
      REX_ASSERT(!buffer_length || buffer_length == 20);

#pragma pack(push, 4)
      struct {
        be_u32 obj_ptr;
        be_u64 xuid;
        be_u32 num_views;
        be_u32 views_ptr;
      }* args = membase_.TranslateVirtual<decltype(args)>(buffer_ptr);
#pragma pack(pop)
      REX_STATIC_ASSERT_SIZE(decltype(*args), 20);
      u32 obj_ptr = args->obj_ptr;
      u64 xuid = args->xuid;
      u32 num_views = args->num_views;
      u32 views_ptr = args->views_ptr;

      REXLOG_DEBUG(xam, "XSessionWriteStats({:08X}, {:016X}, {:08X}, {:08X})", obj_ptr, xuid,
                   num_views, views_ptr);

      return kernel::X_E_SUCCESS;
    }
    case 0x000B0026: {
      REX_ASSERT(!buffer_length || buffer_length == 20);

#pragma pack(push, 4)
      struct {
        be_u32 obj_ptr;
        be_u64 xuid;
        be_u32 num_views;
        be_u32 views_ptr;
      }* args = membase_.TranslateVirtual<decltype(args)>(buffer_ptr);
#pragma pack(pop)
      REX_STATIC_ASSERT_SIZE(decltype(*args), 20);
      u32 obj_ptr = args->obj_ptr;
      u64 xuid = args->xuid;
      u32 num_views = args->num_views;
      u32 views_ptr = args->views_ptr;

      REXLOG_DEBUG(xam, "XSessionFlushStats({:08X}, {:016X}, {:08X}, {:08X})", obj_ptr, xuid,
                   num_views, views_ptr);

      return kernel::X_E_SUCCESS;
    }
    case 0x000B0036: {
      // The message carries no buffer.
      REX_XAM_STUB("XInvalidateGamerTileCache");
      return kernel::X_E_FAIL;
    }
    case 0x000B003D: {
      REX_ASSERT(!buffer_length || buffer_length == 16);

      struct {
        be_u32 user_index;
        be_u32 anid_buffer_size;
        be_u32 anid_buffer_ptr;
        be_u32 block;
      }* args = membase_.TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 16);
      u32 user_index = args->user_index;
      u32 anid_buffer_size = args->anid_buffer_size;
      u32 anid_buffer_ptr = args->anid_buffer_ptr;
      u32 block = args->block;

      REXLOG_DEBUG(xam, "XUserGetANID({:08X}, {:08X}, {:08X}, {:08X})", user_index,
                   anid_buffer_size, anid_buffer_ptr, block);

      return kernel::X_E_SUCCESS;
    }
    case 0x000B0041: {
      REX_ASSERT(!buffer_length || buffer_length == 32);
      struct {
        be_u32 user_index;
        u8 pad_04[12];
        be_u32 context_ptr;
        u8 pad_14[12];
      }* args = membase_.TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 32);
      struct Context {
        be_u32 id;
        be_u32 value;
      };
      REX_STATIC_ASSERT_SIZE(Context, 8);
      u32 user_index = args->user_index;
      u32 context_ptr = args->context_ptr;
      auto* context = context_ptr ? membase_.TranslateVirtual<Context*>(context_ptr) : nullptr;
      u32 context_id = context ? u32{context->id} : 0;
      REXLOG_DEBUG(xam, "XGIUserGetContext({:08X}, {:08X}, {:08X}))", user_index, context_ptr,
                   context_id);
      u32 value = 0;
      if (context) {
        context->value = value;
      }
      return kernel::X_E_FAIL;
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
      }* args = membase_.TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 32);
      u32 user_index = args->user_index;
      u32 num_session_ids = args->num_session_ids;
      u32 session_ids_ptr = args->session_ids_ptr;
      u32 results_buffer_size = args->results_buffer_size;
      u32 search_results_ptr = args->search_results_ptr;
      u32 reserved1 = args->reserved1;
      u32 reserved2 = args->reserved2;
      u32 reserved3 = args->reserved3;

      REXLOG_DEBUG(xam, "XSessionSearchByIds({:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {}, {}, {})",
                   user_index, num_session_ids, session_ids_ptr, results_buffer_size,
                   search_results_ptr, reserved1, reserved2, reserved3);

      return kernel::X_E_SUCCESS;
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
      }* args = membase_.TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 52);
      u32 proc_index = args->proc_index;
      u32 user_index = args->user_index;
      u32 num_results = args->num_results;
      u16 num_weighted_properties = args->num_weighted_properties;
      u16 num_weighted_contexts = args->num_weighted_contexts;
      u32 weighted_search_properties_ptr = args->weighted_search_properties_ptr;
      u32 weighted_search_contexts_ptr = args->weighted_search_contexts_ptr;
      u16 num_props = args->num_props;
      u16 num_ctx = args->num_ctx;
      u32 non_weighted_search_properties_ptr = args->non_weighted_search_properties_ptr;
      u32 non_weighted_search_contexts_ptr = args->non_weighted_search_contexts_ptr;
      u32 results_buffer_size = args->results_buffer_size;
      u32 search_results_ptr = args->search_results_ptr;
      u32 num_users = args->num_users;
      u32 weighted_search = args->weighted_search;

      REXLOG_DEBUG(
          xam,
          "XSessionSearchWeighted({:08X}, {:08X}, {:08X}, {}, {}, {:08X}, {:08X}, {}, {}, {:08X}, "
          "{:08X}, {:08X}, {:08X}, {:08X}, {:08X})",
          proc_index, user_index, num_results, num_weighted_properties, num_weighted_contexts,
          weighted_search_properties_ptr, weighted_search_contexts_ptr, num_props, num_ctx,
          non_weighted_search_properties_ptr, non_weighted_search_contexts_ptr, results_buffer_size,
          search_results_ptr, num_users, weighted_search);

      return kernel::X_E_SUCCESS;
    }
    case 0x000B0071: {
      REX_XAM_STUB("XGI 0x000B0071");
      return kernel::X_E_SUCCESS;
    }
    default:
      break;
  }
  REX_FAIL(xam, kernel::X_E_FAIL,
           "Unimplemented XGI message app={:08X}, msg={:08X}, arg1={:08X}, "
           "arg2={:08X}",
           GetAppID(), message, buffer_ptr, buffer_length);
}

}  // namespace apps

}  // namespace xam
}  // namespace rex
