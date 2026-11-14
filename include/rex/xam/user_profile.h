/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @file        xam/user_profile.h
 * @brief       Profile settings and the signed in user profile
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 *
 * @remarks     Parts derived from Xenia (Ben Vanik, 2020).
 * @remarks     Parts derived from Xenia (Ben Vanik, 2022).
 */

#pragma once

#include <cstddef>
#include <memory>
#include <unordered_map>
#include <vector>
#include <format>
#include <string>
#include <filesystem>
#include <optional>
#include <string_view>
#include <functional>
#include <span>

#include <rex/platform.h>
#include <rex/types.h>
#include <rex/memory.h>
#include <rex/kernel/types/status.h>
#include <rex/memory/stream.h>
#include <rex/kernel/types/content.h>
#include <rex/result.h>
#include <rex/filesystem/xdbf.h>
#include <rex/utility.h>
#include <rex/title.h>

namespace rex::xam {

class IContentManager;

/** Guest ABI value of a profile setting, laid out as XUSER_DATA. */
struct X_USER_PROFILE_SETTING_DATA {
  /**
   * UserProfile::Setting::Type, the XUSER_DATA type byte. Titles do not always zero the 3 bytes
   * after it.
   */
  u8 type;
  u8 unk_1[3];  /**< Padding after the type byte. */
  be_u32 unk_4; /**< Padding before the 8 byte aligned value union. */
  /** Union of the XUSER_DATA value types. */
  union {
    be_i32 s32; /**< XUSER_DATA_TYPE_INT32 value. */
    be_i64 s64; /**< XUSER_DATA_TYPE_INT64 value. */
    be_u32 u32; /**< Unsigned view of the 32 bit value. */
    be_f64 f64; /**< XUSER_DATA_TYPE_DOUBLE value. */
    struct {
      be_u32 size; /**< Text size in bytes including the terminator. */
      be_u32 ptr;  /**< Guest address of the UTF-16 text, 0 when empty. */
    } unicode;     /**< XUSER_DATA_TYPE_UNICODE value. */
    be_f32 f32;    /**< XUSER_DATA_TYPE_FLOAT value. */
    struct {
      be_u32 size;   /**< Data size in bytes. */
      be_u32 ptr;    /**< Guest address of the data, 0 when empty. */
    } binary;        /**< XUSER_DATA_TYPE_BINARY value. */
    be_u64 filetime; /**< XUSER_DATA_TYPE_DATETIME value. */
  };
};
REX_STATIC_ASSERT_SIZE(X_USER_PROFILE_SETTING_DATA, 16);

/** Guest ABI entry of a profile settings read result, laid out as XUSER_PROFILE_SETTING. */
struct X_USER_PROFILE_SETTING {
  be_u32 from;   /**< Source of the value, an XUSER_PROFILE_SOURCE value. */
  be_u32 unk_04; /**< Padding before the 8 byte aligned union. */
  union {
    be_u32 user_index; /**< User index when the read named a user index. */
    be_u64 xuid;       /**< XUID when the read named XUIDs. */
  };
  be_u32 setting_id;
  be_u32 unk_14; /**< Padding before the 8 byte aligned data. */
  union {
    u8 data_bytes[sizeof(X_USER_PROFILE_SETTING_DATA)];
    X_USER_PROFILE_SETTING_DATA data;
  };
};
REX_STATIC_ASSERT_SIZE(X_USER_PROFILE_SETTING, 40);

class UserProfile {
 public:
  /** Byte stream that tracks the guest address of its buffer. */
  class SettingByteStream : public memory::ByteStream {
   public:
    SettingByteStream(u32 ptr, u8* data, size_t data_length, size_t offset = 0)
        : memory::ByteStream(data, data_length, offset), ptr_(ptr) {}

    /** Returns the guest address of the current offset. */
    u32 GetPtr() const { return static_cast<u32>(ptr_ + GetOffset()); }

   private:
    u32 ptr_;
  };
  /** A profile setting identified by ID, which can write itself into a guest value. */
  struct Setting {
    /** Value type of a setting, matching the XUSER_DATA type byte. */
    enum class Type : u8 {
      kContext = 0,
      kInt32 = 1,
      kInt64 = 2,
      kDouble = 3,
      kUnicode = 4,
      kFloat = 5,
      kBinary = 6,
      kDateTime = 7,
      kUnset = 0xFF,
    };
    union Key {
      u32 value;
      struct {
        u32 id : 14;
        u32 unk : 2;   /**< Bits 14 and 15 of the setting ID. */
        u32 size : 12; /**< Size in bytes of a string or binary value. */
        u32 type : 4;
      };
    };
    u32 setting_id;
    Type type;
    size_t size;
    bool is_set;
    u32 loaded_title_id; /**< ID of the title whose saved value was loaded, 0 when none. */
    /**
     * @param is_set True when the setting holds a value.
     */
    Setting(u32 setting_id, Type type, size_t size, bool is_set)
        : setting_id(setting_id), type(type), size(size), is_set(is_set), loaded_title_id(0) {}
    virtual ~Setting() = default;
    /**
     * Writes the setting into a guest value.
     *
     * @param data The guest value to fill.
     * @param stream The stream that receives variable length data.
     */
    virtual void WriteTo(X_USER_PROFILE_SETTING_DATA* data, SettingByteStream* stream) {
      (void)stream;
      data->type = static_cast<u8>(type);
    }
    /** Returns the bytes saved to disk for a title specific setting. */
    virtual std::vector<u8> Serialize() const { return std::vector<u8>(); }
    /** Restores the value from bytes saved to disk. */
    virtual void Deserialize(const std::vector<u8>&) {}
    /**
     * Reports whether the setting is saved per title.
     *
     * @return True when the ID is in the title specific range.
     */
    bool IsTitleSpecific() const { return (setting_id & 0x3F00) == 0x3F00; }
  };
  struct Int32Setting : public Setting {
    Int32Setting(u32 id, i32 value) : Setting(id, Type::kInt32, 4, true), value(value) {}
    i32 value;
    void WriteTo(X_USER_PROFILE_SETTING_DATA* data, SettingByteStream* stream) override {
      Setting::WriteTo(data, stream);
      data->s32 = value;
    }
  };
  struct Int64Setting : public Setting {
    Int64Setting(u32 id, i64 value) : Setting(id, Type::kInt64, 8, true), value(value) {}
    i64 value;
    void WriteTo(X_USER_PROFILE_SETTING_DATA* data, SettingByteStream* stream) override {
      Setting::WriteTo(data, stream);
      data->s64 = value;
    }
  };
  struct DoubleSetting : public Setting {
    DoubleSetting(u32 id, f64 value) : Setting(id, Type::kDouble, 8, true), value(value) {}
    f64 value;
    void WriteTo(X_USER_PROFILE_SETTING_DATA* data, SettingByteStream* stream) override {
      Setting::WriteTo(data, stream);
      data->f64 = value;
    }
  };
  struct UnicodeSetting : public Setting {
    UnicodeSetting(u32 id, const std::u16string& value)
        : Setting(id, Type::kUnicode, 8, true), value(value) {}
    std::u16string value;
    void WriteTo(X_USER_PROFILE_SETTING_DATA* data, SettingByteStream* stream) override {
      Setting::WriteTo(data, stream);
      if (value.empty()) {
        data->unicode.size = 0;
        data->unicode.ptr = 0;
      } else {
        size_t count = value.size() + 1;
        size_t size = 2 * count;
        REX_ASSERT(size <= std::numeric_limits<u32>::max());
        data->unicode.size = static_cast<u32>(size);
        data->unicode.ptr = stream->GetPtr();
        auto buffer = reinterpret_cast<u16*>(&stream->GetData()[stream->GetOffset()]);
        stream->Advance(size);
        memory::copy_and_swap(buffer, reinterpret_cast<const u16*>(value.data()), count);
      }
    }
  };
  struct FloatSetting : public Setting {
    FloatSetting(u32 id, f32 value) : Setting(id, Type::kFloat, 4, true), value(value) {}
    f32 value;
    void WriteTo(X_USER_PROFILE_SETTING_DATA* data, SettingByteStream* stream) override {
      Setting::WriteTo(data, stream);
      data->f32 = value;
    }
  };
  struct BinarySetting : public Setting {
    BinarySetting(u32 id) : Setting(id, Type::kBinary, 8, false) {}
    BinarySetting(u32 id, const std::vector<u8>& value)
        : Setting(id, Type::kBinary, 8, true), value(value) {}
    std::vector<u8> value;
    void WriteTo(X_USER_PROFILE_SETTING_DATA* data, SettingByteStream* stream) override {
      Setting::WriteTo(data, stream);
      if (value.empty()) {
        data->binary.size = 0;
        data->binary.ptr = 0;
      } else {
        size_t size = value.size();
        REX_ASSERT(size <= std::numeric_limits<u32>::max());
        data->binary.size = static_cast<u32>(size);
        data->binary.ptr = stream->GetPtr();
        stream->Write(value.data(), size);
      }
    }
    std::vector<u8> Serialize() const override {
      return std::vector<u8>(value.data(), value.data() + value.size());
    }
    void Deserialize(const std::vector<u8>& data) override {
      value = data;
      is_set = true;
    }
  };
  struct DateTimeSetting : public Setting {
    DateTimeSetting(u32 id, i64 value) : Setting(id, Type::kDateTime, 8, true), value(value) {}
    i64 value;
    void WriteTo(X_USER_PROFILE_SETTING_DATA* data, SettingByteStream* stream) override {
      Setting::WriteTo(data, stream);
      data->filetime = value;
    }
  };

  REX_XAM_API UserProfile();

  /**
   * Sets the content manager that locates the title specific settings.
   *
   * @param content The content manager, which outlives the profile.
   * @param title_id Returns the title ID of the executable.
   */
  void SetContentManager(IContentManager& content, rex::MoveOnlyFunction<u32()> title_id) {
    content_ = &content;
    title_id_ = std::move(title_id);
  }

  u64 GetXUID() const { return xuid_; }

  std::string GetName() const { return name_; }

  /**
   * @return Always 1.
   */
  u32 GetSigninState() const { return 1; }

  /**
   * Returns the profile type bits that the XUID query matches against its type mask. It is always
   * 3.
   */
  u32 GetType() const { return 1 | 2; }

  /**
   * Adds a setting, replacing any with the same ID. A title specific setting is saved to disk when
   * added.
   *
   * @param setting The setting, owned by the profile afterwards.
   */
  REX_XAM_API void AddSetting(std::unique_ptr<Setting> setting);
  /**
   * Returns a setting, loading a title specific one from disk if the title has not loaded it, null
   * when the ID is unknown.
   */
  REX_XAM_API Setting* GetSetting(u32 setting_id);

 private:
  u64 xuid_;
  std::string name_;
  std::vector<std::unique_ptr<Setting>> setting_list_;
  std::unordered_map<u32, Setting*> settings_;
  IContentManager* content_ = nullptr;
  rex::MoveOnlyFunction<u32()> title_id_;

  REX_XAM_API void LoadSetting(UserProfile::Setting*);
  REX_XAM_API void SaveSetting(UserProfile::Setting*);
};

}  // namespace rex::xam

/** Formats the enum as its underlying integer. */
template <>
struct std::formatter<rex::xam::UserProfile::Setting::Type> : std::formatter<rex::i32> {
  auto format(rex::xam::UserProfile::Setting::Type value, std::format_context& ctx) const {
    return std::formatter<rex::i32>::format(static_cast<rex::i32>(value), ctx);
  }
};
