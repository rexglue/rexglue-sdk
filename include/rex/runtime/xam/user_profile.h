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

#include <rex/macros.h>
#include <rex/memory.h>
#include <rex/stream.h>
#include <rex/runtime/xtypes.h>

namespace rex {
class Runtime;
}  // namespace rex

namespace rex {
namespace runtime {
namespace xam {

/** Guest ABI value of a profile setting, laid out as XUSER_DATA. */
struct X_USER_PROFILE_SETTING_DATA {
  /**
   * UserProfile::Setting::Type, the XUSER_DATA type byte. Titles do not always zero the 3 bytes
   * after it.
   */
  uint8_t type;
  uint8_t unk_1[3]; /**< Offset 0x01. Padding after the type byte. */
  be_u32 unk_4;     /**< Offset 0x04. Padding before the 8 byte aligned value union. */
  /** Union of the XUSER_DATA value types. */
  union {
    be_i32 s32; /**< Offset 0x08. XUSER_DATA_TYPE_INT32 value. */
    be_i64 s64; /**< Offset 0x08. XUSER_DATA_TYPE_INT64 value. */
    be_u32 u32; /**< Offset 0x08. Unsigned view of the 32 bit value. */
    be_f64 f64; /**< Offset 0x08. XUSER_DATA_TYPE_DOUBLE value. */
    struct {
      be_u32 size; /**< Offset 0x08. Text size in bytes including the terminator. */
      be_u32 ptr;  /**< Offset 0x0C. Guest address of the UTF-16 text, 0 when empty. */
    } unicode;     /**< XUSER_DATA_TYPE_UNICODE value. */
    be_f32 f32;    /**< Offset 0x08. XUSER_DATA_TYPE_FLOAT value. */
    struct {
      be_u32 size;   /**< Offset 0x08. Data size in bytes. */
      be_u32 ptr;    /**< Offset 0x0C. Guest address of the data, 0 when empty. */
    } binary;        /**< XUSER_DATA_TYPE_BINARY value. */
    be_u64 filetime; /**< Offset 0x08. XUSER_DATA_TYPE_DATETIME value. */
  };
};
REX_STATIC_ASSERT_SIZE(X_USER_PROFILE_SETTING_DATA, 16);

/** Guest ABI entry of a profile settings read result, laid out as XUSER_PROFILE_SETTING. */
struct X_USER_PROFILE_SETTING {
  be_u32 from;  /**< Offset 0x00. Source of the value, an XUSER_PROFILE_SOURCE value. */
  be_u32 unk04; /**< Offset 0x04. Padding before the 8 byte aligned union. */
  union {
    be_u32 user_index; /**< Offset 0x08. User index when the read named a user index. */
    be_u64 xuid;       /**< Offset 0x08. XUID when the read named XUIDs. */
  };
  be_u32 setting_id; /**< Offset 0x10. Setting id. */
  be_u32 unk14;      /**< Offset 0x14. Padding before the 8 byte aligned data. */
  union {
    uint8_t data_bytes[sizeof(X_USER_PROFILE_SETTING_DATA)]; /**< Offset 0x18. Raw bytes. */
    X_USER_PROFILE_SETTING_DATA data;                        /**< Offset 0x18. Value. */
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
    SettingByteStream(uint32_t ptr, uint8_t* data, size_t data_length, size_t offset = 0)
        : ByteStream(data, data_length, offset), ptr_(ptr) {}

    /**
     * Returns the guest address of the current offset.
     *
     * @return The guest address.
     */
    uint32_t ptr() const { return static_cast<uint32_t>(ptr_ + offset()); }

   private:
    uint32_t ptr_;
  };
  /** A profile setting identified by id, which can write itself into a guest value. */
  struct Setting {
    /** Value type of a setting, matching the XUSER_DATA type byte. */
    enum class Type {
      CONTENT = 0,  /**< Context value. */
      INT32 = 1,    /**< 32 bit integer. */
      INT64 = 2,    /**< 64 bit integer. */
      DOUBLE = 3,   /**< 64 bit floating point. */
      WSTRING = 4,  /**< UTF-16 string. */
      FLOAT = 5,    /**< 32 bit floating point. */
      BINARY = 6,   /**< Binary data. */
      DATETIME = 7, /**< Date and time. */
      UNSET = 0xFF, /**< No value. */
    };
    /** Fields packed into a setting id. */
    union Key {
      uint32_t value; /**< The raw setting id. */
      struct {
        uint32_t id : 14;   /**< Setting number. */
        uint32_t unk : 2;   /**< Bits 14 and 15 of the setting id. */
        uint32_t size : 12; /**< Size in bytes of a string or binary value. */
        uint32_t type : 4;  /**< Type value. */
      };
    };
    uint32_t setting_id;      /**< Id of the setting. */
    Type type;                /**< Value type. */
    size_t size;              /**< Value size in bytes. */
    bool is_set;              /**< True when the setting holds a value. */
    uint32_t loaded_title_id; /**< Id of the title whose saved value was loaded, 0 when none. */
    /**
     * Constructs a setting.
     *
     * @param setting_id The id of the setting.
     * @param type The value type.
     * @param size The value size in bytes.
     * @param is_set True when the setting holds a value.
     */
    Setting(uint32_t setting_id, Type type, size_t size, bool is_set)
        : setting_id(setting_id), type(type), size(size), is_set(is_set), loaded_title_id(0) {}
    /**
     * Writes the setting into a guest value.
     *
     * @param data The guest value to fill.
     * @param stream The stream that receives variable length data.
     */
    virtual void Append(X_USER_PROFILE_SETTING_DATA* data, SettingByteStream* stream) {
      (void)stream;
      data->type = static_cast<uint8_t>(type);
    }
    /**
     * Returns the bytes saved to disk for a title specific setting.
     *
     * @return The bytes.
     */
    virtual std::vector<uint8_t> Serialize() const { return std::vector<uint8_t>(); }
    /**
     * Restores the value from bytes saved to disk.
     *
     * @param data The saved bytes.
     */
    virtual void Deserialize(std::vector<uint8_t>) {}
    /**
     * Reports whether the setting is saved per title.
     *
     * @return True when the id is in the title specific range.
     */
    bool is_title_specific() const { return (setting_id & 0x3F00) == 0x3F00; }
  };
  /** Profile setting holding a 32 bit integer. */
  struct Int32Setting : public Setting {
    /**
     * Constructs the setting.
     *
     * @param setting_id The id of the setting.
     * @param value The integer.
     */
    Int32Setting(uint32_t setting_id, int32_t value)
        : Setting(setting_id, Type::INT32, 4, true), value(value) {}
    int32_t value; /**< The integer. */
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
     * @param setting_id The id of the setting.
     * @param value The integer.
     */
    Int64Setting(uint32_t setting_id, int64_t value)
        : Setting(setting_id, Type::INT64, 8, true), value(value) {}
    int64_t value; /**< The integer. */
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
     * @param setting_id The id of the setting.
     * @param value The floating point value.
     */
    DoubleSetting(uint32_t setting_id, double value)
        : Setting(setting_id, Type::DOUBLE, 8, true), value(value) {}
    double value; /**< The floating point value. */
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
     * @param setting_id The id of the setting.
     * @param value The text.
     */
    UnicodeSetting(uint32_t setting_id, const std::u16string& value)
        : Setting(setting_id, Type::WSTRING, 8, true), value(value) {}
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
        REX_ASSERT(size <= std::numeric_limits<uint32_t>::max());
        data->unicode.size = static_cast<uint32_t>(size);
        data->unicode.ptr = stream->ptr();
        auto buffer = reinterpret_cast<uint16_t*>(&stream->data()[stream->offset()]);
        stream->Advance(size);
        memory::copy_and_swap(buffer, reinterpret_cast<const uint16_t*>(value.data()), count);
      }
    }
  };
  /** Profile setting holding a 32 bit floating point value. */
  struct FloatSetting : public Setting {
    /**
     * Constructs the setting.
     *
     * @param setting_id The id of the setting.
     * @param value The floating point value.
     */
    FloatSetting(uint32_t setting_id, float value)
        : Setting(setting_id, Type::FLOAT, 4, true), value(value) {}
    float value; /**< The floating point value. */
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
     * @param setting_id The id of the setting.
     */
    BinarySetting(uint32_t setting_id) : Setting(setting_id, Type::BINARY, 8, false), value() {}
    /**
     * Constructs a setting holding data.
     *
     * @param setting_id The id of the setting.
     * @param value The data.
     */
    BinarySetting(uint32_t setting_id, const std::vector<uint8_t>& value)
        : Setting(setting_id, Type::BINARY, 8, true), value(value) {}
    std::vector<uint8_t> value; /**< The data. */
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
        REX_ASSERT(size <= std::numeric_limits<uint32_t>::max());
        data->binary.size = static_cast<uint32_t>(size);
        data->binary.ptr = stream->ptr();
        stream->Write(value.data(), size);
      }
    }
    /**
     * Returns the bytes saved to disk.
     *
     * @return The data.
     */
    std::vector<uint8_t> Serialize() const override {
      return std::vector<uint8_t>(value.data(), value.data() + value.size());
    }
    /**
     * Replaces the data with bytes read from disk and marks the setting set.
     *
     * @param data The saved bytes.
     */
    void Deserialize(std::vector<uint8_t> data) override {
      value = data;
      is_set = true;
    }
  };
  /** Profile setting holding a FILETIME timestamp. */
  struct DateTimeSetting : public Setting {
    /**
     * Constructs the setting.
     *
     * @param setting_id The id of the setting.
     * @param value The timestamp.
     */
    DateTimeSetting(uint32_t setting_id, int64_t value)
        : Setting(setting_id, Type::DATETIME, 8, true), value(value) {}
    int64_t value; /**< The timestamp. */
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

  uint64_t xuid() const { return xuid_; }
  std::string name() const { return name_; }
  uint32_t signin_state() const { return 1; }
  uint32_t type() const { return 1 | 2; /* local | online profile? */ }

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
  REX_RUNTIME_API Setting* GetSetting(uint32_t setting_id);

 private:
  uint64_t xuid_;
  std::string name_;
  std::vector<std::unique_ptr<Setting>> setting_list_;
  std::unordered_map<uint32_t, Setting*> settings_;
  Runtime& runtime_;

  REX_RUNTIME_API void LoadSetting(UserProfile::Setting*);
  REX_RUNTIME_API void SaveSetting(UserProfile::Setting*);
};

}  // namespace xam
}  // namespace runtime
}  // namespace rex

/** Formats the enum as its underlying integer. */
template <>
struct std::formatter<rex::runtime::xam::UserProfile::Setting::Type> : std::formatter<rex::i32> {
  /**
   * Formats the value.
   *
   * @param value The value.
   * @param ctx The format context.
   * @return The output iterator.
   */
  auto format(rex::runtime::xam::UserProfile::Setting::Type value, std::format_context& ctx) const {
    return std::formatter<rex::i32>::format(static_cast<rex::i32>(value), ctx);
  }
};
