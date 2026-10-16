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

#include <rex/kernel/xam/apps/app.h>
#include <cstring>
#include <string_view>

#include <rex/fail.h>
#include <rex/logging/log.h>
#include <rex/runtime.h>
#include <rex/runtime/content_manager.h>
#include <rex/runtime/enumerator_object.h>
#include <rex/runtime/object_table.h>
#include <rex/runtime/xam/content_device.h>
#include <rex/platform/thread.h>

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::runtime;
namespace apps {
using namespace rex::runtime;

XAMApp::XAMApp(Runtime& runtime) : App(runtime, 0xFE) {}

X_HRESULT XAMApp::DispatchMessageSync(u32 message, u32 buffer_ptr, u32 buffer_length) {
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  switch (message) {
    case 0x0002000E: {
      struct MessageData {
        be_u32 user_index;
        be_u32 unk_04;
        be_u32 extra_ptr;
        be_u32 buffer_ptr;
        be_u32 buffer_size;
        be_u32 unk_14;
        be_u32 length_ptr;
        be_u32 unk_1c;
      }* data = reinterpret_cast<MessageData*>(buffer);
      REXLOG_DEBUG(kernel,
                   "XamAppEnumerateContentAggregate({}, {:08X}, {:08X}, {:08X}, {}, "
                   "{:08X}, {:08X}, {:08X})",
                   static_cast<u32>(data->user_index), static_cast<u32>(data->unk_04),
                   static_cast<u32>(data->extra_ptr), static_cast<u32>(data->buffer_ptr),
                   static_cast<u32>(data->buffer_size), static_cast<u32>(data->unk_14),
                   static_cast<u32>(data->length_ptr), static_cast<u32>(data->unk_1c));
      auto extra = memory_->TranslateVirtual<X_KENUMERATOR_CONTENT_AGGREGATE*>(data->extra_ptr);
      auto content_buffer = memory_->TranslateVirtual(data->buffer_ptr);
      auto e = runtime_.GetObjectTable().Get<EnumeratorObject>(extra->handle);
      if (!e || !content_buffer || !extra) {
        return X_E_INVALIDARG;
      }
      REX_ASSERT(extra->magic == kStashSignature);
      if (data->buffer_size) {
        std::memset(content_buffer, 0, data->buffer_size);
      }
      u32 item_count = 0;
      auto result = e->write_items(data->buffer_ptr, content_buffer, &item_count);

      if (result == X_ERROR_SUCCESS && item_count >= 1) {
        if (data->length_ptr) {
          auto length_ptr = memory_->TranslateVirtual<be_u32*>(data->length_ptr);
          *length_ptr = 1;
        }
        return X_E_SUCCESS;
      }
      if (data->length_ptr) {
        *memory_->TranslateVirtual<be_u32*>(data->length_ptr) = 0;
      }
      return X_E_NO_MORE_FILES;
    }
    case 0x00020021: {
      struct MessageData {
        char unk_00[64];
        be_u32 unk_40; /**< `KeGetCurrentProcessType() < 1 ? 1 : 0` */
        be_u32 unk_44;
        be_u32 unk_48;
      }* data = reinterpret_cast<MessageData*>(buffer);
      REX_ASSERT(buffer_length == sizeof(MessageData));
      REXLOG_DEBUG(kernel, "XAMApp(0x00020021)('{}', {:08X}, {:08X}, {:08X})", data->unk_00,
                   static_cast<u32>(data->unk_40), static_cast<u32>(data->unk_44),
                   static_cast<u32>(data->unk_48));
      const auto device_id = runtime_.GetContentManager().GetContentDeviceID(
          std::string_view(data->unk_00, strnlen(data->unk_00, sizeof(data->unk_00))));
      const auto* device = device_id ? GetDummyDeviceInfo(*device_id) : nullptr;
      if (!device) {
        return X_E_INVALIDARG;
      }
      *memory_->TranslateVirtual<be_u32*>(data->unk_44) = static_cast<u32>(device->device_type);
      return X_E_SUCCESS;
    }
    case 0x00021012: {
      REXLOG_DEBUG(kernel, "XAMApp(0x00021012)");
      return X_E_SUCCESS;
    }
    case 0x00022005: {
      struct MessageData {
        be_u32 unk_00;
        be_u32 unk_04;
      }* data = reinterpret_cast<MessageData*>(buffer);
      REX_ASSERT(buffer_length == sizeof(MessageData));
      REXLOG_DEBUG(kernel, "XAMApp(0x00022005)(%.8X, %.8X)", static_cast<u32>(data->unk_00),
                   static_cast<u32>(data->unk_04));
      return X_E_SUCCESS;
    }
    default:
      break;
  }
  REX_FAIL(kernel, X_E_FAIL,
           "Unimplemented XAM message app={:08X}, msg={:08X}, arg1={:08X}, "
           "arg2={:08X}",
           GetAppId(), message, buffer_ptr, buffer_length);
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace rex
