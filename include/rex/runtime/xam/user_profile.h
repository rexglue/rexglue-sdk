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

#pragma once

#include <format>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/platform.h>
#include <rex/types.h>
#include <rex/export.h>
#include <rex/memory.h>
#include <rex/stream.h>
#include <rex/runtime/xtypes.h>

namespace rex {
class Runtime;
}  // namespace rex

namespace rex {
namespace runtime {

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
  be_u32 from;  /**< Source of the value, an XUSER_PROFILE_SOURCE value. */
  be_u32 unk04; /**< Padding before the 8 byte aligned union. */
  union {
    be_u32 user_index; /**< User index when the read named a user index. */
    be_u64 xuid;       /**< XUID when the read named XUIDs. */
  };
  be_u32 setting_id; /**< Setting id. */
  be_u32 unk14;      /**< Padding before the 8 byte aligned data. */
  union {
    u8 data_bytes[sizeof(X_USER_PROFILE_SETTING_DATA)]; /**< Raw bytes. */
    X_USER_PROFILE_SETTING_DATA data;                   /**< Value. */
  };
};
REX_STATIC_ASSERT_SIZE(X_USER_PROFILE_SETTING, 40);

/** Local profile of the signed in user, holding its settings. */
class UserProfile {
 public:
  /** Byte stream that tracks the guest address of its buffer. */
  class SettingByteStream : public ByteStream {
   public:
    /**
     * Constructs the stream over a buffer.
     *
     * @param ptr The guest address of the buffer.
     * @param data The host buffer.
     * @param data_length The buffer length in bytes.
     * @param offset The initial offset in bytes.
     */
    SettingByteStream(u32 ptr, u8* data, size_t data_length, size_t offset = 0)
        : ByteStream(data, data_length, offset), ptr_(ptr) {}

    /**
     * Returns the guest address of the current offset.
     *
     * @return The guest address.
     */
    u32 GetPtr() const { return static_cast<u32>(ptr_ + GetOffset()); }

   private:
    u32 ptr_;
  };
  /** A profile setting identified by id, which can write itself into a guest value. */
  struct Setting {
    /** Value type of a setting, matching the XUSER_DATA type byte. */
    enum class Type : u8 {
      kContent = 0,  /**< Context value. */
      kInt32 = 1,    /**< 32 bit integer. */
      kInt64 = 2,    /**< 64 bit integer. */
      kDouble = 3,   /**< 64 bit floating point. */
      kWString = 4,  /**< UTF-16 string. */
      kFloat = 5,    /**< 32 bit floating point. */
      kBinary = 6,   /**< Binary data. */
      kDateTime = 7, /**< Date and time. */
      kUnset = 0xFF, /**< No value. */
    };
    /** Fields packed into a setting id. */
    union Key {
      u32 value; /**< The raw setting id. */
      struct {
        u32 id : 14;   /**< Setting number. */
        u32 unk : 2;   /**< Bits 14 and 15 of the setting id. */
        u32 size : 12; /**< Size in bytes of a string or binary value. */
        u32 type : 4;  /**< Type value. */
      };
    };
    u32 setting_id;      /**< Id of the setting. */
    Type type;           /**< Value type. */
    size_t size;         /**< Value size in bytes. */
    bool is_set;         /**< True when the setting holds a value. */
    u32 loaded_title_id; /**< Id of the title whose saved value was loaded, 0 when none. */
    /**
     * Constructs a setting.
     *
     * @param setting_id The id of the setting.
     * @param type The value type.
     * @param size The value size in bytes.
     * @param is_set True when the setting holds a value.
     */
    Setting(u32 setting_id, Type type, size_t size, bool is_set)
        : setting_id(setting_id), type(type), size(size), is_set(is_set), loaded_title_id(0) {}
    /** Destroys the setting through its base. */
    virtual ~Setting() = default;
    /**
     * Writes the setting into a guest value.
     *
     * @param data The guest value to fill.
     * @param stream The stream that receives variable length data.
     */
    virtual void Append(X_USER_PROFILE_SETTING_DATA* data, SettingByteStream* stream) {
      (void)stream;
      data->type = static_cast<u8>(type);
    }
    /**
     * Returns the bytes saved to disk for a title specific setting.
     *
     * @return The bytes.
     */
    virtual std::vector<u8> Serialize() const { return std::vector<u8>(); }
    /**
     * Restores the value from bytes saved to disk.
     *
     * @param data The saved bytes.
     */
    virtual void Deserialize(const std::vector<u8>&) {}
    /**
     * Reports whether the setting is saved per title.
     *
     * @return True when the id is in the title specific range.
     */
    bool IsTitleSpecific() const { return (setting_id & 0x3F00) == 0x3F00; }
  };
  /** Profile setting holding a 32 bit integer. */
  struct Int32Setting : public Setting {
    /**
     * Constructs the setting.
     *
     * @param id The id of the setting.
     * @param value The integer.
     */
    Int32Setting(u32 id, i32 value) : Setting(id, Type::kInt32, 4, true), value(value) {}
    i32 value; /**< The integer. */
    /**
     * Writes the integer into the guest value.
     *
     * @param data The guest value to fill.
     * @param stream The stream that receives variable length data.
     */
    void Append(X_USER_PROFILE_SETTING_DATA* data, SettingByteStream* stream) override {
      Setting::Append(data, stream);
      data->s32 = value;
    }
  };
  /** Profile setting holding a 64 bit integer. */
  struct Int64Setting : public Setting {
    /**
     * Constructs the setting.
     *
     * @param id The id of the setting.
     * @param value The integer.
     */
    Int64Setting(u32 id, i64 value) : Setting(id, Type::kInt64, 8, true), value(value) {}
    i64 value; /**< The integer. */
    /**
     * Writes the integer into the guest value.
     *
     * @param data The guest value to fill.
     * @param stream The stream that receives variable length data.
     */
    void Append(X_USER_PROFILE_SETTING_DATA* data, SettingByteStream* stream) override {
      Setting::Append(data, stream);
      data->s64 = value;
    }
  };
  /** Profile setting holding a 64 bit floating point value. */
  struct DoubleSetting : public Setting {
    /**
     * Constructs the setting.
     *
     * @param id The id of the setting.
     * @param value The floating point value.
     */
    DoubleSetting(u32 id, f64 value) : Setting(id, Type::kDouble, 8, true), value(value) {}
    f64 value; /**< The floating point value. */
    /**
     * Writes the floating point value into the guest value.
     *
     * @param data The guest value to fill.
     * @param stream The stream that receives variable length data.
     */
    void Append(X_USER_PROFILE_SETTING_DATA* data, SettingByteStream* stream) override {
      Setting::Append(data, stream);
      data->f64 = value;
    }
  };
  /** Profile setting holding a UTF-16 string. */
  struct UnicodeSetting : public Setting {
    /**
     * Constructs the setting.
     *
     * @param id The id of the setting.
     * @param value The text.
     */
    UnicodeSetting(u32 id, const std::u16string& value)
        : Setting(id, Type::kWString, 8, true), value(value) {}
    std::u16string value; /**< The text. */
    /**
     * Writes the text after the stream offset and points the guest value at it.
     *
     * @param data The guest value to fill.
     * @param stream The stream that receives variable length data.
     */
    void Append(X_USER_PROFILE_SETTING_DATA* data, SettingByteStream* stream) override {
      Setting::Append(data, stream);
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
  /** Profile setting holding a 32 bit floating point value. */
  struct FloatSetting : public Setting {
    /**
     * Constructs the setting.
     *
     * @param id The id of the setting.
     * @param value The floating point value.
     */
    FloatSetting(u32 id, f32 value) : Setting(id, Type::kFloat, 4, true), value(value) {}
    f32 value; /**< The floating point value. */
    /**
     * Writes the floating point value into the guest value.
     *
     * @param data The guest value to fill.
     * @param stream The stream that receives variable length data.
     */
    void Append(X_USER_PROFILE_SETTING_DATA* data, SettingByteStream* stream) override {
      Setting::Append(data, stream);
      data->f32 = value;
    }
  };
  /** Profile setting holding binary data. */
  struct BinarySetting : public Setting {
    /**
     * Constructs a setting with no value.
     *
     * @param id The id of the setting.
     */
    BinarySetting(u32 id) : Setting(id, Type::kBinary, 8, false) {}
    /**
     * Constructs a setting holding data.
     *
     * @param id The id of the setting.
     * @param value The data.
     */
    BinarySetting(u32 id, const std::vector<u8>& value)
        : Setting(id, Type::kBinary, 8, true), value(value) {}
    std::vector<u8> value; /**< The data. */
    /**
     * Writes the data after the stream offset and points the guest value at it.
     *
     * @param data The guest value to fill.
     * @param stream The stream that receives variable length data.
     */
    void Append(X_USER_PROFILE_SETTING_DATA* data, SettingByteStream* stream) override {
      Setting::Append(data, stream);
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
    /**
     * Returns the bytes saved to disk.
     *
     * @return The data.
     */
    std::vector<u8> Serialize() const override {
      return std::vector<u8>(value.data(), value.data() + value.size());
    }
    /**
     * Replaces the data with bytes read from disk and marks the setting set.
     *
     * @param data The saved bytes.
     */
    void Deserialize(const std::vector<u8>& data) override {
      value = data;
      is_set = true;
    }
  };
  /** Profile setting holding a FILETIME timestamp. */
  struct DateTimeSetting : public Setting {
    /**
     * Constructs the setting.
     *
     * @param id The id of the setting.
     * @param value The timestamp.
     */
    DateTimeSetting(u32 id, i64 value) : Setting(id, Type::kDateTime, 8, true), value(value) {}
    i64 value; /**< The timestamp. */
    /**
     * Writes the timestamp into the guest value.
     *
     * @param data The guest value to fill.
     * @param stream The stream that receives variable length data.
     */
    void Append(X_USER_PROFILE_SETTING_DATA* data, SettingByteStream* stream) override {
      Setting::Append(data, stream);
      data->filetime = value;
    }
  };

  /**
   * @param runtime The runtime whose content manager holds the profile settings.
   */
  REX_RUNTIME_API explicit UserProfile(Runtime& runtime);

  u64 GetXUID() const { return xuid_; }
  std::string GetName() const { return name_; }
  u32 GetSigninState() const { return 1; }
  u32 GetType() const { return 1 | 2; /* local | online profile? */ }

  /**
   * Adds a setting, replacing any setting with the same id.
   * A set title specific setting is saved to disk.
   *
   * @param setting The setting, owned by the profile afterwards.
   */
  REX_RUNTIME_API void AddSetting(std::unique_ptr<Setting> setting);
  /**
   * Returns a setting, loading a title specific one from disk
   * when the current title has not loaded it.
   *
   * @param setting_id The id of the setting.
   * @return The setting, null when the id is unknown.
   */
  REX_RUNTIME_API Setting* GetSetting(u32 setting_id);

 private:
  u64 xuid_;
  std::string name_;
  std::vector<std::unique_ptr<Setting>> setting_list_;
  std::unordered_map<u32, Setting*> settings_;
  Runtime& runtime_;

  REX_RUNTIME_API void LoadSetting(UserProfile::Setting*);
  REX_RUNTIME_API void SaveSetting(UserProfile::Setting*);
};

}  // namespace runtime
}  // namespace rex

/** Formats the enum as its underlying integer. */
template <>
struct std::formatter<rex::runtime::UserProfile::Setting::Type> : std::formatter<rex::i32> {
  /**
   * Formats the value.
   *
   * @param value The value.
   * @param ctx The format context.
   * @return The output iterator.
   */
  auto format(rex::runtime::UserProfile::Setting::Type value, std::format_context& ctx) const {
    return std::formatter<rex::i32>::format(static_cast<rex::i32>(value), ctx);
  }
};
