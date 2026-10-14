#include <rex/platform/crash.h>

#include <algorithm>
#include <bit>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <format>
#include <mutex>
#include <span>
#include <thread>
#include <typeinfo>

#include <rex/logging.h>
#include <rex/platform/file.h>
#include <rex/system.h>
#include <rex/version.h>

#include "core/crash_report.h"
#include "core/logger.h"

#if !REX_PLATFORM_WIN32
#include <cxxabi.h>
#endif

namespace rex::platform {

namespace {

constexpr auto kLogTimeout = std::chrono::milliseconds(200);
constexpr u64 kStackScanWords = 64;
constexpr u32 kStackScanHits = 24;
constexpr u64 kScanPageSize = 4096;
constexpr size_t kLineSize = 512;
constexpr size_t kCodeTextSize = 192;
constexpr size_t kModuleTextSize = 128;
constexpr size_t kDescriberTextSize = 256;

std::mutex g_install_mutex;
CrashReporterOptions g_options;
std::atomic<bool> g_installed{false};
std::atomic<CrashAddressDescriber> g_describer{nullptr};
std::atomic_flag g_reporting = ATOMIC_FLAG_INIT;
thread_local bool t_reporting = false;
std::terminate_handler g_previous_terminate = nullptr;
bool g_raw_output = false;
char g_line[kLineSize];
char g_code_text[kCodeTextSize];

void WriteLine(std::string_view text) {
#if !REX_PLATFORM_WIN32
  if (g_raw_output) {
    detail::WriteRaw(text);
    return;
  }
#endif
  if (!log::GetLoggerInstance().TryWrite(log::system, log::Level::kCritical, text, kLogTimeout)) {
    std::fwrite(text.data(), 1, text.size(), stderr);
    std::fputc('\n', stderr);
  }
}

template <typename... Args>
std::string_view FormatInto(std::span<char> out, std::format_string<Args...> format,
                            Args&&... args) {
  const auto result = std::format_to_n(out.data(), static_cast<std::ptrdiff_t>(out.size()), format,
                                       std::forward<Args>(args)...);
  return {out.data(), std::min(static_cast<size_t>(result.size), out.size())};
}

template <typename... Args>
void Line(std::format_string<Args...> format, Args&&... args) {
  WriteLine(FormatInto(g_line, format, std::forward<Args>(args)...));
}

std::string_view CodeText(u64 address) {
  char module[kModuleTextSize];
  const std::string_view name = detail::DescribeCode(address, module);
  return name.empty() ? FormatInto(g_code_text, "{:#018x}", address)
                      : FormatInto(g_code_text, "{:#018x} ({})", address, name);
}

void Banner() {
  Line("==== {} crash report, rexglue {} ====", g_options.app_name, REXGLUE_VERSION_STRING);
}

void LogRegisters(const HostThreadContext& context) {
  Line("registers:");
#if REX_ARCH_AMD64
  static constexpr const char* kNames[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp",
                                             "rsi", "rdi", "r8",  "r9",  "r10", "r11",
                                             "r12", "r13", "r14", "r15"};
  Line("    rip = {:#018x}   eflags = {:#010x}", context.rip, context.eflags);
  for (u32 i = 0; i < 16; ++i) {
    Line("    {:>3} = {:#018x}", kNames[i], context.int_registers[i]);
  }
#elif REX_ARCH_ARM64
  Line("    pc = {:#018x}   sp = {:#018x}   pstate = {:#010x}", context.pc, context.sp,
       context.pstate);
  for (u32 i = 0; i < 31; ++i) {
    Line("    x{:<2} = {:#018x}", i, context.x[i]);
  }
#endif
}

void LogStackCodeAddresses(const HostThreadContext& context) {
#if REX_ARCH_AMD64
  const u64 sp = context.rsp;
#elif REX_ARCH_ARM64
  const u64 sp = context.sp;
#else
  const u64 sp = 0;
#endif
  if (!sp || sp % sizeof(u64)) {
    return;
  }
  const u64 limit = std::min(sp + kStackScanWords * sizeof(u64), (sp | (kScanPageSize - 1)) + 1);
  Line("stack code addresses (sp {:#018x}):", sp);
  if (!detail::IsReadable(sp, limit - sp)) {
    Line("    unreadable");
    return;
  }
  u32 found = 0;
  for (u64 slot = sp; slot + sizeof(u64) <= limit && found < kStackScanHits; slot += sizeof(u64)) {
    u64 value = 0;
    std::memcpy(&value, std::bit_cast<const void*>(slot), sizeof(value));
    if (!value) {
      continue;
    }
    char text[kModuleTextSize];
    const std::string_view module = detail::DescribeCode(value, text);
    if (module.empty()) {
      continue;
    }
    Line("    [sp+{:#05x}] {:#018x} ({})", slot - sp, value, module);
    ++found;
  }
  if (!found) {
    Line("    none");
  }
}

std::string TypeName(const std::type_info& type) {
#if REX_PLATFORM_WIN32
  return type.name();
#else
  int status = 0;
  char* demangled = abi::__cxa_demangle(type.name(), nullptr, nullptr, &status);
  std::string name = status == 0 && demangled ? demangled : type.name();
  std::free(demangled);
  return name;
#endif
}

void DescribeContext(u64 address) {
  if (const CrashAddressDescriber describer = g_describer.load(std::memory_order_acquire)) {
    char text[kDescriberTextSize];
    const u32 length = describer(address, text);
    if (length) {
      Line("{}", std::string_view(text, std::min<size_t>(length, sizeof(text))));
    }
  }
}

[[noreturn]] void OnTerminate() {
  if (detail::BeginReport()) {
    std::string message = "std::terminate with no active exception";
    if (std::exception_ptr active = std::current_exception()) {
      try {
        std::rethrow_exception(active);
      } catch (const std::exception& error) {
        message = std::format("uncaught {}: {}", TypeName(typeid(error)), error.what());
      } catch (...) {
        message = "uncaught exception of a type not derived from std::exception";
      }
    }
    detail::ReportMessage(message);
    detail::EndReport();
  } else {
    detail::WaitForOtherReport();
  }
  std::abort();
}

}  // namespace

namespace detail {

bool BeginReport() {
  if (g_reporting.test_and_set(std::memory_order_acq_rel)) {
    return false;
  }
  t_reporting = true;
  return true;
}

void WaitForOtherReport() {
  if (t_reporting) {
    return;
  }
  for (;;) {
    std::this_thread::sleep_for(std::chrono::hours(1));
  }
}

#if !REX_PLATFORM_WIN32
void UseRawOutput() {
  g_raw_output = true;
}
#endif

bool IsCrashReporterInstalled() {
  return g_installed.load(std::memory_order_acquire);
}

void ReportFault(const FaultReport& fault) {
  Banner();
  Line("exception: {} ({:#x}) at {}", fault.kind, fault.code, CodeText(fault.pc));
  if (fault.fault_address) {
    Line("fault address: {:#018x} ({})", *fault.fault_address,
         fault.access.empty() ? std::string_view("unknown") : fault.access);
  }
  DescribeContext(fault.fault_address.value_or(0));
  if (fault.context) {
    LogRegisters(*fault.context);
    LogStackCodeAddresses(*fault.context);
  }
  if (!g_raw_output) {
    log::GetLoggerInstance().TryFlush(kLogTimeout);
  }
  Line("backtrace:");
  LogBacktrace();
}

void ReportMessage(std::string_view message) {
  Banner();
  Line("{}", message);
  DescribeContext(0);
  log::GetLoggerInstance().TryFlush(kLogTimeout);
  Line("backtrace:");
  LogBacktrace();
}

void EndReport() {
  Line("==== end of crash report ====");
  if (g_raw_output) {
    return;
  }
  log::GetLoggerInstance().TryFlush(kLogTimeout);
  std::fflush(stdout);
  std::fflush(stderr);
  if (g_options.show_dialog) {
    ShowSimpleMessageBox(
        SimpleMessageBoxType::kError,
        std::format("{} crashed. The log file has the details.", g_options.app_name));
  }
}

void WriteBacktraceFrame(u32 index, u64 address) {
  Line("    [{:>2}] {}", index, CodeText(address));
}

}  // namespace detail

[[nodiscard]] Result<void> InstallCrashReporter(const CrashReporterOptions& options) {
  std::lock_guard lock(g_install_mutex);
  g_options = options;
  if (g_options.app_name.empty()) {
    g_options.app_name = GetExecutablePath().stem().string();
  }
  PrepareThreadForCrash();
  if (g_installed.load(std::memory_order_acquire)) {
    return Ok();
  }
  REX_TRY_VOID(detail::InstallPlatform());
  g_previous_terminate = std::set_terminate(OnTerminate);
  g_installed.store(true, std::memory_order_release);
  return Ok();
}

void UninstallCrashReporter() {
  std::lock_guard lock(g_install_mutex);
  if (!g_installed.load(std::memory_order_acquire)) {
    return;
  }
  std::set_terminate(g_previous_terminate);
  g_previous_terminate = nullptr;
  detail::UninstallPlatform();
  g_installed.store(false, std::memory_order_release);
}

void SetCrashAddressDescriber(CrashAddressDescriber describer) {
  g_describer.store(describer, std::memory_order_release);
}

}  // namespace rex::platform
