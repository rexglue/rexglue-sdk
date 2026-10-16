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

#include <algorithm>
#include <cctype>
#include <cstring>

#include <rex/logging/log.h>
#include <rex/memory/byte_swap.h>
#include <rex/hook.h>
#include <rex/kernel/xboxkrnl/format.h>

using rex::runtime::ArrayArgList;
using rex::runtime::FormatCore;
using rex::runtime::GuestPtr;
using rex::runtime::StackArgList;
using rex::runtime::StringFormatData;
using rex::runtime::WideCountFormatData;
using rex::runtime::WideStringFormatData;

REX_HOOK_RAW(__imp__DbgPrint) {
  u32 format_ptr = ctx.r3.u32;
  if (!format_ptr) {
    ctx.r3.u64 = 0;
    return;
  }
  auto format = GuestPtr<const u8*>(base, format_ptr);

  StackArgList args(ctx, base, 1);
  StringFormatData data(format);

  i32 count = FormatCore(base, data, args, false);
  if (count > 0) {
    auto str = data.GetOutput();
    while (!str.empty() && std::isspace(static_cast<unsigned char>(str.back()))) {
      str.pop_back();
    }
    REXLOG_DEV(title, "DbgPrint: {}", str);
  }

  ctx.r3.u64 = 0;
}

REX_HOOK_RAW(__imp__XamDbgPrint) {
  u32 format_ptr = ctx.r3.u32;
  if (!format_ptr) {
    ctx.r3.u64 = 0;
    return;
  }
  auto format = GuestPtr<const u8*>(base, format_ptr);

  StackArgList args(ctx, base, 1);
  StringFormatData data(format);

  i32 count = FormatCore(base, data, args, false);
  if (count > 0) {
    auto str = data.GetOutput();
    while (!str.empty() && std::isspace(static_cast<unsigned char>(str.back()))) {
      str.pop_back();
    }
    REXLOG_DEV(title, "XamDbgPrint: {}", str);
  }

  ctx.r3.u64 = 0;
}

REX_HOOK_RAW(__imp__sprintf) {
  u32 buffer_ptr = ctx.r3.u32;
  u32 format_ptr = ctx.r4.u32;

  if (buffer_ptr == 0 || format_ptr == 0) {
    ctx.r3.u64 = static_cast<u64>(-1);
    return;
  }

  auto buffer = GuestPtr<u8*>(base, buffer_ptr);
  auto format = GuestPtr<const u8*>(base, format_ptr);

  StackArgList args(ctx, base, 2);
  StringFormatData data(format);

  i32 count = FormatCore(base, data, args, false);
  if (count <= 0) {
    buffer[0] = '\0';
  } else {
    std::memcpy(buffer, data.GetOutput().c_str(), count);
    buffer[count] = '\0';
  }
  ctx.r3.u64 = count;
}

REX_HOOK_RAW(__imp___snprintf) {
  u32 buffer_ptr = ctx.r3.u32;
  i32 buffer_count = static_cast<i32>(ctx.r4.u32);
  u32 format_ptr = ctx.r5.u32;

  if (buffer_ptr == 0 || buffer_count <= 0 || format_ptr == 0) {
    ctx.r3.u64 = static_cast<u64>(-1);
    return;
  }

  auto buffer = GuestPtr<u8*>(base, buffer_ptr);
  auto format = GuestPtr<const u8*>(base, format_ptr);

  StackArgList args(ctx, base, 3);
  StringFormatData data(format);

  i32 count = FormatCore(base, data, args, false);
  if (count < 0) {
    if (buffer_count > 0) {
      buffer[0] = '\0';
    }
  } else if (count <= buffer_count) {
    std::memcpy(buffer, data.GetOutput().c_str(), count);
    if (count < buffer_count) {
      buffer[count] = '\0';
    }
  } else {
    std::memcpy(buffer, data.GetOutput().c_str(), buffer_count);
    count = -1;
  }
  ctx.r3.u64 = count;
}

REX_HOOK_RAW(__imp__swprintf) {
  u32 buffer_ptr = ctx.r3.u32;
  u32 format_ptr = ctx.r4.u32;

  if (buffer_ptr == 0 || format_ptr == 0) {
    ctx.r3.u64 = static_cast<u64>(-1);
    return;
  }

  auto buffer = GuestPtr<u16*>(base, buffer_ptr);
  auto format = GuestPtr<const u16*>(base, format_ptr);

  StackArgList args(ctx, base, 2);
  WideStringFormatData data(format);

  i32 count = FormatCore(base, data, args, true);
  if (count <= 0) {
    buffer[0] = '\0';
  } else {
    rex::memory::copy_and_swap(buffer, reinterpret_cast<const u16*>(data.GetWstr().c_str()), count);
    buffer[count] = '\0';
  }
  ctx.r3.u64 = count;
}

REX_HOOK_RAW(__imp___snwprintf) {
  u32 buffer_ptr = ctx.r3.u32;
  i32 buffer_count = static_cast<i32>(ctx.r4.u32);
  u32 format_ptr = ctx.r5.u32;

  if (buffer_ptr == 0 || buffer_count <= 0 || format_ptr == 0) {
    ctx.r3.u64 = static_cast<u64>(-1);
    return;
  }

  auto buffer = GuestPtr<u16*>(base, buffer_ptr);
  auto format = GuestPtr<const u16*>(base, format_ptr);

  StackArgList args(ctx, base, 3);
  WideStringFormatData data(format);

  i32 count = FormatCore(base, data, args, true);
  if (count < 0) {
    if (buffer_count > 0) {
      buffer[0] = '\0';
    }
  } else if (count <= buffer_count) {
    rex::memory::copy_and_swap(buffer, reinterpret_cast<const u16*>(data.GetWstr().c_str()), count);
    if (count < buffer_count) {
      buffer[count] = '\0';
    }
  } else {
    rex::memory::copy_and_swap(buffer, reinterpret_cast<const u16*>(data.GetWstr().c_str()),
                               buffer_count);
    count = -1;
  }
  ctx.r3.u64 = count;
}

REX_HOOK_RAW(__imp__vsprintf) {
  u32 buffer_ptr = ctx.r3.u32;
  u32 format_ptr = ctx.r4.u32;
  u32 arg_ptr = ctx.r5.u32;

  if (buffer_ptr == 0 || format_ptr == 0) {
    ctx.r3.u64 = static_cast<u64>(-1);
    return;
  }

  auto buffer = GuestPtr<u8*>(base, buffer_ptr);
  auto format = GuestPtr<const u8*>(base, format_ptr);

  ArrayArgList args(base, arg_ptr);
  StringFormatData data(format);

  i32 count = FormatCore(base, data, args, false);
  if (count <= 0) {
    buffer[0] = '\0';
  } else {
    std::memcpy(buffer, data.GetOutput().c_str(), count);
    buffer[count] = '\0';
  }
  ctx.r3.u64 = count;
}

REX_HOOK_RAW(__imp___vsnprintf) {
  u32 buffer_ptr = ctx.r3.u32;
  i32 buffer_count = static_cast<i32>(ctx.r4.u32);
  u32 format_ptr = ctx.r5.u32;
  u32 arg_ptr = ctx.r6.u32;

  if (buffer_ptr == 0 || buffer_count <= 0 || format_ptr == 0) {
    ctx.r3.u64 = static_cast<u64>(-1);
    return;
  }

  auto buffer = GuestPtr<u8*>(base, buffer_ptr);
  auto format = GuestPtr<const u8*>(base, format_ptr);

  ArrayArgList args(base, arg_ptr);
  StringFormatData data(format);

  i32 count = FormatCore(base, data, args, false);
  if (count < 0) {
    if (buffer_count > 0) {
      buffer[0] = '\0';
    }
  } else if (count <= buffer_count) {
    std::memcpy(buffer, data.GetOutput().c_str(), count);
    if (count < buffer_count) {
      buffer[count] = '\0';
    }
  } else {
    std::memcpy(buffer, data.GetOutput().c_str(), buffer_count);
  }
  ctx.r3.u64 = count;
}

REX_HOOK_RAW(__imp__vswprintf) {
  u32 buffer_ptr = ctx.r3.u32;
  u32 format_ptr = ctx.r4.u32;
  u32 arg_ptr = ctx.r5.u32;

  if (buffer_ptr == 0 || format_ptr == 0) {
    ctx.r3.u64 = static_cast<u64>(-1);
    return;
  }

  auto buffer = GuestPtr<u16*>(base, buffer_ptr);
  auto format = GuestPtr<const u16*>(base, format_ptr);

  ArrayArgList args(base, arg_ptr);
  WideStringFormatData data(format);

  i32 count = FormatCore(base, data, args, true);
  if (count <= 0) {
    buffer[0] = '\0';
  } else {
    rex::memory::copy_and_swap(buffer, reinterpret_cast<const u16*>(data.GetWstr().c_str()), count);
    buffer[count] = '\0';
  }
  ctx.r3.u64 = count;
}

REX_HOOK_RAW(__imp___vsnwprintf) {
  u32 buffer_ptr = ctx.r3.u32;
  i32 buffer_count = static_cast<i32>(ctx.r4.u32);
  u32 format_ptr = ctx.r5.u32;
  u32 arg_ptr = ctx.r6.u32;

  if (buffer_ptr == 0 || buffer_count <= 0 || format_ptr == 0) {
    ctx.r3.u64 = static_cast<u64>(-1);
    return;
  }

  auto buffer = GuestPtr<u16*>(base, buffer_ptr);
  auto format = GuestPtr<const u16*>(base, format_ptr);

  ArrayArgList args(base, arg_ptr);
  WideStringFormatData data(format);

  i32 count = FormatCore(base, data, args, true);
  if (count < 0) {
    if (buffer_count > 0) {
      buffer[0] = '\0';
    }
  } else if (count <= buffer_count) {
    rex::memory::copy_and_swap(buffer, reinterpret_cast<const u16*>(data.GetWstr().c_str()), count);
    if (count < buffer_count) {
      buffer[count] = '\0';
    }
  } else {
    rex::memory::copy_and_swap(buffer, reinterpret_cast<const u16*>(data.GetWstr().c_str()),
                               buffer_count);
  }
  ctx.r3.u64 = count;
}

REX_HOOK_RAW(__imp___vscwprintf) {
  u32 format_ptr = ctx.r3.u32;
  u32 arg_ptr = ctx.r4.u32;

  if (format_ptr == 0) {
    ctx.r3.u64 = static_cast<u64>(-1);
    return;
  }

  auto format = GuestPtr<const u16*>(base, format_ptr);

  ArrayArgList args(base, arg_ptr);
  WideCountFormatData data(format);

  i32 count = FormatCore(base, data, args, true);
  REX_ASSERT(count < 0 || data.GetCount() == count);
  ctx.r3.u64 = count;
}

REX_EXPORT_STUB(__imp___scprintf);
REX_EXPORT_STUB(__imp___scwprintf);
REX_EXPORT_STUB(__imp___vscprintf);
