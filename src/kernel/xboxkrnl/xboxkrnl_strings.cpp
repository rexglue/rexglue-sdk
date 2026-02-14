/**
 * @file        kernel/xboxkrnl/xboxkrnl_strings.cpp
 * @brief       Xbox kernel string function exports
 *
 * @copyright   Copyright 2022 Ben Vanik. All rights reserved. (Xenia Project)
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <algorithm>
#include <cctype>
#include <cstring>

#include <rex/kernel/format.h>
#include <rex/kernel/flags.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>

using namespace rex::kernel::format;

//=============================================================================
// DbgPrint / XamDbgPrint
//=============================================================================

extern "C" PPC_FUNC(__imp__DbgPrint) {
  uint32_t format_ptr = ctx.r3.u32;
  if (!format_ptr) {
    ctx.r3.u64 = 0;
    return;
  }
  auto format = reinterpret_cast<const uint8_t*>(PPC_RAW_ADDR(format_ptr));

  StackArgList args(ctx, base, 1);
  StringFormatData data(format);

  int32_t count = format_core(base, data, args, false);
  if (count > 0) {
    auto str = data.str();
    // Trim trailing whitespace
    while (!str.empty() && std::isspace(static_cast<unsigned char>(str.back()))) {
      str.pop_back();
    }
    REXKRNL_INFO("DbgPrint: {}", str);
  }

  ctx.r3.u64 = 0;  // NTSTATUS success
}

extern "C" PPC_FUNC(__imp__XamDbgPrint) {
  uint32_t format_ptr = ctx.r3.u32;
  if (!format_ptr) {
    ctx.r3.u64 = 0;
    return;
  }
  auto format = reinterpret_cast<const uint8_t*>(PPC_RAW_ADDR(format_ptr));

  StackArgList args(ctx, base, 1);
  StringFormatData data(format);

  int32_t count = format_core(base, data, args, false);
  if (count > 0) {
    auto str = data.str();
    while (!str.empty() && std::isspace(static_cast<unsigned char>(str.back()))) {
      str.pop_back();
    }
    REXKRNL_INFO("XamDbgPrint: {}", str);
  }

  ctx.r3.u64 = 0;
}

//=============================================================================
// sprintf / _snprintf (narrow, stack varargs)
//=============================================================================

extern "C" PPC_FUNC(__imp__sprintf) {
  uint32_t buffer_ptr = ctx.r3.u32;
  uint32_t format_ptr = ctx.r4.u32;

  if (buffer_ptr == 0 || format_ptr == 0) {
    ctx.r3.u64 = static_cast<uint64_t>(-1);
    return;
  }

  auto buffer = reinterpret_cast<uint8_t*>(PPC_RAW_ADDR(buffer_ptr));
  auto format = reinterpret_cast<const uint8_t*>(PPC_RAW_ADDR(format_ptr));

  StackArgList args(ctx, base, 2);
  StringFormatData data(format);

  int32_t count = format_core(base, data, args, false);
  if (count <= 0) {
    buffer[0] = '\0';
  } else {
    std::memcpy(buffer, data.str().c_str(), count);
    buffer[count] = '\0';
  }
  ctx.r3.u64 = count;
}

extern "C" PPC_FUNC(__imp___snprintf) {
  uint32_t buffer_ptr = ctx.r3.u32;
  int32_t buffer_count = static_cast<int32_t>(ctx.r4.u32);
  uint32_t format_ptr = ctx.r5.u32;

  if (buffer_ptr == 0 || buffer_count <= 0 || format_ptr == 0) {
    ctx.r3.u64 = static_cast<uint64_t>(-1);
    return;
  }

  auto buffer = reinterpret_cast<uint8_t*>(PPC_RAW_ADDR(buffer_ptr));
  auto format = reinterpret_cast<const uint8_t*>(PPC_RAW_ADDR(format_ptr));

  StackArgList args(ctx, base, 3);
  StringFormatData data(format);

  int32_t count = format_core(base, data, args, false);
  if (count < 0) {
    if (buffer_count > 0) {
      buffer[0] = '\0';
    }
  } else if (count <= buffer_count) {
    std::memcpy(buffer, data.str().c_str(), count);
    if (count < buffer_count) {
      buffer[count] = '\0';
    }
  } else {
    std::memcpy(buffer, data.str().c_str(), buffer_count);
    count = -1;
  }
  ctx.r3.u64 = count;
}

//=============================================================================
// swprintf / _snwprintf (wide, stack varargs)
//=============================================================================

extern "C" PPC_FUNC(__imp__swprintf) {
  uint32_t buffer_ptr = ctx.r3.u32;
  uint32_t format_ptr = ctx.r4.u32;

  if (buffer_ptr == 0 || format_ptr == 0) {
    ctx.r3.u64 = static_cast<uint64_t>(-1);
    return;
  }

  auto buffer = reinterpret_cast<uint16_t*>(PPC_RAW_ADDR(buffer_ptr));
  auto format = reinterpret_cast<const uint16_t*>(PPC_RAW_ADDR(format_ptr));

  StackArgList args(ctx, base, 2);
  WideStringFormatData data(format);

  int32_t count = format_core(base, data, args, true);
  if (count <= 0) {
    buffer[0] = '\0';
  } else {
    rex::memory::copy_and_swap(buffer, reinterpret_cast<const uint16_t*>(data.wstr().c_str()), count);
    buffer[count] = '\0';
  }
  ctx.r3.u64 = count;
}

extern "C" PPC_FUNC(__imp___snwprintf) {
  uint32_t buffer_ptr = ctx.r3.u32;
  int32_t buffer_count = static_cast<int32_t>(ctx.r4.u32);
  uint32_t format_ptr = ctx.r5.u32;

  if (buffer_ptr == 0 || buffer_count <= 0 || format_ptr == 0) {
    ctx.r3.u64 = static_cast<uint64_t>(-1);
    return;
  }

  auto buffer = reinterpret_cast<uint16_t*>(PPC_RAW_ADDR(buffer_ptr));
  auto format = reinterpret_cast<const uint16_t*>(PPC_RAW_ADDR(format_ptr));

  StackArgList args(ctx, base, 3);
  WideStringFormatData data(format);

  int32_t count = format_core(base, data, args, true);
  if (count < 0) {
    if (buffer_count > 0) {
      buffer[0] = '\0';
    }
  } else if (count <= buffer_count) {
    rex::memory::copy_and_swap(buffer, reinterpret_cast<const uint16_t*>(data.wstr().c_str()), count);
    if (count < buffer_count) {
      buffer[count] = '\0';
    }
  } else {
    rex::memory::copy_and_swap(buffer, reinterpret_cast<const uint16_t*>(data.wstr().c_str()), buffer_count);
    count = -1;
  }
  ctx.r3.u64 = count;
}

//=============================================================================
// vsprintf / _vsnprintf (narrow, va_list from memory)
//=============================================================================

extern "C" PPC_FUNC(__imp__vsprintf) {
  uint32_t buffer_ptr = ctx.r3.u32;
  uint32_t format_ptr = ctx.r4.u32;
  uint32_t arg_ptr = ctx.r5.u32;

  if (buffer_ptr == 0 || format_ptr == 0) {
    ctx.r3.u64 = static_cast<uint64_t>(-1);
    return;
  }

  auto buffer = reinterpret_cast<uint8_t*>(PPC_RAW_ADDR(buffer_ptr));
  auto format = reinterpret_cast<const uint8_t*>(PPC_RAW_ADDR(format_ptr));

  ArrayArgList args(base, arg_ptr);
  StringFormatData data(format);

  int32_t count = format_core(base, data, args, false);
  if (count <= 0) {
    buffer[0] = '\0';
  } else {
    std::memcpy(buffer, data.str().c_str(), count);
    buffer[count] = '\0';
  }
  ctx.r3.u64 = count;
}

extern "C" PPC_FUNC(__imp___vsnprintf) {
  uint32_t buffer_ptr = ctx.r3.u32;
  int32_t buffer_count = static_cast<int32_t>(ctx.r4.u32);
  uint32_t format_ptr = ctx.r5.u32;
  uint32_t arg_ptr = ctx.r6.u32;

  if (buffer_ptr == 0 || buffer_count <= 0 || format_ptr == 0) {
    ctx.r3.u64 = static_cast<uint64_t>(-1);
    return;
  }

  auto buffer = reinterpret_cast<uint8_t*>(PPC_RAW_ADDR(buffer_ptr));
  auto format = reinterpret_cast<const uint8_t*>(PPC_RAW_ADDR(format_ptr));

  ArrayArgList args(base, arg_ptr);
  StringFormatData data(format);

  int32_t count = format_core(base, data, args, false);
  if (count < 0) {
    if (buffer_count > 0) {
      buffer[0] = '\0';
    }
  } else if (count <= buffer_count) {
    std::memcpy(buffer, data.str().c_str(), count);
    if (count < buffer_count) {
      buffer[count] = '\0';
    }
  } else {
    std::memcpy(buffer, data.str().c_str(), buffer_count);
  }
  ctx.r3.u64 = count;
}

//=============================================================================
// vswprintf / _vsnwprintf / _vscwprintf (wide, va_list from memory)
//=============================================================================

extern "C" PPC_FUNC(__imp__vswprintf) {
  uint32_t buffer_ptr = ctx.r3.u32;
  uint32_t format_ptr = ctx.r4.u32;
  uint32_t arg_ptr = ctx.r5.u32;

  if (buffer_ptr == 0 || format_ptr == 0) {
    ctx.r3.u64 = static_cast<uint64_t>(-1);
    return;
  }

  auto buffer = reinterpret_cast<uint16_t*>(PPC_RAW_ADDR(buffer_ptr));
  auto format = reinterpret_cast<const uint16_t*>(PPC_RAW_ADDR(format_ptr));

  ArrayArgList args(base, arg_ptr);
  WideStringFormatData data(format);

  int32_t count = format_core(base, data, args, true);
  if (count <= 0) {
    buffer[0] = '\0';
  } else {
    rex::memory::copy_and_swap(buffer, reinterpret_cast<const uint16_t*>(data.wstr().c_str()), count);
    buffer[count] = '\0';
  }
  ctx.r3.u64 = count;
}

extern "C" PPC_FUNC(__imp___vsnwprintf) {
  uint32_t buffer_ptr = ctx.r3.u32;
  int32_t buffer_count = static_cast<int32_t>(ctx.r4.u32);
  uint32_t format_ptr = ctx.r5.u32;
  uint32_t arg_ptr = ctx.r6.u32;

  if (buffer_ptr == 0 || buffer_count <= 0 || format_ptr == 0) {
    ctx.r3.u64 = static_cast<uint64_t>(-1);
    return;
  }

  auto buffer = reinterpret_cast<uint16_t*>(PPC_RAW_ADDR(buffer_ptr));
  auto format = reinterpret_cast<const uint16_t*>(PPC_RAW_ADDR(format_ptr));

  ArrayArgList args(base, arg_ptr);
  WideStringFormatData data(format);

  int32_t count = format_core(base, data, args, true);
  if (count < 0) {
    if (buffer_count > 0) {
      buffer[0] = '\0';
    }
  } else if (count <= buffer_count) {
    rex::memory::copy_and_swap(buffer, reinterpret_cast<const uint16_t*>(data.wstr().c_str()), count);
    if (count < buffer_count) {
      buffer[count] = '\0';
    }
  } else {
    rex::memory::copy_and_swap(buffer, reinterpret_cast<const uint16_t*>(data.wstr().c_str()), buffer_count);
  }
  ctx.r3.u64 = count;
}

extern "C" PPC_FUNC(__imp___vscwprintf) {
  uint32_t format_ptr = ctx.r3.u32;
  uint32_t arg_ptr = ctx.r4.u32;

  if (format_ptr == 0) {
    ctx.r3.u64 = static_cast<uint64_t>(-1);
    return;
  }

  auto format = reinterpret_cast<const uint16_t*>(PPC_RAW_ADDR(format_ptr));

  ArrayArgList args(base, arg_ptr);
  WideCountFormatData data(format);

  int32_t count = format_core(base, data, args, true);
  assert_true(count < 0 || data.count() == count);
  ctx.r3.u64 = count;
}
