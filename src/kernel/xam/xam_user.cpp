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

#include <cstring>

#include <rex/api.h>
#include <rex/cvar.h>
#include <rex/kernel/xam/private.h>
#include <rex/logging/log.h>
#include <rex/math.h>
#include <rex/hook.h>
#include <rex/runtime/achievement_manager.h>
#include <rex/runtime/content_manager.h>
#include <rex/runtime/dispatcher.h>
#include <rex/runtime/notifier.h>
#include <rex/types.h>
#include <rex/string.h>
#include <rex/runtime.h>
#include <rex/runtime/xam/user_profile.h>
#include <rex/runtime/enumerator_object.h>
#include <rex/runtime/kernel_object.h>
#include <rex/runtime/object_table.h>
#include <rex/runtime/xio.h>
#include <rex/runtime/xthread.h>
#include <rex/runtime/xtypes.h>
#include "kernel/stub_log.h"
#include "kernel/object_reference.h"

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::runtime;

i32 XamUserGetXUID_entry(u32 user_index, u32 type_mask, MappedU64 xuid_ptr) {
  REX_ASSERT(type_mask == 1 || type_mask == 2 || type_mask == 3 || type_mask == 4 ||
             type_mask == 7);
  if (!xuid_ptr) {
    return static_cast<i32>(X_E_INVALIDARG);
  }
  u32 result = X_E_NO_SUCH_USER;
  u64 xuid = 0;
  if (user_index < 4) {
    if (user_index == 0) {
      auto& user_profile = runtime::GetCurrent().GetContentManager().GetUserProfile();
      auto type = user_profile.GetType() & type_mask;
      if (type & (1 | 2 | 4)) {
        xuid = user_profile.GetXUID();
        result = X_E_SUCCESS;
      }
    }
  } else {
    result = X_E_INVALIDARG;
  }
  *xuid_ptr = xuid;
  return static_cast<i32>(result);
}

u32 XamUserGetSigninState_entry(u32 user_index) {
  u32 signin_state = 0;
  if (user_index < 4) {
    if (user_index == 0) {
      auto& user_profile = runtime::GetCurrent().GetContentManager().GetUserProfile();
      signin_state = user_profile.GetSigninState();
    }
  }
  return signin_state;
}

struct X_USER_SIGNIN_INFO {
  be_u64 xuid;
  be_u32 unk08;
  be_u32 signin_state;
  be_u32 unk10;
  be_u32 unk14;
  char name[16];
};
REX_STATIC_ASSERT_SIZE(X_USER_SIGNIN_INFO, 40);

i32 XamUserGetSigninInfo_entry(u32 user_index, [[maybe_unused]] u32 flags,
                               GuestPtrT<X_USER_SIGNIN_INFO> info) {
  if (!info) {
    return static_cast<i32>(X_E_INVALIDARG);
  }

  std::memset(info, 0, sizeof(X_USER_SIGNIN_INFO));
  if (user_index) {
    return static_cast<i32>(X_E_NO_SUCH_USER);
  }

  auto& user_profile = runtime::GetCurrent().GetContentManager().GetUserProfile();
  info->xuid = user_profile.GetXUID();
  info->signin_state = user_profile.GetSigninState();
  rex::CopyTruncating(info->name, user_profile.GetName(), rex::CountOf(info->name));
  return X_E_SUCCESS;
}

u32 XamUserGetName_entry(u32 user_index, MappedString buffer, u32 buffer_len) {
  if (user_index >= 4) {
    return X_E_INVALIDARG;
  }

  if (user_index) {
    return X_E_NO_SUCH_USER;
  }

  auto& user_profile = runtime::GetCurrent().GetContentManager().GetUserProfile();
  const auto& user_name = user_profile.GetName();
  rex::CopyTruncating(buffer, user_name, std::min(buffer_len, static_cast<u32>(16)));
  return X_E_SUCCESS;
}

u32 XamUserGetGamerTag_entry(u32 user_index, MappedWString buffer, u32 buffer_len) {
  if (user_index >= 4) {
    return X_E_INVALIDARG;
  }

  if (user_index) {
    return X_E_NO_SUCH_USER;
  }

  if (!buffer || buffer_len < 16) {
    return X_E_INVALIDARG;
  }

  auto& user_profile = runtime::GetCurrent().GetContentManager().GetUserProfile();
  auto user_name = rex::ToUTF16(user_profile.GetName());
  rex::CopyAndSwapTruncating(buffer, user_name, std::min(buffer_len, static_cast<u32>(16)));
  return X_E_SUCCESS;
}

struct X_USER_READ_PROFILE_SETTINGS {
  be_u32 setting_count;
  be_u32 settings_ptr;
};
REX_STATIC_ASSERT_SIZE(X_USER_READ_PROFILE_SETTINGS, 8);

u32 XamUserReadProfileSettingsEx([[maybe_unused]] u32 title_id, u32 user_index, u32 xuid_count,
                                 be_u64* xuids, u32 setting_count, be_u32* setting_ids,
                                 [[maybe_unused]] u32 unk, be_u32* buffer_size_ptr, u8* buffer,
                                 XOVERLAPPED* overlapped) {
  const bool by_xuid = xuid_count && xuids;
  const u32 xuid_entries = by_xuid ? xuid_count : 1;

  if (setting_count < 1 || setting_count > 32) {
    return X_ERROR_INVALID_PARAMETER;
  }

  if (!buffer_size_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }

  auto buffer_size = static_cast<u32>(*buffer_size_ptr);
  if (buffer_size && !buffer) {
    return X_ERROR_INVALID_PARAMETER;
  }

  u32 needed_header_size = 0;
  u32 needed_data_size = 0;
  for (u32 i = 0; i < setting_count; ++i) {
    needed_header_size += sizeof(X_USER_PROFILE_SETTING);
    UserProfile::Setting::Key setting_key;
    setting_key.value = static_cast<u32>(setting_ids[i]);
    switch (static_cast<UserProfile::Setting::Type>(setting_key.type)) {
      case UserProfile::Setting::Type::kUnicode:
      case UserProfile::Setting::Type::kBinary:
        needed_data_size += setting_key.size;
        break;
      default:
        break;
    }
  }
  needed_header_size *= xuid_entries;
  needed_data_size *= xuid_entries;
  needed_header_size += sizeof(X_USER_READ_PROFILE_SETTINGS);

  u32 needed_size = needed_header_size + needed_data_size;
  if (!buffer || buffer_size < needed_size) {
    if (!buffer_size) {
      *buffer_size_ptr = needed_size;
    }
    return X_ERROR_INSUFFICIENT_BUFFER;
  }

  if (!by_xuid && user_index) {
    if (overlapped) {
      runtime::GetCurrent().GetDispatcher().Complete(
          runtime::GetCurrent().GetMemory()->HostToGuestVirtual(overlapped),
          {.result = X_ERROR_NO_SUCH_USER});
      return X_ERROR_IO_PENDING;
    }
    return X_ERROR_NO_SUCH_USER;
  }

  auto& user_profile = runtime::GetCurrent().GetContentManager().GetUserProfile();

  auto out_header = reinterpret_cast<X_USER_READ_PROFILE_SETTINGS*>(buffer);
  auto out_setting = reinterpret_cast<X_USER_PROFILE_SETTING*>(&out_header[1]);
  out_header->setting_count = xuid_entries * setting_count;
  out_header->settings_ptr = runtime::GetCurrent().GetMemory()->HostToGuestVirtual(out_setting);

  UserProfile::SettingByteStream out_stream(
      runtime::GetCurrent().GetMemory()->HostToGuestVirtual(buffer), buffer, buffer_size,
      needed_header_size);
  for (u32 x = 0; x < xuid_entries; ++x) {
    const u64 xuid = by_xuid ? static_cast<u64>(xuids[x]) : 0;
    const bool own_profile = !by_xuid || xuid == user_profile.GetXUID();
    for (u32 n = 0; n < setting_count; ++n) {
      u32 setting_id = setting_ids[n];
      UserProfile::Setting* setting = own_profile ? user_profile.GetSetting(setting_id) : nullptr;
      if (own_profile && !setting) {
        REX_KERNEL_STUB("XamUserReadProfileSettingsEx setting {:08X}", setting_id);
      }

      std::memset(out_setting, 0, sizeof(X_USER_PROFILE_SETTING));
      out_setting->from = !setting || !setting->is_set ? 0 : setting->IsTitleSpecific() ? 2 : 1;
      if (by_xuid) {
        out_setting->xuid = xuid;
      } else {
        out_setting->user_index = user_index;
      }
      out_setting->setting_id = setting_id;

      if (setting && setting->is_set) {
        setting->Append(&out_setting->data, &out_stream);
      }
      ++out_setting;
    }
  }

  if (overlapped) {
    runtime::GetCurrent().GetDispatcher().Complete(
        runtime::GetCurrent().GetMemory()->HostToGuestVirtual(overlapped),
        {.result = X_ERROR_SUCCESS});
    return X_ERROR_IO_PENDING;
  }
  return X_ERROR_SUCCESS;
}

u32 XamUserReadProfileSettings_entry(u32 title_id, u32 user_index, u32 xuid_count, MappedU64 xuids,
                                     u32 setting_count, MappedU32 setting_ids,
                                     MappedU32 buffer_size_ptr, MappedVoid buffer_ptr,
                                     GuestPtrT<XOVERLAPPED> overlapped) {
  return XamUserReadProfileSettingsEx(title_id, user_index, xuid_count, xuids, setting_count,
                                      setting_ids, 0, buffer_size_ptr, buffer_ptr, overlapped);
}

u32 XamUserReadProfileSettingsEx_entry(u32 title_id, u32 user_index, u32 xuid_count,
                                       MappedU64 xuids, u32 setting_count, MappedU32 setting_ids,
                                       MappedU32 buffer_size_ptr, u32 unk_2, MappedVoid buffer_ptr,
                                       GuestPtrT<XOVERLAPPED> overlapped) {
  return XamUserReadProfileSettingsEx(title_id, user_index, xuid_count, xuids, setting_count,
                                      setting_ids, unk_2, buffer_size_ptr, buffer_ptr, overlapped);
}

u32 XamUserWriteProfileSettings_entry([[maybe_unused]] u32 title_id, u32 user_index,
                                      u32 setting_count, GuestPtrT<X_USER_PROFILE_SETTING> settings,
                                      GuestPtrT<XOVERLAPPED> overlapped) {
  if (!setting_count || !settings) {
    return X_ERROR_INVALID_PARAMETER;
  }

  if (user_index) {
    if (overlapped) {
      runtime::GetCurrent().GetDispatcher().Complete(overlapped.GetGuestAddress(),
                                                     {.result = X_ERROR_NO_SUCH_USER});
      return X_ERROR_IO_PENDING;
    }
    return X_ERROR_NO_SUCH_USER;
  }

  auto& user_profile = runtime::GetCurrent().GetContentManager().GetUserProfile();

  for (u32 n = 0; n < setting_count; ++n) {
    const X_USER_PROFILE_SETTING& setting = settings[n];

    auto setting_type = static_cast<UserProfile::Setting::Type>(setting.data.type);
    if (setting_type == UserProfile::Setting::Type::kUnset) {
      continue;
    }

    REXLOG_DEBUG(kernel,
                 "XamUserWriteProfileSettings: setting index [{}]:"
                 " from={} setting_id={:08X} data.type={}",
                 n, static_cast<u32>(setting.from), static_cast<u32>(setting.setting_id),
                 setting.data.type);

    switch (setting_type) {
      case UserProfile::Setting::Type::kContext:
      case UserProfile::Setting::Type::kBinary: {
        u8* binary_ptr =
            runtime::GetCurrent().GetMemory()->TranslateVirtual(setting.data.binary.ptr);
        size_t binary_size = setting.data.binary.size;
        std::vector<u8> bytes(binary_size, 0);
        if (setting.data.binary.ptr && binary_size) {
          std::memcpy(bytes.data(), binary_ptr, binary_size);
        }
        user_profile.AddSetting(
            std::make_unique<runtime::UserProfile::BinarySetting>(setting.setting_id, bytes));
      } break;
      case UserProfile::Setting::Type::kUnicode:
      case UserProfile::Setting::Type::kDouble:
      case UserProfile::Setting::Type::kFloat:
      case UserProfile::Setting::Type::kInt32:
      case UserProfile::Setting::Type::kInt64:
      case UserProfile::Setting::Type::kDateTime:
      default: {
        REX_KERNEL_STUB("XamUserWriteProfileSettings data type {}", setting_type);
      } break;
    };
  }

  if (overlapped) {
    runtime::GetCurrent().GetDispatcher().Complete(overlapped.GetGuestAddress(),
                                                   {.result = X_ERROR_SUCCESS});
    return X_ERROR_IO_PENDING;
  }
  return X_ERROR_SUCCESS;
}

u32 XamUserCheckPrivilege_entry(u32 user_index, [[maybe_unused]] u32 mask, MappedU32 out_value) {
  // 0xFF is XUSER_INDEX_ANY.
  if (user_index != 0xFF) {
    if (user_index >= 4) {
      return X_ERROR_INVALID_PARAMETER;
    }

    if (user_index) {
      return X_ERROR_NO_SUCH_USER;
    }
  }

  *out_value = 0;
  return X_ERROR_SUCCESS;
}

u32 XamUserContentRestrictionGetFlags_entry(u32 user_index, MappedU32 out_flags) {
  if (user_index) {
    return X_ERROR_NO_SUCH_USER;
  }

  *out_flags = 0;
  return X_ERROR_SUCCESS;
}

u32 XamUserContentRestrictionGetRating_entry(u32 user_index, [[maybe_unused]] u32 unk1,
                                             MappedU32 out_unk2, MappedU32 out_unk3) {
  if (user_index) {
    return X_ERROR_NO_SUCH_USER;
  }

  *out_unk2 = 0x3F;
  *out_unk3 = 0;
  return X_ERROR_SUCCESS;
}

u32 XamUserContentRestrictionCheckAccess_entry([[maybe_unused]] u32 user_index,
                                               [[maybe_unused]] u32 unk1, [[maybe_unused]] u32 unk2,
                                               [[maybe_unused]] u32 unk3, [[maybe_unused]] u32 unk4,
                                               MappedU32 out_unk5, u32 overlapped_ptr) {
  *out_unk5 = 1;

  if (overlapped_ptr) {
    runtime::GetCurrent().GetDispatcher().Complete(overlapped_ptr, {.result = X_ERROR_SUCCESS});
  }

  return X_ERROR_SUCCESS;
}

u32 XamUserIsOnlineEnabled_entry([[maybe_unused]] u32 user_index) {
  return 1;
}

u32 XamUserGetMembershipTier_entry(u32 user_index) {
  if (user_index >= 4) {
    return X_ERROR_INVALID_PARAMETER;
  }
  if (user_index) {
    return X_ERROR_NO_SUCH_USER;
  }
  return 6;
}

u32 XamUserAreUsersFriends_entry(u32 user_index, [[maybe_unused]] u32 unk1,
                                 [[maybe_unused]] u32 unk2, MappedU32 out_value,
                                 u32 overlapped_ptr) {
  u32 are_friends = 0;
  X_RESULT result;

  if (user_index >= 4) {
    result = X_ERROR_INVALID_PARAMETER;
  } else {
    if (user_index == 0) {
      auto& user_profile = runtime::GetCurrent().GetContentManager().GetUserProfile();
      if (user_profile.GetSigninState() == 0) {
        result = X_ERROR_NOT_LOGGED_ON;
      } else {
        are_friends = 0;
        result = X_ERROR_SUCCESS;
      }
    } else {
      result = X_ERROR_NO_SUCH_USER;
    }
  }

  if (out_value) {
    REX_ASSERT(!overlapped_ptr);
    *out_value = result == X_ERROR_SUCCESS ? are_friends : 0;
    return result;
  } else if (overlapped_ptr) {
    REX_ASSERT(!out_value);
    runtime::GetCurrent().GetDispatcher().Complete(
        overlapped_ptr,
        {.result = result == X_ERROR_SUCCESS ? X_ERROR_SUCCESS : X_ERROR_FUNCTION_FAILED,
         .extended_error = XHResultFromWin32(result),
         .length = result == X_ERROR_SUCCESS ? are_friends : 0});
    return X_ERROR_IO_PENDING;
  } else {
    REX_ASSERT_ALWAYS();
    return X_ERROR_INVALID_PARAMETER;
  }
}

u32 XamShowSigninUI_entry([[maybe_unused]] u32 unk, [[maybe_unused]] u32 unk_mask) {
  // XN_SYS_SIGNINCHANGED
  runtime::GetCurrent().GetNotifier().BroadcastNotification(0x0000000A, 1);
  // XN_SYS_UI with the UI off.
  runtime::GetCurrent().GetNotifier().BroadcastNotification(0x00000009, 0);
  return X_ERROR_SUCCESS;
}

struct X_ACHIEVEMENT_UNLOCK_TIME {
  be_u32 unk_0;
  be_u32 unk_4;
};

struct X_ACHIEVEMENT_DETAILS {
  be_u32 id;
  be_u32 label_ptr;
  be_u32 description_ptr;
  be_u32 unachieved_ptr;
  be_u32 image_id;
  be_u32 gamerscore;
  X_ACHIEVEMENT_UNLOCK_TIME unlock_time;
  be_u32 flags;

  static const size_t kStringBufferSize = 464;
};
REX_STATIC_ASSERT_SIZE(X_ACHIEVEMENT_DETAILS, 36);

class AchievementItems {
 public:
  struct AchievementDetails {
    u32 id;
    std::u16string label;
    std::u16string description;
    std::u16string unachieved;
    u32 image_id;
    u32 gamerscore;
    struct {
      u32 unk_0;
      u32 unk_4;
    } unlock_time;
    u32 flags;
  };

  AchievementItems(size_t items_per_enumerate, u32 flags)
      : items_per_enumerate_(items_per_enumerate), flags_(flags) {}

  void Append(AchievementDetails item) { items_.push_back(std::move(item)); }

  u32 Write(u32 buffer_ptr, u8* buffer_data, u32* written_count) {
    size_t count = std::min(items_.size() - current_item_, items_per_enumerate_);
    if (!count) {
      return X_ERROR_NO_MORE_FILES;
    }

    auto details = reinterpret_cast<X_ACHIEVEMENT_DETAILS*>(buffer_data);
    size_t string_offset = items_per_enumerate_ * sizeof(X_ACHIEVEMENT_DETAILS);
    auto string_buffer =
        StringBuffer{buffer_ptr + static_cast<u32>(string_offset), &buffer_data[string_offset],
                     count * X_ACHIEVEMENT_DETAILS::kStringBufferSize};
    for (size_t i = 0; i < count; ++i, ++current_item_) {
      const auto& item = items_[current_item_];
      details[i].id = item.id;
      details[i].label_ptr = !!(flags_ & 1) ? AppendString(string_buffer, item.label) : 0;
      details[i].description_ptr =
          !!(flags_ & 2) ? AppendString(string_buffer, item.description) : 0;
      details[i].unachieved_ptr = !!(flags_ & 4) ? AppendString(string_buffer, item.unachieved) : 0;
      details[i].image_id = item.image_id;
      details[i].gamerscore = item.gamerscore;
      details[i].unlock_time.unk_0 = item.unlock_time.unk_0;
      details[i].unlock_time.unk_4 = item.unlock_time.unk_4;
      details[i].flags = item.flags;
    }

    if (written_count) {
      *written_count = static_cast<u32>(count);
    }

    return X_ERROR_SUCCESS;
  }

 private:
  struct StringBuffer {
    u32 ptr;
    u8* data;
    size_t remaining_bytes;
  };

  u32 AppendString(StringBuffer& sb, const std::u16string_view string) {
    size_t count = string.length() + 1;
    size_t size = count * sizeof(char16_t);
    if (size > sb.remaining_bytes) {
      REX_ASSERT_ALWAYS();
      return 0;
    }
    auto ptr = sb.ptr;
    rex::CopyAndSwapTruncating(reinterpret_cast<char16_t*>(sb.data), string, count);
    sb.ptr += static_cast<u32>(size);
    sb.data += size;
    sb.remaining_bytes -= size;
    return ptr;
  }

  size_t items_per_enumerate_;
  u32 flags_;
  std::vector<AchievementDetails> items_;
  size_t current_item_ = 0;
};

u32 XamUserCreateAchievementEnumerator_entry([[maybe_unused]] u32 title_id, u32 user_index,
                                             [[maybe_unused]] u32 xuid, u32 flags,
                                             [[maybe_unused]] u32 offset, u32 count,
                                             MappedU32 buffer_size_ptr, MappedU32 handle_ptr) {
  if (!count || !buffer_size_ptr || !handle_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }

  if (user_index >= 4) {
    return X_ERROR_INVALID_PARAMETER;
  }

  size_t entry_size = sizeof(X_ACHIEVEMENT_DETAILS);
  if (flags & 7) {
    entry_size += X_ACHIEVEMENT_DETAILS::kStringBufferSize;
  }

  if (buffer_size_ptr) {
    *buffer_size_ptr = static_cast<u32>(entry_size) * count;
  }

  AchievementItems items(count, flags);

  // ACHIEVED | ACHIEVED_ONLINE flags titles test to treat an achievement as earned.
  constexpr u32 kAchievedFlags = 0x00030000;

  auto fill_unlock = [](AchievementItems::AchievementDetails& item, u32 id, Runtime& runtime) {
    u64 ft = runtime.GetAchievementManager().GetUnlockTime(id).value_or(0);
    if (ft) {
      item.flags |= kAchievedFlags;
      item.unlock_time.unk_0 = static_cast<u32>(ft & 0xFFFF'FFFF);
      item.unlock_time.unk_4 = static_cast<u32>(ft >> 32);
    }
  };

  auto& runtime = runtime::GetCurrent();

  const auto achievements = runtime.GetAchievementManager().ListAchievements();
  if (!achievements.empty()) {
    for (const auto& info : achievements) {
      auto item = AchievementItems::AchievementDetails{info.id,
                                                       rex::ToUTF16(info.label),
                                                       rex::ToUTF16(info.description),
                                                       rex::ToUTF16(info.unachieved_description),
                                                       info.image_id,
                                                       info.gamerscore,
                                                       {0, 0},
                                                       info.flags};
      fill_unlock(item, info.id, runtime);
      items.Append(item);
    }
  } else {
    const runtime::XDBFTitleData db = runtime.GetContentManager().GetTitleXDBF();
    if (db.IsValid()) {
      const XLanguage language = db.GetSupportedLanguage(
          ToXLanguage(runtime::GetCurrent().GetConfig().runtime.kernel.user_language));
      for (const runtime::XDBFAchievementTableEntry& entry : db.GetAchievements()) {
        auto item = AchievementItems::AchievementDetails{
            entry.id,
            rex::ToUTF16(db.GetStringTableEntry(language, entry.label_id)),
            rex::ToUTF16(db.GetStringTableEntry(language, entry.description_id)),
            rex::ToUTF16(db.GetStringTableEntry(language, entry.unachieved_id)),
            entry.image_id,
            entry.gamerscore,
            {0, 0},
            entry.flags};
        fill_unlock(item, entry.id, runtime);
        items.Append(item);
      }
    }
  }

  X_HANDLE handle = kXInvalidHandleValue;
  auto result = CreateEnumerator(
      runtime::GetCurrent(), &handle, EnumeratorInit{user_index, 0xFB, 0xB000A, 0xB000B, 0},
      EnumeratorObject(
          count, entry_size,
          [items = std::move(items)](u32 buffer_ptr, u8* buffer_data, u32* written_count) mutable {
            return items.Write(buffer_ptr, buffer_data, written_count);
          }));
  if (result == X_STATUS_NO_MEMORY) {
    return X_ERROR_NOT_ENOUGH_MEMORY;
  }
  if (XFAILED(result)) {
    return result;
  }

  *handle_ptr = handle;
  return X_ERROR_SUCCESS;
}

namespace {

constexpr u32 kStatsMaxRows = 100;
constexpr u32 kStatsMaxSpecs = 10;
constexpr u32 kStatsAttrsInSpec = 64;
constexpr u32 kStatsSpecSize = 8 + kStatsAttrsInSpec * 2;
constexpr u32 kStatsResultsSize = 8;
constexpr u32 kStatsViewSize = 16;
constexpr u32 kStatsRowSize = 48;
constexpr u32 kStatsColumnSize = 24;

}  // namespace

u32 XamUserCreateStatsEnumerator_entry([[maybe_unused]] u32 title_id, [[maybe_unused]] u32 by_rank,
                                       [[maybe_unused]] u64 pivot, u32 row_count, u32 spec_count,
                                       MappedVoid specs_ptr, MappedU32 buffer_size_ptr,
                                       MappedU32 handle_ptr) {
  if (!row_count || row_count > kStatsMaxRows || !spec_count || spec_count > kStatsMaxSpecs ||
      !specs_ptr || !buffer_size_ptr || !handle_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }

  const u8* specs = specs_ptr.As<const u8*>();
  std::vector<u32> view_ids;
  u64 buffer_size = kStatsResultsSize;
  for (u32 i = 0; i < spec_count; ++i) {
    const u8* spec = specs + static_cast<size_t>(i) * kStatsSpecSize;
    const u32 column_count = *reinterpret_cast<const be_u32*>(spec + 4);
    if (column_count > kStatsAttrsInSpec) {
      return X_ERROR_INVALID_PARAMETER;
    }
    view_ids.push_back(*reinterpret_cast<const be_u32*>(spec));
    buffer_size += kStatsViewSize +
                   static_cast<u64>(row_count) * (kStatsRowSize + column_count * kStatsColumnSize);
  }

  EnumeratorObject stats(
      1, static_cast<size_t>(buffer_size),
      [view_ids = std::move(view_ids), delivered = false](u32 buffer_ptr, u8* buffer_data,
                                                          u32* written_count) mutable -> u32 {
        if (delivered) {
          return X_ERROR_NO_MORE_FILES;
        }
        delivered = true;
        const u32 view_count = static_cast<u32>(view_ids.size());
        std::memset(buffer_data, 0, kStatsResultsSize + view_count * kStatsViewSize);
        *reinterpret_cast<be_u32*>(buffer_data) = view_count;
        *reinterpret_cast<be_u32*>(buffer_data + 4) = buffer_ptr + kStatsResultsSize;
        for (u32 i = 0; i < view_count; ++i) {
          *reinterpret_cast<be_u32*>(buffer_data + kStatsResultsSize +
                                     static_cast<size_t>(i) * kStatsViewSize) = view_ids[i];
        }
        if (written_count) {
          *written_count = view_count;
        }
        return X_ERROR_SUCCESS;
      });
  stats.results_in_buffer = true;

  X_HANDLE handle = kXInvalidHandleValue;
  auto result = CreateEnumerator(runtime::GetCurrent(), &handle, EnumeratorInit{0xFF, 0, 0, 0, 0},
                                 std::move(stats));
  if (result == X_STATUS_NO_MEMORY) {
    return X_ERROR_NOT_ENOUGH_MEMORY;
  }
  if (XFAILED(result)) {
    return result;
  }

  *buffer_size_ptr = static_cast<u32>(buffer_size);
  *handle_ptr = handle;
  return X_ERROR_SUCCESS;
}

u32 XamParseGamerTileKey_entry([[maybe_unused]] MappedU32 key_ptr, MappedU32 out1_ptr,
                               MappedU32 out2_ptr, MappedU32 out3_ptr) {
  *out1_ptr = 0xC0DE0001;
  *out2_ptr = 0xC0DE0002;
  *out3_ptr = 0xC0DE0003;
  return X_ERROR_SUCCESS;
}

u32 XamReadTileToTexture_entry([[maybe_unused]] u32 unknown, [[maybe_unused]] u32 title_id,
                               u64 tile_id, [[maybe_unused]] u32 user_index, MappedVoid buffer_ptr,
                               u32 stride, u32 height, u32 overlapped_ptr) {
  if (!tile_id) {
    return X_ERROR_INVALID_PARAMETER;
  }

  size_t size = static_cast<size_t>(stride) * static_cast<size_t>(height);
  std::memset(buffer_ptr, 0xFF, size);

  if (overlapped_ptr) {
    runtime::GetCurrent().GetDispatcher().Complete(overlapped_ptr, {.result = X_ERROR_SUCCESS});
    return X_ERROR_IO_PENDING;
  }
  return X_ERROR_SUCCESS;
}

u32 XamWriteGamerTile_entry([[maybe_unused]] u32 arg1, [[maybe_unused]] u32 arg2,
                            [[maybe_unused]] u32 arg3, [[maybe_unused]] u32 arg4,
                            [[maybe_unused]] u32 arg5, u32 overlapped_ptr) {
  if (overlapped_ptr) {
    runtime::GetCurrent().GetDispatcher().Complete(overlapped_ptr, {.result = X_ERROR_SUCCESS});
    return X_ERROR_IO_PENDING;
  }
  return X_ERROR_SUCCESS;
}

u32 XamSessionCreateHandle_entry(MappedU32 handle_ptr) {
  auto& runtime = runtime::GetCurrent();
  ObjectBlock block = AllocObjectBlock(runtime.GetMemory(), sizeof(X_DISPATCH_HEADER));
  if (!block.address) {
    return X_ERROR_NOT_ENOUGH_MEMORY;
  }
  Result<X_HANDLE> added = runtime.GetObjectTable().Emplace<SessionObject>(AddOptions{{}, block});
  if (!added) {
    return X_ERROR_NOT_ENOUGH_MEMORY;
  }
  StashHandle(runtime.GetMemory()->TranslateVirtual<X_DISPATCH_HEADER*>(block.address), *added);
  *handle_ptr = *added;
  return X_ERROR_SUCCESS;
}

u32 XamSessionRefObjByHandle_entry(u32 handle, MappedU32 obj_ptr) {
  auto& table = runtime::GetCurrent().GetObjectTable();
  auto object = table.Get(handle);
  if (!object || object.GetKind() != ObjectKind::kSession) {
    return X_ERROR_INVALID_HANDLE;
  }
  auto referenced = kernel::ReferencePrimary(table, object, object.GetGuestAddress());
  if (!referenced) {
    return X_ERROR_INVALID_HANDLE;
  }
  *obj_ptr = *referenced;
  return X_ERROR_SUCCESS;
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__XamUserGetXUID, rex::kernel::xam::XamUserGetXUID_entry)
REX_EXPORT(__imp__XamUserGetSigninState, rex::kernel::xam::XamUserGetSigninState_entry)
REX_EXPORT(__imp__XamUserGetSigninInfo, rex::kernel::xam::XamUserGetSigninInfo_entry)
REX_EXPORT(__imp__XamUserGetName, rex::kernel::xam::XamUserGetName_entry)
REX_EXPORT(__imp__XamUserGetGamerTag, rex::kernel::xam::XamUserGetGamerTag_entry)
REX_EXPORT(__imp__XamUserReadProfileSettings, rex::kernel::xam::XamUserReadProfileSettings_entry)
REX_EXPORT(__imp__XamUserReadProfileSettingsEx,
           rex::kernel::xam::XamUserReadProfileSettingsEx_entry)
REX_EXPORT(__imp__XamUserWriteProfileSettings, rex::kernel::xam::XamUserWriteProfileSettings_entry)
REX_EXPORT(__imp__XamUserCheckPrivilege, rex::kernel::xam::XamUserCheckPrivilege_entry)
REX_EXPORT(__imp__XamUserContentRestrictionGetFlags,
           rex::kernel::xam::XamUserContentRestrictionGetFlags_entry)
REX_EXPORT(__imp__XamUserContentRestrictionGetRating,
           rex::kernel::xam::XamUserContentRestrictionGetRating_entry)
REX_EXPORT(__imp__XamUserContentRestrictionCheckAccess,
           rex::kernel::xam::XamUserContentRestrictionCheckAccess_entry)
REX_EXPORT(__imp__XamUserIsOnlineEnabled, rex::kernel::xam::XamUserIsOnlineEnabled_entry)
REX_EXPORT(__imp__XamUserGetMembershipTier, rex::kernel::xam::XamUserGetMembershipTier_entry)
REX_EXPORT(__imp__XamUserAreUsersFriends, rex::kernel::xam::XamUserAreUsersFriends_entry)
REX_EXPORT(__imp__XamShowSigninUI, rex::kernel::xam::XamShowSigninUI_entry)
REX_EXPORT(__imp__XamUserCreateAchievementEnumerator,
           rex::kernel::xam::XamUserCreateAchievementEnumerator_entry)
REX_EXPORT(__imp__XamUserCreateStatsEnumerator,
           rex::kernel::xam::XamUserCreateStatsEnumerator_entry)
REX_EXPORT(__imp__XamParseGamerTileKey, rex::kernel::xam::XamParseGamerTileKey_entry)
REX_EXPORT(__imp__XamReadTileToTexture, rex::kernel::xam::XamReadTileToTexture_entry)
REX_EXPORT(__imp__XamWriteGamerTile, rex::kernel::xam::XamWriteGamerTile_entry)
REX_EXPORT(__imp__XamSessionCreateHandle, rex::kernel::xam::XamSessionCreateHandle_entry)
REX_EXPORT(__imp__XamSessionRefObjByHandle, rex::kernel::xam::XamSessionRefObjByHandle_entry)

REX_EXPORT_STUB(__imp__XamUserAddRecentPlayer);
REX_EXPORT_STUB(__imp__XamUserAllowedToPostToSocialNetwork);
REX_EXPORT_STUB(__imp__XamUserCreateAvatarAssetEnumerator);
REX_EXPORT_STUB(__imp__XamUserCreatePlayerEnumerator);
REX_EXPORT_STUB(__imp__XamUserCreateTitlesPlayedEnumerator);
REX_EXPORT_STUB(__imp__XamUserFlushLogonQueue);
REX_EXPORT_STUB(__imp__XamUserGetAge);
REX_EXPORT_STUB(__imp__XamUserGetAgeGroup);
REX_EXPORT_STUB(__imp__XamUserGetCachedUserFlags);
REX_EXPORT_STUB(__imp__XamUserGetDeviceId);
REX_EXPORT_STUB(__imp__XamUserGetIndexFromXUID);
REX_EXPORT_STUB(__imp__XamUserGetMembershipTierFromXUID);
REX_EXPORT_STUB(__imp__XamUserGetOnlineCountryFromXUID);
REX_EXPORT_STUB(__imp__XamUserGetOnlineLanguageFromXUID);
REX_EXPORT_STUB(__imp__XamUserGetOnlineXUIDFromOfflineXUID);
REX_EXPORT_STUB(__imp__XamUserGetReportingInfo);
REX_EXPORT_STUB(__imp__XamUserGetRequestedUserIndexMask);
REX_EXPORT_STUB(__imp__XamUserGetSubscriptionType);
REX_EXPORT_STUB(__imp__XamUserGetUserFlags);
REX_EXPORT_STUB(__imp__XamUserGetUserFlagsFromXUID);
REX_EXPORT_STUB(__imp__XamUserGetUserIndexMask);
REX_EXPORT_STUB(__imp__XamUserGetUserTenure);
REX_EXPORT_STUB(__imp__XamUserGetUsersMissingAvatars);
REX_EXPORT_STUB(__imp__XamUserGetXUIDForTFA);
REX_EXPORT_STUB(__imp__XamUserInvalidateProfileSetting);
REX_EXPORT_STUB(__imp__XamUserIsGuest);
REX_EXPORT_STUB(__imp__XamUserIsLogonPreviewModeEnabled);
REX_EXPORT_STUB(__imp__XamUserIsParentalControlled);
REX_EXPORT_STUB(__imp__XamUserIsPartial);
REX_EXPORT_STUB(__imp__XamUserIsPartialProfile);
REX_EXPORT_STUB(__imp__XamUserIsUnsafeProgrammingAllowed);
REX_EXPORT_STUB(__imp__XamUserLockLogonPreviewMode);
REX_EXPORT_STUB(__imp__XamUserLogon);
REX_EXPORT_STUB(__imp__XamUserLogonEx);
REX_EXPORT_STUB(__imp__XamUserLookupDevice);
REX_EXPORT_STUB(__imp__XamUserNuiBind);
REX_EXPORT_STUB(__imp__XamUserNuiEnableBiometric);
REX_EXPORT_STUB(__imp__XamUserNuiGetEnrollmentIndex);
REX_EXPORT_STUB(__imp__XamUserNuiGetUserIndex);
REX_EXPORT_STUB(__imp__XamUserNuiGetUserIndexForBind);
REX_EXPORT_STUB(__imp__XamUserNuiGetUserIndexForSignin);
REX_EXPORT_STUB(__imp__XamUserNuiIsBiometricEnabled);
REX_EXPORT_STUB(__imp__XamUserNuiUnbind);
REX_EXPORT_STUB(__imp__XamUserOverrideBindingCallbacks);
REX_EXPORT_STUB(__imp__XamUserOverrideDeviceBindings);
REX_EXPORT_STUB(__imp__XamUserOverrideGlobalState);
REX_EXPORT_STUB(__imp__XamUserOverrideUserInfo);
REX_EXPORT_STUB(__imp__XamUserPrefetchProfileSettings);
REX_EXPORT_STUB(__imp__XamUserProfileSync);
REX_EXPORT_STUB(__imp__XamUserReadUserPreference);
REX_EXPORT_STUB(__imp__XamUserResetSubscriptionType);
REX_EXPORT_STUB(__imp__XamUserUnlockLogonPreviewMode);
REX_EXPORT_STUB(__imp__XamUserUpdateRecentPlayer);
REX_EXPORT_STUB(__imp__XamUserValidateAvatarManifest);
REX_EXPORT_STUB(__imp__XamUserWriteUserPreference);
REX_EXPORT_STUB(__imp__XamVerifyPasscode);
