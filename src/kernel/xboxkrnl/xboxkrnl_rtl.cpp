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
#include <atomic>
#include <cstddef>
#include <string>

#include <rex/chrono/chrono_steady_cast.h>
#include <rex/kernel/xboxkrnl/rtl.h>
#include <rex/kernel/xboxkrnl/threading.h>
#include <rex/logging/log.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/string.h>
#include <rex/runtime.h>
#include <rex/runtime/module_object.h>
#include <rex/runtime/kernel_string.h>
#include <rex/runtime/event_object.h>
#include <rex/runtime/xthread.h>
#include <rex/platform/thread.h>
#include "kernel/stub_log.h"

namespace rex::kernel::xboxkrnl {
using namespace rex::runtime;

namespace {

constexpr u16 kLastLatin1 = 0xFF;
constexpr u8 kFirstLatin1Lower = 0xE0;
constexpr u8 kLatin1Division = 0xF7;
constexpr u8 kCaseBit = 0x20;
constexpr u8 kUnmappableChar = '?';
constexpr u32 kMaxStringBytes = 0xFFFF;

}  // namespace

u32 RtlCompareMemory_entry(MappedVoid source1, MappedVoid source2, u32 length) {
  u8* p1 = source1;
  u8* p2 = source2;

  // The return value is the count of matching bytes, so memcmp does not apply.

  u32 c = 0;
  for (u32 n = 0; n < length; n++, p1++, p2++) {
    if (*p1 == *p2) {
      c++;
    }
  }

  return c;
}

u32 RtlCompareMemoryUlong_entry(MappedVoid source, u32 length, u32 pattern) {
  if (source.GetGuestAddress() % 4 || length % 4) {
    return 0;
  }

  u32 n = 0;
  for (u32 i = 0; i < (length / 4); i++) {
    u32 val = source.AsArray<u32>()[i];
    if (val == pattern) {
      n++;
    }
  }

  return n;
}

void RtlFillMemoryUlong_entry(MappedVoid destination, u32 length, u32 pattern) {
  // length must be a multiple of 4.
  std::fill_n(destination.As<be_u32*>(), length >> 2, pattern);
}

u32 RtlUpperChar_entry(u32 in) {
  const u8 c = static_cast<u8>(in & 0xFF);
  if (c >= 'a' && c <= 'z') {
    return c ^ kCaseBit;
  }
  if (c >= kFirstLatin1Lower && c != kLatin1Division) {
    return c == kLastLatin1 ? kUnmappableChar : c ^ kCaseBit;
  }

  return c;
}

u32 RtlLowerChar_entry(u32 in) {
  const u8 c = static_cast<u8>(in & 0xFF);
  if (c >= 'A' && c <= 'Z') {
    return c ^ 0x20;
  }

  return c;
}

u32 RtlCompareString_entry(MappedString string_1, MappedString string_2, u32 case_insensitive) {
  int ret = case_insensitive ? rex::CompareIgnoreCase(string_1, string_2)
                             : std::strcmp(string_1, string_2);

  return ret;
}

u32 RtlCompareStringN_entry(MappedString string_1, u32 string_1_len, MappedString string_2,
                            u32 string_2_len, u32 case_insensitive) {
  u32 len1 = string_1_len;
  u32 len2 = string_2_len;

  if (string_1_len == 0xFFFF) {
    len1 = static_cast<u32>(std::strlen(string_1));
  }
  if (string_2_len == 0xFFFF) {
    len2 = static_cast<u32>(std::strlen(string_2));
  }
  auto len = std::min(len1, len2);

  int ret = case_insensitive ? rex::CompareIgnoreCaseN(string_1, string_2, len)
                             : std::strncmp(string_1, string_2, len);

  return ret;
}

void RtlInitAnsiString_entry(GuestPtrT<X_ANSI_STRING> destination, MappedString source) {
  REXLOG_TRACE(kernel,
               "RtlInitAnsiString "
               "str={}",
               source ? source.GetValue() : "(null)");
  if (source) {
    u16 length = static_cast<u16>(strlen(source));
    destination->length = length;
    destination->maximum_length = length + 1;
  } else {
    destination->Reset();
  }

  destination->pointer = source.GetGuestAddress();
}

void RtlFreeAnsiString_entry(GuestPtrT<X_ANSI_STRING> string) {
  if (string->pointer) {
    runtime::GetCurrent().GetMemory()->SystemHeapFree(string->pointer);
  }

  string->Reset();
}

void RtlInitUnicodeString_entry(GuestPtrT<X_UNICODE_STRING> destination, MappedWString source) {
  if (source) {
    destination->length = static_cast<u16>(source.GetValue().size()) * 2;
    destination->maximum_length = static_cast<u16>(source.GetValue().size() + 1) * 2;
    destination->pointer = source.GetGuestAddress();
  } else {
    destination->Reset();
  }
}

void RtlFreeUnicodeString_entry(GuestPtrT<X_UNICODE_STRING> string) {
  if (string->pointer) {
    runtime::GetCurrent().GetMemory()->SystemHeapFree(string->pointer);
  }

  string->Reset();
}

void RtlCopyString_entry(GuestPtrT<X_ANSI_STRING> destination, GuestPtrT<X_ANSI_STRING> source) {
  if (!source) {
    destination->length = 0;
    return;
  }

  auto length = std::min(destination->maximum_length, source->length);
  if (length > 0) {
    auto dst_buf = runtime::GetCurrent().GetMemory()->TranslateVirtual(destination->pointer);
    auto src_buf = runtime::GetCurrent().GetMemory()->TranslateVirtual(source->pointer);
    std::memcpy(dst_buf, src_buf, length);
  }
  destination->length = length;
}

void RtlCopyUnicodeString_entry(GuestPtrT<X_UNICODE_STRING> destination,
                                GuestPtrT<X_UNICODE_STRING> source) {
  if (!source) {
    destination->length = 0;
    return;
  }

  auto length = std::min(destination->maximum_length, source->length);
  if (length > 0) {
    auto dst_buf = runtime::GetCurrent().GetMemory()->TranslateVirtual(destination->pointer);
    auto src_buf = runtime::GetCurrent().GetMemory()->TranslateVirtual(source->pointer);
    std::memcpy(dst_buf, src_buf, length);
  }
  destination->length = length;
}

u32 RtlUnicodeStringToAnsiString_entry(GuestPtrT<X_ANSI_STRING> destination_ptr,
                                       GuestPtrT<X_UNICODE_STRING> source_ptr, u32 alloc_dest) {
  auto* memory = runtime::GetCurrent().GetMemory();
  const u32 count = source_ptr->length / 2;
  u32 capacity = 0;
  u32 buffer_ptr = 0;
  if (alloc_dest) {
    capacity = count + 1;
    buffer_ptr = memory->SystemHeapAlloc(capacity);
    if (!buffer_ptr) {
      return X_STATUS_NO_MEMORY;
    }
    destination_ptr->maximum_length = static_cast<u16>(capacity);
    destination_ptr->pointer = buffer_ptr;
  } else {
    capacity = destination_ptr->maximum_length;
    buffer_ptr = destination_ptr->pointer;
  }
  if (capacity == 0) {
    destination_ptr->length = static_cast<u16>(count);
    return X_STATUS_BUFFER_OVERFLOW;
  }

  const u32 copy_len = std::min(count, capacity - 1);
  const auto* source = memory->TranslateVirtual<const be_u16*>(source_ptr->pointer);
  u8* buffer = memory->TranslateVirtual(buffer_ptr);
  std::transform(source, source + copy_len, buffer, [](u16 unit) {
    return unit <= kLastLatin1 ? static_cast<u8>(unit) : kUnmappableChar;
  });
  buffer[copy_len] = 0;
  destination_ptr->length = static_cast<u16>(copy_len);
  return copy_len < count ? X_STATUS_BUFFER_OVERFLOW : X_STATUS_SUCCESS;
}

u32 RtlAnsiStringToUnicodeString_entry(GuestPtrT<X_UNICODE_STRING> destination_ptr,
                                       GuestPtrT<X_ANSI_STRING> source_ptr, u32 alloc_dest) {
  auto* memory = runtime::GetCurrent().GetMemory();
  const u32 count = source_ptr->length;
  const u32 length = count * 2;
  const u32 needed = length + 2;
  if (needed > kMaxStringBytes) {
    return X_STATUS_INVALID_PARAMETER_2;
  }
  u32 buffer_ptr = 0;
  if (alloc_dest) {
    buffer_ptr = memory->SystemHeapAlloc(needed);
    if (!buffer_ptr) {
      return X_STATUS_NO_MEMORY;
    }
    destination_ptr->maximum_length = static_cast<u16>(needed);
    destination_ptr->pointer = buffer_ptr;
  } else {
    if (destination_ptr->maximum_length < needed) {
      destination_ptr->length = static_cast<u16>(length);
      return X_STATUS_BUFFER_OVERFLOW;
    }
    buffer_ptr = destination_ptr->pointer;
  }

  const auto* source = memory->TranslateVirtual<const u8*>(source_ptr->pointer);
  auto* buffer = memory->TranslateVirtual<be_u16*>(buffer_ptr);
  std::copy_n(source, count, buffer);
  buffer[count] = 0;
  destination_ptr->length = static_cast<u16>(length);
  return X_STATUS_SUCCESS;
}

u32 RtlMultiByteToUnicodeN_entry(MappedU16 destination_ptr, u32 destination_len,
                                 MappedU32 written_ptr, GuestPtrT<u8> source_ptr, u32 source_len) {
  u32 copy_len = destination_len >> 1;
  copy_len = copy_len < source_len ? copy_len : source_len;

  for (u32 i = 0; i < copy_len; i++) {
    destination_ptr[i] = source_ptr[i];
  }

  if (written_ptr.GetGuestAddress() != 0) {
    *written_ptr = copy_len << 1;
  }

  return 0;
}

u32 RtlUnicodeToMultiByteN_entry(GuestPtrT<u8> destination_ptr, u32 destination_len,
                                 MappedU32 written_ptr, MappedU16 source_ptr, u32 source_len) {
  u32 copy_len = source_len >> 1;
  copy_len = copy_len < destination_len ? copy_len : destination_len;

  for (u32 i = 0; i < copy_len; i++) {
    u16 c = source_ptr[i];
    destination_ptr[i] = c < 256 ? static_cast<u8>(c) : '?';
  }

  if (written_ptr.GetGuestAddress() != 0) {
    *written_ptr = copy_len;
  }
  return 0;
}

u32 RtlMultiByteToUnicodeSize_entry(MappedU32 size_ptr, [[maybe_unused]] MappedVoid source,
                                    u32 source_len) {
  if (size_ptr.GetGuestAddress() != 0) {
    *size_ptr = source_len << 1;
  }
  return X_STATUS_SUCCESS;
}

u32 RtlUnicodeToMultiByteSize_entry(MappedU32 size_ptr, [[maybe_unused]] MappedVoid source,
                                    u32 source_len) {
  if (size_ptr.GetGuestAddress() != 0) {
    *size_ptr = source_len >> 1;
  }
  return X_STATUS_SUCCESS;
}

u32 RtlImageNtHeader_entry([[maybe_unused]] MappedVoid module) {
  return 0;
}

u32 RtlImageXexHeaderField_entry(GuestPtrT<filesystem::XEXHeader> xex_header, u32 field_dword) {
  u32 field_value = 0;
  u32 field = field_dword;

  ModuleObject::GetOptHeader(runtime::GetCurrent().GetMemory(), xex_header,
                             filesystem::XEXHeaderKey(field), &field_value);

  return field_value;
}

/**
 * Critical section as laid out on the console, 28 bytes. The host RTL_CRITICAL_SECTION is 32
 * bytes, so it cannot replace the console one in place, and the console never calls
 * RtlDeleteCriticalSection, so host handles could not be freed. The struct is mapped
 * into guest memory, where it is opaque to the title. Titles can embed critical sections in
 * initialized form, so the initializers may never run.
 *
 * header.type is 1 (EventSynchronizationObject, auto reset) and header.absolute holds the spin
 * count divided by 256.
 */
#pragma pack(push, 1)
struct X_RTL_CRITICAL_SECTION {
  X_DISPATCH_HEADER header;
  i32 lock_count;         /**< -1 becomes 0 on first lock. */
  be_i32 recursion_count; /**< 0 becomes 1 on first lock. */
  be_u32 owning_thread;   /**< PKTHREAD or 0 when unlocked. */
};
#pragma pack(pop)
REX_STATIC_ASSERT_SIZE(X_RTL_CRITICAL_SECTION, 28);
static_assert(offsetof(X_RTL_CRITICAL_SECTION, lock_count) %
                  std::atomic_ref<i32>::required_alignment ==
              0);

namespace {

std::atomic_ref<i32> LockCount(X_RTL_CRITICAL_SECTION& cs) {
  return std::atomic_ref<i32>(*reinterpret_cast<i32*>(
      reinterpret_cast<u8*>(&cs) + offsetof(X_RTL_CRITICAL_SECTION, lock_count)));
}

bool TryAcquireLockCount(X_RTL_CRITICAL_SECTION& cs) {
  i32 expected = -1;
  return LockCount(cs).compare_exchange_strong(expected, 0);
}

}  // namespace

void RtlInitializeCriticalSection(X_RTL_CRITICAL_SECTION* cs) {
  cs->header.type = 1;
  cs->header.absolute = 0;
  cs->header.signal_state = 0;
  cs->lock_count = -1;
  cs->recursion_count = 0;
  cs->owning_thread = 0;
}

void RtlInitializeCriticalSection_entry(GuestPtrT<X_RTL_CRITICAL_SECTION> cs) {
  REXLOG_TRACE(kernel,
               "RtlInitializeCriticalSection "
               "cs={:#x}",
               cs.GetGuestAddress());
  RtlInitializeCriticalSection(cs);
}

X_STATUS RtlInitializeCriticalSectionAndSpinCount(X_RTL_CRITICAL_SECTION* cs, u32 spin_count) {
  u32 spin_count_div_256 = (spin_count + 255) >> 8;
  if (spin_count_div_256 > 255) {
    spin_count_div_256 = 255;
  }

  cs->header.type = 1;
  cs->header.absolute = spin_count_div_256;
  cs->header.signal_state = 0;
  cs->lock_count = -1;
  cs->recursion_count = 0;
  cs->owning_thread = 0;

  return X_STATUS_SUCCESS;
}

u32 RtlInitializeCriticalSectionAndSpinCount_entry(GuestPtrT<X_RTL_CRITICAL_SECTION> cs,
                                                   u32 spin_count) {
  return RtlInitializeCriticalSectionAndSpinCount(cs, spin_count);
}

void RtlEnterCriticalSection_entry(GuestPtrT<X_RTL_CRITICAL_SECTION> cs) {
  u32 cur_thread = KernelThread::GetCurrentThread()->GetGuestObject();
  u32 spin_count = cs->header.absolute * 256;

  if (cs->owning_thread == cur_thread) {
    ++LockCount(*cs);
    cs->recursion_count++;
    return;
  }

  while (spin_count--) {
    if (TryAcquireLockCount(*cs)) {
      cs->owning_thread = cur_thread;
      cs->recursion_count = 1;
      return;
    }
  }

  if (++LockCount(*cs) != 0) {
    KeWaitForSingleObject(cs.GetGuestAddress(), 8, 0, 0, nullptr);
  }

  REX_ASSERT(cs->owning_thread == 0);
  cs->owning_thread = cur_thread;
  cs->recursion_count = 1;
}

u32 RtlTryEnterCriticalSection_entry(GuestPtrT<X_RTL_CRITICAL_SECTION> cs) {
  u32 thread = KernelThread::GetCurrentThread()->GetGuestObject();

  if (TryAcquireLockCount(*cs)) {
    cs->owning_thread = thread;
    cs->recursion_count = 1;
    return 1;
  } else if (cs->owning_thread == thread) {
    ++LockCount(*cs);
    ++cs->recursion_count;
    return 1;
  }

  return 0;
}

void RtlLeaveCriticalSection_entry(GuestPtrT<X_RTL_CRITICAL_SECTION> cs) {
  REX_ASSERT(cs->owning_thread == KernelThread::GetCurrentThread()->GetGuestObject());

  REX_ASSERT(cs->recursion_count > 0);
  if (--cs->recursion_count != 0) {
    REX_ASSERT(cs->recursion_count >= 0);

    --LockCount(*cs);
    return;
  }

  cs->owning_thread = 0;
  if (--LockCount(*cs) != -1) {
    KeSetEvent(cs.GetGuestAddress(), 1, 0);
  }
}

struct X_TIME_FIELDS {
  be_u16 year;
  be_u16 month;
  be_u16 day;
  be_u16 hour;
  be_u16 minute;
  be_u16 second;
  be_u16 milliseconds;
  be_u16 weekday;
};
REX_STATIC_ASSERT_SIZE(X_TIME_FIELDS, 16);

void RtlTimeToTimeFields_entry(MappedU64 time_ptr, GuestPtrT<X_TIME_FIELDS> time_fields_ptr) {
  // Use the host clock because scaling must not apply, only conversion.
  using rex::WinSystemClock;
  auto tp = WinSystemClock::to_sys(WinSystemClock::FromFileTime(time_ptr.GetValue()));
  auto dp = std::chrono::floor<std::chrono::days>(tp);
  auto year_month_day = std::chrono::year_month_day{dp};
  auto weekday = std::chrono::weekday{dp};
  auto time = std::chrono::hh_mm_ss{std::chrono::floor<std::chrono::milliseconds>(tp - dp)};
  time_fields_ptr->year = static_cast<int>(year_month_day.year());
  time_fields_ptr->month = static_cast<unsigned>(year_month_day.month());
  time_fields_ptr->day = static_cast<unsigned>(year_month_day.day());
  time_fields_ptr->weekday = weekday.c_encoding();
  time_fields_ptr->hour = time.hours().count();
  time_fields_ptr->minute = time.minutes().count();
  time_fields_ptr->second = static_cast<u16>(time.seconds().count());
  time_fields_ptr->milliseconds = static_cast<u16>(time.subseconds().count());
}

u32 RtlTimeFieldsToTime_entry(GuestPtrT<X_TIME_FIELDS> time_fields_ptr, MappedU64 time_ptr) {
  using rex::WinSystemClock;
  if (time_fields_ptr->year < 1601 || time_fields_ptr->month < 1 || time_fields_ptr->month > 12 ||
      time_fields_ptr->day < 1 || time_fields_ptr->day > 31 || time_fields_ptr->hour > 23 ||
      time_fields_ptr->minute > 59 || time_fields_ptr->second > 59 ||
      time_fields_ptr->milliseconds > 999) {
    return 0;
  }
  auto year = std::chrono::year{time_fields_ptr->year};
  auto month = std::chrono::month{time_fields_ptr->month};
  auto day = std::chrono::day{time_fields_ptr->day};
  auto year_month_day = std::chrono::year_month_day{year, month, day};
  if (!year_month_day.ok()) {
    return 0;
  }
  auto dp = static_cast<std::chrono::sys_days>(year_month_day);
  std::chrono::sys_time<rex::HundredNanoseconds> time = dp;
  time += std::chrono::hours{time_fields_ptr->hour};
  time += std::chrono::minutes{time_fields_ptr->minute};
  time += std::chrono::seconds{time_fields_ptr->second};
  time += std::chrono::milliseconds{time_fields_ptr->milliseconds};
  *time_ptr = WinSystemClock::ToFileTime(WinSystemClock::from_sys(time));
  return 1;
}

static u32 g_crc32_table[256] = {
    0x00000000u, 0x77073096u, 0xEE0E612Cu, 0x990951BAu, 0x076DC419u, 0x706AF48Fu, 0xE963A535u,
    0x9E6495A3u, 0x0EDB8832u, 0x79DCB8A4u, 0xE0D5E91Eu, 0x97D2D988u, 0x09B64C2Bu, 0x7EB17CBDu,
    0xE7B82D07u, 0x90BF1D91u, 0x1DB71064u, 0x6AB020F2u, 0xF3B97148u, 0x84BE41DEu, 0x1ADAD47Du,
    0x6DDDE4EBu, 0xF4D4B551u, 0x83D385C7u, 0x136C9856u, 0x646BA8C0u, 0xFD62F97Au, 0x8A65C9ECu,
    0x14015C4Fu, 0x63066CD9u, 0xFA0F3D63u, 0x8D080DF5u, 0x3B6E20C8u, 0x4C69105Eu, 0xD56041E4u,
    0xA2677172u, 0x3C03E4D1u, 0x4B04D447u, 0xD20D85FDu, 0xA50AB56Bu, 0x35B5A8FAu, 0x42B2986Cu,
    0xDBBBC9D6u, 0xACBCF940u, 0x32D86CE3u, 0x45DF5C75u, 0xDCD60DCFu, 0xABD13D59u, 0x26D930ACu,
    0x51DE003Au, 0xC8D75180u, 0xBFD06116u, 0x21B4F4B5u, 0x56B3C423u, 0xCFBA9599u, 0xB8BDA50Fu,
    0x2802B89Eu, 0x5F058808u, 0xC60CD9B2u, 0xB10BE924u, 0x2F6F7C87u, 0x58684C11u, 0xC1611DABu,
    0xB6662D3Du, 0x76DC4190u, 0x01DB7106u, 0x98D220BCu, 0xEFD5102Au, 0x71B18589u, 0x06B6B51Fu,
    0x9FBFE4A5u, 0xE8B8D433u, 0x7807C9A2u, 0x0F00F934u, 0x9609A88Eu, 0xE10E9818u, 0x7F6A0DBBu,
    0x086D3D2Du, 0x91646C97u, 0xE6635C01u, 0x6B6B51F4u, 0x1C6C6162u, 0x856530D8u, 0xF262004Eu,
    0x6C0695EDu, 0x1B01A57Bu, 0x8208F4C1u, 0xF50FC457u, 0x65B0D9C6u, 0x12B7E950u, 0x8BBEB8EAu,
    0xFCB9887Cu, 0x62DD1DDFu, 0x15DA2D49u, 0x8CD37CF3u, 0xFBD44C65u, 0x4DB26158u, 0x3AB551CEu,
    0xA3BC0074u, 0xD4BB30E2u, 0x4ADFA541u, 0x3DD895D7u, 0xA4D1C46Du, 0xD3D6F4FBu, 0x4369E96Au,
    0x346ED9FCu, 0xAD678846u, 0xDA60B8D0u, 0x44042D73u, 0x33031DE5u, 0xAA0A4C5Fu, 0xDD0D7CC9u,
    0x5005713Cu, 0x270241AAu, 0xBE0B1010u, 0xC90C2086u, 0x5768B525u, 0x206F85B3u, 0xB966D409u,
    0xCE61E49Fu, 0x5EDEF90Eu, 0x29D9C998u, 0xB0D09822u, 0xC7D7A8B4u, 0x59B33D17u, 0x2EB40D81u,
    0xB7BD5C3Bu, 0xC0BA6CADu, 0xEDB88320u, 0x9ABFB3B6u, 0x03B6E20Cu, 0x74B1D29Au, 0xEAD54739u,
    0x9DD277AFu, 0x04DB2615u, 0x73DC1683u, 0xE3630B12u, 0x94643B84u, 0x0D6D6A3Eu, 0x7A6A5AA8u,
    0xE40ECF0Bu, 0x9309FF9Du, 0x0A00AE27u, 0x7D079EB1u, 0xF00F9344u, 0x8708A3D2u, 0x1E01F268u,
    0x6906C2FEu, 0xF762575Du, 0x806567CBu, 0x196C3671u, 0x6E6B06E7u, 0xFED41B76u, 0x89D32BE0u,
    0x10DA7A5Au, 0x67DD4ACCu, 0xF9B9DF6Fu, 0x8EBEEFF9u, 0x17B7BE43u, 0x60B08ED5u, 0xD6D6A3E8u,
    0xA1D1937Eu, 0x38D8C2C4u, 0x4FDFF252u, 0xD1BB67F1u, 0xA6BC5767u, 0x3FB506DDu, 0x48B2364Bu,
    0xD80D2BDAu, 0xAF0A1B4Cu, 0x36034AF6u, 0x41047A60u, 0xDF60EFC3u, 0xA867DF55u, 0x316E8EEFu,
    0x4669BE79u, 0xCB61B38Cu, 0xBC66831Au, 0x256FD2A0u, 0x5268E236u, 0xCC0C7795u, 0xBB0B4703u,
    0x220216B9u, 0x5505262Fu, 0xC5BA3BBEu, 0xB2BD0B28u, 0x2BB45A92u, 0x5CB36A04u, 0xC2D7FFA7u,
    0xB5D0CF31u, 0x2CD99E8Bu, 0x5BDEAE1Du, 0x9B64C2B0u, 0xEC63F226u, 0x756AA39Cu, 0x026D930Au,
    0x9C0906A9u, 0xEB0E363Fu, 0x72076785u, 0x05005713u, 0x95BF4A82u, 0xE2B87A14u, 0x7BB12BAEu,
    0x0CB61B38u, 0x92D28E9Bu, 0xE5D5BE0Du, 0x7CDCEFB7u, 0x0BDBDF21u, 0x86D3D2D4u, 0xF1D4E242u,
    0x68DDB3F8u, 0x1FDA836Eu, 0x81BE16CDu, 0xF6B9265Bu, 0x6FB077E1u, 0x18B74777u, 0x88085AE6u,
    0xFF0F6A70u, 0x66063BCAu, 0x11010B5Cu, 0x8F659EFFu, 0xF862AE69u, 0x616BFFD3u, 0x166CCF45u,
    0xA00AE278u, 0xD70DD2EEu, 0x4E048354u, 0x3903B3C2u, 0xA7672661u, 0xD06016F7u, 0x4969474Du,
    0x3E6E77DBu, 0xAED16A4Au, 0xD9D65ADCu, 0x40DF0B66u, 0x37D83BF0u, 0xA9BCAE53u, 0xDEBB9EC5u,
    0x47B2CF7Fu, 0x30B5FFE9u, 0xBDBDF21Cu, 0xCABAC28Au, 0x53B39330u, 0x24B4A3A6u, 0xBAD03605u,
    0xCDD70693u, 0x54DE5729u, 0x23D967BFu, 0xB3667A2Eu, 0xC4614AB8u, 0x5D681B02u, 0x2A6F2B94u,
    0xB40BBE37u, 0xC30C8EA1u, 0x5A05DF1Bu, 0x2D02EF8Du,
};

u32 RtlComputeCrc32_entry(u32 initial_crc, MappedVoid buffer, u32 length) {
  if (!length) {
    return initial_crc;
  }
  u32 hash = ~initial_crc;
  for (u32 i = 0; i < length; ++i) {
    hash = g_crc32_table[buffer[i] ^ (hash & 0xFF)] ^ (hash >> 8);
  }
  return ~hash;
}

void RtlCaptureContext_entry() {
  REX_KERNEL_STUB("RtlCaptureContext");
}

void RtlUnwind_entry() {
  REX_KERNEL_STUB("RtlUnwind");
}

void __C_specific_handler_entry() {
  REX_KERNEL_STUB("__C_specific_handler");
}

REX_EXPORT_STUB(__imp__RtlAppendStringToString);
REX_EXPORT_STUB(__imp__RtlAppendUnicodeStringToString);
REX_EXPORT_STUB(__imp__RtlAppendUnicodeToString);
REX_EXPORT_STUB(__imp__RtlCompareUnicodeString);
REX_EXPORT_STUB(__imp__RtlCompareUnicodeStringN);
REX_EXPORT_STUB(__imp__RtlCompareUtf8ToUnicode);
REX_EXPORT_STUB(__imp__RtlCreateUnicodeString);
REX_EXPORT_STUB(__imp__RtlDowncaseUnicodeChar);
REX_EXPORT_STUB(__imp__RtlGetCallersAddress);
REX_EXPORT_STUB(__imp__RtlGetStackLimits);
REX_EXPORT_STUB(__imp__RtlLookupFunctionEntry);
REX_EXPORT_STUB(__imp__RtlUnicodeToUtf8);
REX_EXPORT_STUB(__imp__RtlUnicodeToUtf8Size);
REX_EXPORT_STUB(__imp__RtlUnwind2);
REX_EXPORT_STUB(__imp__RtlUpcaseUnicodeChar);
REX_EXPORT_STUB(__imp__RtlVirtualUnwind);
REX_EXPORT_STUB(__imp__RtlImageDirectoryEntryToData);
REX_EXPORT_STUB(__imp__RtlCaptureStackBackTrace);
REX_EXPORT_STUB(__imp__RtlSetVectoredExceptionHandler);
REX_EXPORT_STUB(__imp__RtlClearVectoredExceptionHandler);

}  // namespace rex::kernel::xboxkrnl

REX_EXPORT(__imp__RtlCompareMemory, rex::kernel::xboxkrnl::RtlCompareMemory_entry)
REX_EXPORT(__imp__RtlCompareMemoryUlong, rex::kernel::xboxkrnl::RtlCompareMemoryUlong_entry)
REX_EXPORT(__imp__RtlFillMemoryUlong, rex::kernel::xboxkrnl::RtlFillMemoryUlong_entry)
REX_EXPORT(__imp__RtlUpperChar, rex::kernel::xboxkrnl::RtlUpperChar_entry)
REX_EXPORT(__imp__RtlLowerChar, rex::kernel::xboxkrnl::RtlLowerChar_entry)
REX_EXPORT(__imp__RtlCompareString, rex::kernel::xboxkrnl::RtlCompareString_entry)
REX_EXPORT(__imp__RtlCompareStringN, rex::kernel::xboxkrnl::RtlCompareStringN_entry)
REX_EXPORT(__imp__RtlInitAnsiString, rex::kernel::xboxkrnl::RtlInitAnsiString_entry)
REX_EXPORT(__imp__RtlFreeAnsiString, rex::kernel::xboxkrnl::RtlFreeAnsiString_entry)
REX_EXPORT(__imp__RtlInitUnicodeString, rex::kernel::xboxkrnl::RtlInitUnicodeString_entry)
REX_EXPORT(__imp__RtlFreeUnicodeString, rex::kernel::xboxkrnl::RtlFreeUnicodeString_entry)
REX_EXPORT(__imp__RtlCopyString, rex::kernel::xboxkrnl::RtlCopyString_entry)
REX_EXPORT(__imp__RtlCopyUnicodeString, rex::kernel::xboxkrnl::RtlCopyUnicodeString_entry)
REX_EXPORT(__imp__RtlUnicodeStringToAnsiString,
           rex::kernel::xboxkrnl::RtlUnicodeStringToAnsiString_entry)
REX_EXPORT(__imp__RtlAnsiStringToUnicodeString,
           rex::kernel::xboxkrnl::RtlAnsiStringToUnicodeString_entry)
REX_EXPORT(__imp__RtlMultiByteToUnicodeN, rex::kernel::xboxkrnl::RtlMultiByteToUnicodeN_entry)
REX_EXPORT(__imp__RtlUnicodeToMultiByteN, rex::kernel::xboxkrnl::RtlUnicodeToMultiByteN_entry)
REX_EXPORT(__imp__RtlMultiByteToUnicodeSize, rex::kernel::xboxkrnl::RtlMultiByteToUnicodeSize_entry)
REX_EXPORT(__imp__RtlUnicodeToMultiByteSize, rex::kernel::xboxkrnl::RtlUnicodeToMultiByteSize_entry)
REX_EXPORT(__imp__RtlImageNtHeader, rex::kernel::xboxkrnl::RtlImageNtHeader_entry)
REX_EXPORT(__imp__RtlImageXexHeaderField, rex::kernel::xboxkrnl::RtlImageXexHeaderField_entry)
REX_EXPORT(__imp__RtlInitializeCriticalSection,
           rex::kernel::xboxkrnl::RtlInitializeCriticalSection_entry)
REX_EXPORT(__imp__RtlInitializeCriticalSectionAndSpinCount,
           rex::kernel::xboxkrnl::RtlInitializeCriticalSectionAndSpinCount_entry)
REX_EXPORT(__imp__RtlEnterCriticalSection, rex::kernel::xboxkrnl::RtlEnterCriticalSection_entry)
REX_EXPORT(__imp__RtlTryEnterCriticalSection,
           rex::kernel::xboxkrnl::RtlTryEnterCriticalSection_entry)
REX_EXPORT(__imp__RtlLeaveCriticalSection, rex::kernel::xboxkrnl::RtlLeaveCriticalSection_entry)
REX_EXPORT(__imp__RtlTimeToTimeFields, rex::kernel::xboxkrnl::RtlTimeToTimeFields_entry)
REX_EXPORT(__imp__RtlTimeFieldsToTime, rex::kernel::xboxkrnl::RtlTimeFieldsToTime_entry)
REX_EXPORT(__imp__RtlComputeCrc32, rex::kernel::xboxkrnl::RtlComputeCrc32_entry)
REX_EXPORT(__imp__RtlCaptureContext, rex::kernel::xboxkrnl::RtlCaptureContext_entry)
REX_EXPORT(__imp__RtlUnwind, rex::kernel::xboxkrnl::RtlUnwind_entry)
REX_EXPORT(__imp____C_specific_handler, rex::kernel::xboxkrnl::__C_specific_handler_entry)
