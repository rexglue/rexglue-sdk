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

// Disable warnings about unused parameters for kernel functions
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <algorithm>
#include <string>

#include <rex/chrono/chrono_steady_cast.h>
#include <rex/kernel/xboxkrnl/private.h>
#include <rex/kernel/xboxkrnl/rtl.h>
#include <rex/kernel/xboxkrnl/threading.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/string.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/util/string_utils.h>
#include <rex/system/xevent.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>
#include <rex/thread/atomic.h>

namespace rex::kernel::xboxkrnl {
using namespace rex::system;

// https://msdn.microsoft.com/en-us/library/ff561778
u32 RtlCompareMemory_entry(mapped_void source1, mapped_void source2, u32 length) {
  uint8_t* p1 = source1;
  uint8_t* p2 = source2;

  // Note that the return value is the number of bytes that match, so it's best
  // we just do this ourselves vs. using memcmp.
  // On Windows we could use the builtin function.

  uint32_t c = 0;
  for (uint32_t n = 0; n < length; n++, p1++, p2++) {
    if (*p1 == *p2) {
      c++;
    }
  }

  return c;
}

// https://msdn.microsoft.com/en-us/library/ff552123
u32 RtlCompareMemoryUlong_entry(mapped_void source, u32 length, u32 pattern) {
  // Return 0 if source/length not aligned
  if (source.guest_address() % 4 || length % 4) {
    return 0;
  }

  uint32_t n = 0;
  for (uint32_t i = 0; i < (length / 4); i++) {
    // FIXME: This assumes as_array returns rex::be
    uint32_t val = source.as_array<uint32_t>()[i];
    if (val == pattern) {
      n++;
    }
  }

  return n;
}

// https://msdn.microsoft.com/en-us/library/ff552263
void RtlFillMemoryUlong_entry(mapped_void destination, u32 length, u32 pattern) {
  // NOTE: length must be % 4, so we can work on uint32s.
  uint32_t count = length >> 2;

  uint32_t* p = destination.as<uint32_t*>();
  uint32_t swapped_pattern = rex::byte_swap(pattern);
  for (uint32_t n = 0; n < count; n++, p++) {
    *p = swapped_pattern;
  }
}

u32 RtlUpperChar_entry(u32 in) {
  char c = in & 0xFF;
  if (c >= 'a' && c <= 'z') {
    return c ^ 0x20;
  }

  return c;
}

u32 RtlLowerChar_entry(u32 in) {
  char c = in & 0xFF;
  if (c >= 'A' && c <= 'Z') {
    return c ^ 0x20;
  }

  return c;
}

u32 RtlCompareString_entry(mapped_string string_1, mapped_string string_2, u32 case_insensitive) {
  int ret = case_insensitive ? rex::string::compare_case(string_1, string_2)
                             : std::strcmp(string_1, string_2);

  return ret;
}

u32 RtlCompareStringN_entry(mapped_string string_1, u32 string_1_len, mapped_string string_2,
                            u32 string_2_len, u32 case_insensitive) {
  uint32_t len1 = string_1_len;
  uint32_t len2 = string_2_len;

  if (string_1_len == 0xFFFF) {
    len1 = uint32_t(std::strlen(string_1));
  }
  if (string_2_len == 0xFFFF) {
    len2 = uint32_t(std::strlen(string_2));
  }
  auto len = std::min(string_1_len, string_2_len);

  int ret = case_insensitive ? rex::string::compare_case_n(string_1, string_2, len)
                             : std::strncmp(string_1, string_2, len);

  return ret;
}

// https://msdn.microsoft.com/en-us/library/ff561918
void RtlInitAnsiString_entry(ppc_ptr_t<X_ANSI_STRING> destination, mapped_string source) {
  REXKRNL_IMPORT_TRACE("RtlInitAnsiString", "str={}", source ? source.value() : "(null)");
  if (source) {
    uint16_t length = (uint16_t)strlen(source);
    destination->length = length;
    destination->maximum_length = length + 1;
  } else {
    destination->reset();
  }

  destination->pointer = source.guest_address();
}

// https://msdn.microsoft.com/en-us/library/ff561899
void RtlFreeAnsiString_entry(ppc_ptr_t<X_ANSI_STRING> string) {
  if (string->pointer) {
    REX_KERNEL_MEMORY()->SystemHeapFree(string->pointer);
  }

  string->reset();
}

// https://msdn.microsoft.com/en-us/library/ff561934
void RtlInitUnicodeString_entry(ppc_ptr_t<X_UNICODE_STRING> destination, mapped_wstring source) {
  if (source) {
    destination->length = (uint16_t)source.value().size() * 2;
    destination->maximum_length = (uint16_t)(source.value().size() + 1) * 2;
    destination->pointer = source.guest_address();
  } else {
    destination->reset();
  }
}

// https://msdn.microsoft.com/en-us/library/ff561903
void RtlFreeUnicodeString_entry(ppc_ptr_t<X_UNICODE_STRING> string) {
  if (string->pointer) {
    REX_KERNEL_MEMORY()->SystemHeapFree(string->pointer);
  }

  string->reset();
}

void RtlCopyString_entry(ppc_ptr_t<X_ANSI_STRING> destination, ppc_ptr_t<X_ANSI_STRING> source) {
  if (!source) {
    destination->length = 0;
    return;
  }

  auto length = std::min(destination->maximum_length, source->length);
  if (length > 0) {
    auto dst_buf = REX_KERNEL_MEMORY()->TranslateVirtual(destination->pointer);
    auto src_buf = REX_KERNEL_MEMORY()->TranslateVirtual(source->pointer);
    std::memcpy(dst_buf, src_buf, length);
  }
  destination->length = length;
}

void RtlCopyUnicodeString_entry(ppc_ptr_t<X_UNICODE_STRING> destination,
                                ppc_ptr_t<X_UNICODE_STRING> source) {
  if (!source) {
    destination->length = 0;
    return;
  }

  auto length = std::min(destination->maximum_length, source->length);
  if (length > 0) {
    auto dst_buf = REX_KERNEL_MEMORY()->TranslateVirtual(destination->pointer);
    auto src_buf = REX_KERNEL_MEMORY()->TranslateVirtual(source->pointer);
    std::memcpy(dst_buf, src_buf, length * 2);
  }
  destination->length = length;
}

// https://msdn.microsoft.com/en-us/library/ff562969
u32 RtlUnicodeStringToAnsiString_entry(ppc_ptr_t<X_ANSI_STRING> destination_ptr,
                                       ppc_ptr_t<X_UNICODE_STRING> source_ptr, u32 alloc_dest) {
  // NTSTATUS
  // _Inout_  PANSI_STRING DestinationString,
  // _In_     PCUNICODE_STRING SourceString,
  // _In_     BOOLEAN AllocateDestinationString

  std::u16string unicode_str = util::TranslateUnicodeString(REX_KERNEL_MEMORY(), source_ptr);
  std::string ansi_str = rex::string::to_utf8(unicode_str);
  if (ansi_str.size() > 0xFFFF - 1) {
    return X_STATUS_INVALID_PARAMETER_2;
  }

  X_STATUS result = X_STATUS_SUCCESS;
  if (alloc_dest) {
    uint32_t buffer_ptr = REX_KERNEL_MEMORY()->SystemHeapAlloc(uint32_t(ansi_str.size() + 1));

    memcpy(REX_KERNEL_MEMORY()->TranslateVirtual(buffer_ptr), ansi_str.data(), ansi_str.size() + 1);
    destination_ptr->length = static_cast<uint16_t>(ansi_str.size());
    destination_ptr->maximum_length = static_cast<uint16_t>(ansi_str.size() + 1);
    destination_ptr->pointer = static_cast<uint32_t>(buffer_ptr);
  } else {
    uint32_t buffer_capacity = destination_ptr->maximum_length;
    auto buffer_ptr = REX_KERNEL_MEMORY()->TranslateVirtual(destination_ptr->pointer);
    if (buffer_capacity < ansi_str.size() + 1) {
      // Too large - we just write what we can.
      result = X_STATUS_BUFFER_OVERFLOW;
      memcpy(buffer_ptr, ansi_str.data(), buffer_capacity - 1);
    } else {
      memcpy(buffer_ptr, ansi_str.data(), ansi_str.size() + 1);
    }
    buffer_ptr[buffer_capacity - 1] = 0;  // \0
  }
  return result;
}

// https://msdn.microsoft.com/en-us/library/ff553113
u32 RtlMultiByteToUnicodeN_entry(mapped_u16 destination_ptr, u32 destination_len,
                                 mapped_u32 written_ptr, ppc_ptr_t<uint8_t> source_ptr,
                                 u32 source_len) {
  uint32_t copy_len = destination_len >> 1;
  copy_len = copy_len < source_len ? copy_len : source_len;

  // TODO(benvanik): maybe use MultiByteToUnicode on Win32? would require
  // swapping.

  for (uint32_t i = 0; i < copy_len; i++) {
    destination_ptr[i] = source_ptr[i];
  }

  if (written_ptr.guest_address() != 0) {
    *written_ptr = copy_len << 1;
  }

  return 0;
}

// https://msdn.microsoft.com/en-us/library/ff553261
u32 RtlUnicodeToMultiByteN_entry(ppc_ptr_t<uint8_t> destination_ptr, u32 destination_len,
                                 mapped_u32 written_ptr, mapped_u16 source_ptr, u32 source_len) {
  uint32_t copy_len = source_len >> 1;
  copy_len = copy_len < destination_len ? copy_len : destination_len;

  // TODO(benvanik): maybe use UnicodeToMultiByte on Win32?
  for (uint32_t i = 0; i < copy_len; i++) {
    uint16_t c = source_ptr[i];
    destination_ptr[i] = c < 256 ? (uint8_t)c : '?';
  }

  if (written_ptr.guest_address() != 0) {
    *written_ptr = copy_len;
  }
  return 0;
}

// https://undocumented.ntinternals.net/UserMode/Undocumented%20Functions/Executable%20Images/RtlImageNtHeader.html
u32 RtlImageNtHeader_entry(mapped_void module) {
  if (!module) {
    return 0;
  }

  // Little-endian! no swapping!

  auto dos_header = module.as<const uint8_t*>();
  auto dos_magic = *reinterpret_cast<const uint16_t*>(&dos_header[0x00]);
  if (dos_magic != 0x5A4D) {  // 'MZ'
    return 0;
  }
  auto dos_lfanew = *reinterpret_cast<const int32_t*>(&dos_header[0x3C]);

  auto nt_header = &dos_header[dos_lfanew];
  auto nt_magic = *reinterpret_cast<const uint32_t*>(&nt_header[0x00]);
  if (nt_magic != 0x4550) {  // 'PE'
    return 0;
  }
  return REX_KERNEL_MEMORY()->HostToGuestVirtual(nt_header);
}

u32 RtlImageXexHeaderField_entry(ppc_ptr_t<xex2_header> xex_header, u32 field_dword) {
  uint32_t field_value = 0;
  uint32_t field = field_dword;  // VS acts weird going from u32 -> enum

  UserModule::GetOptHeader(REX_KERNEL_MEMORY(), xex_header, xex2_header_keys(field), &field_value);

  return field_value;
}

// Unfortunately the Windows RTL_CRITICAL_SECTION object is bigger than the one
// on the 360 (32b vs. 28b). This means that we can't do in-place splatting of
// the critical sections. Also, the 360 never calls RtlDeleteCriticalSection
// so we can't clean up the native handles.
//
// Because of this, we reimplement it poorly. Hooray.
// We have 28b to work with so we need to be careful. We map our struct directly
// into guest memory, as it should be opaque and so long as our size is right
// the user code will never know.
//
// Ref:
// https://web.archive.org/web/20161214022602/https://msdn.microsoft.com/en-us/magazine/cc164040.aspx
// Ref:
// https://github.com/reactos/reactos/blob/master/sdk/lib/rtl/critical.c

// This structure tries to match the one on the 360 as best I can figure out.
// Unfortunately some games have the critical sections pre-initialized in
// their embedded data and InitializeCriticalSection will never be called.
#pragma pack(push, 1)
struct X_RTL_CRITICAL_SECTION {
  X_DISPATCH_HEADER header;
  int32_t lock_count;                // 0x10 -1 -> 0 on first lock
  rex::be<int32_t> recursion_count;  // 0x14  0 -> 1 on first lock
  rex::be<uint32_t> owning_thread;   // 0x18 PKTHREAD 0 unless locked
};
#pragma pack(pop)
static_assert_size(X_RTL_CRITICAL_SECTION, 28);

void xeRtlInitializeCriticalSection(X_RTL_CRITICAL_SECTION* cs, uint32_t cs_ptr) {
  cs->header.type = 1;      // EventSynchronizationObject (auto reset)
  cs->header.absolute = 0;  // spin count div 256
  cs->header.signal_state = 0;
  cs->lock_count = -1;
  cs->recursion_count = 0;
  cs->owning_thread = 0;
}

void RtlInitializeCriticalSection_entry(ppc_ptr_t<X_RTL_CRITICAL_SECTION> cs) {
  REXKRNL_IMPORT_TRACE("RtlInitializeCriticalSection", "cs={:#x}", cs.guest_address());
  xeRtlInitializeCriticalSection(cs, cs.guest_address());
}

X_STATUS xeRtlInitializeCriticalSectionAndSpinCount(X_RTL_CRITICAL_SECTION* cs, uint32_t cs_ptr,
                                                    uint32_t spin_count) {
  // Spin count is rounded up to 256 intervals then packed in.
  // uint32_t spin_count_div_256 = (uint32_t)floor(spin_count / 256.0f + 0.5f);
  uint32_t spin_count_div_256 = (spin_count + 255) >> 8;
  if (spin_count_div_256 > 255) {
    spin_count_div_256 = 255;
  }

  cs->header.type = 1;  // EventSynchronizationObject (auto reset)
  cs->header.absolute = spin_count_div_256;
  cs->header.signal_state = 0;
  cs->lock_count = -1;
  cs->recursion_count = 0;
  cs->owning_thread = 0;

  return X_STATUS_SUCCESS;
}

u32 RtlInitializeCriticalSectionAndSpinCount_entry(ppc_ptr_t<X_RTL_CRITICAL_SECTION> cs,
                                                   u32 spin_count) {
  return xeRtlInitializeCriticalSectionAndSpinCount(cs, cs.guest_address(), spin_count);
}

void RtlEnterCriticalSection_entry(ppc_ptr_t<X_RTL_CRITICAL_SECTION> cs) {
  uint32_t cur_thread = XThread::GetCurrentThread()->guest_object();
  uint32_t spin_count = cs->header.absolute * 256;

  if (cs->owning_thread == cur_thread) {
    // We already own the lock.
    rex::thread::atomic_inc(&cs->lock_count);
    cs->recursion_count++;
    return;
  }

  // Spin loop
  while (spin_count--) {
    if (rex::thread::atomic_cas(-1, 0, &cs->lock_count)) {
      // Acquired.
      cs->owning_thread = cur_thread;
      cs->recursion_count = 1;
      return;
    }
  }

  if (rex::thread::atomic_inc(&cs->lock_count) != 0) {
    // Create a full waiter.
    xeKeWaitForSingleObject(reinterpret_cast<void*>(cs.host_address()), 8, 0, 0, nullptr);
  }

  assert_true(cs->owning_thread == 0);
  cs->owning_thread = cur_thread;
  cs->recursion_count = 1;
}

u32 RtlTryEnterCriticalSection_entry(ppc_ptr_t<X_RTL_CRITICAL_SECTION> cs) {
  uint32_t thread = XThread::GetCurrentThread()->guest_object();

  if (rex::thread::atomic_cas(-1, 0, &cs->lock_count)) {
    // Able to steal the lock right away.
    cs->owning_thread = thread;
    cs->recursion_count = 1;
    return 1;
  } else if (cs->owning_thread == thread) {
    // Already own the lock.
    rex::thread::atomic_inc(&cs->lock_count);
    ++cs->recursion_count;
    return 1;
  }

  // Failed to acquire lock.
  return 0;
}

void RtlLeaveCriticalSection_entry(ppc_ptr_t<X_RTL_CRITICAL_SECTION> cs) {
  assert_true(cs->owning_thread == XThread::GetCurrentThread()->guest_object());

  // Drop recursion count - if it isn't zero we still have the lock.
  assert_true(cs->recursion_count > 0);
  if (--cs->recursion_count != 0) {
    assert_true(cs->recursion_count >= 0);

    rex::thread::atomic_dec(&cs->lock_count);
    return;
  }

  // Not owned - unlock!
  cs->owning_thread = 0;
  if (rex::thread::atomic_dec(&cs->lock_count) != -1) {
    // There were waiters - wake one of them.
    xeKeSetEvent(reinterpret_cast<X_KEVENT*>(cs.host_address()), 1, 0);
  }
}

struct X_TIME_FIELDS {
  rex::be<uint16_t> year;
  rex::be<uint16_t> month;
  rex::be<uint16_t> day;
  rex::be<uint16_t> hour;
  rex::be<uint16_t> minute;
  rex::be<uint16_t> second;
  rex::be<uint16_t> milliseconds;
  rex::be<uint16_t> weekday;
};
static_assert_size(X_TIME_FIELDS, 16);

// https://docs.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-rtltimetotimefields
void RtlTimeToTimeFields_entry(mapped_u64 time_ptr, ppc_ptr_t<X_TIME_FIELDS> time_fields_ptr) {
  // Use host clock because we don't want scaling to be applied, just conversion
  using rex::chrono::WinSystemClock;
  auto tp = WinSystemClock::to_sys(WinSystemClock::from_file_time(time_ptr.value()));
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
  time_fields_ptr->second = static_cast<uint16_t>(time.seconds().count());
  time_fields_ptr->milliseconds = static_cast<uint16_t>(time.subseconds().count());
}

// https://docs.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-rtltimefieldstotime
u32 RtlTimeFieldsToTime_entry(ppc_ptr_t<X_TIME_FIELDS> time_fields_ptr, mapped_u64 time_ptr) {
  using rex::chrono::WinSystemClock;
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
  std::chrono::system_clock::time_point time = dp;
  time += std::chrono::hours{time_fields_ptr->hour};
  time += std::chrono::minutes{time_fields_ptr->minute};
  time += std::chrono::seconds{time_fields_ptr->second};
  time += std::chrono::milliseconds{time_fields_ptr->milliseconds};
  *time_ptr = WinSystemClock::to_file_time(WinSystemClock::from_sys(time));
  return 1;
}

static uint32_t crc32_table[256] = {
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

u32 RtlComputeCrc32_entry(u32 seed, mapped_void buffer, u32 length) {
  if (!length) {
    return seed;
  }
  uint32_t hash = ~seed;
  for (uint32_t i = 0; i < length; ++i) {
    hash = crc32_table[buffer[i] ^ (hash & 0xFF)] ^ (hash >> 8);
  }
  return ~hash;
}

void RtlCaptureContext_entry() {
  // TODO(tomc): do we even need this?
  REXKRNL_WARN("[STUB] RtlCaptureContext called - not implemented");
}

void RtlUnwind_entry() {
  // TODO(tomc): do we even need this?
  REXKRNL_WARN("[STUB] RtlUnwind called - not implemented");
}

void __C_specific_handler_entry() {
  // TODO(tomc): do we even need this?
  REXKRNL_WARN("[STUB] __C_specific_handler called - not implemented");
}

REX_EXPORT_STUB(__imp__RtlAnsiStringToUnicodeString);
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
REX_EXPORT_STUB(__imp__RtlMultiByteToUnicodeSize);
REX_EXPORT_STUB(__imp__RtlUnicodeToMultiByteSize);
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
REX_EXPORT(__imp__RtlMultiByteToUnicodeN, rex::kernel::xboxkrnl::RtlMultiByteToUnicodeN_entry)
REX_EXPORT(__imp__RtlUnicodeToMultiByteN, rex::kernel::xboxkrnl::RtlUnicodeToMultiByteN_entry)
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
