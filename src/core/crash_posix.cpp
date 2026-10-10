#include <rex/platform/crash.h>

#include <rex/platform.h>

#if !REX_PLATFORM_WIN32

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <format>

#include <execinfo.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

#if REX_PLATFORM_MAC
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif

#include "core/crash_report.h"
#include "core/logger.h"
#if REX_PLATFORM_LINUX
#include "core/proc_maps.h"
#endif

namespace rex::platform {

namespace {

constexpr size_t kAltStackSize = size_t{128} * 1024;
constexpr int kBacktraceFrames = 32;
constexpr u32 kMaxRepeatedFaults = 64;
constexpr u32 kReportWatchdogSeconds = 10;
constexpr std::array kReporterSignals = {SIGSEGV, SIGILL, SIGBUS, SIGFPE, SIGABRT};

constexpr size_t kMaxModules = 512;
constexpr size_t kModuleNameSize = 96;

std::array<bool, kReporterSignals.size()> g_owned{};

struct MappedModule {
  u64 start = 0;
  u64 end = 0;
  u64 offset = 0;
  std::array<char, kModuleNameSize> name{};
  u8 name_length = 0;
};

std::array<MappedModule, kMaxModules> g_modules;
size_t g_module_count = 0;
bool g_modules_read = false;

struct AltStack {
  void* memory = nullptr;

  ~AltStack() {
    if (!memory) {
      return;
    }
    stack_t disable{};
    disable.ss_flags = SS_DISABLE;
    sigaltstack(&disable, nullptr);
    munmap(memory, kAltStackSize);
  }
};

thread_local AltStack t_alt_stack;

std::string_view SignalName(int signal_number) {
  switch (signal_number) {
    case SIGSEGV:
      return "SIGSEGV";
    case SIGILL:
      return "SIGILL";
    case SIGBUS:
      return "SIGBUS";
    case SIGFPE:
      return "SIGFPE";
    case SIGABRT:
      return "SIGABRT";
    default:
      return "signal";
  }
}

bool ReadContext(void* signal_context, HostThreadContext& out, std::string_view& access) {
  access = "unknown";
  if (!signal_context) {
    return false;
  }
  auto* ucontext = static_cast<ucontext_t*>(signal_context);
#if REX_ARCH_AMD64
  constexpr u64 kWriteBit = u64{1} << 1;
#if REX_PLATFORM_MAC
  const auto& state = ucontext->uc_mcontext->__ss;
  out.rip = state.__rip;
  out.eflags = static_cast<u32>(state.__rflags);
  const u64 registers[16] = {state.__rax, state.__rcx, state.__rdx, state.__rbx,
                             state.__rsp, state.__rbp, state.__rsi, state.__rdi,
                             state.__r8,  state.__r9,  state.__r10, state.__r11,
                             state.__r12, state.__r13, state.__r14, state.__r15};
  access = (ucontext->uc_mcontext->__es.__err & kWriteBit) ? "write" : "read";
#else
  const auto& gregs = ucontext->uc_mcontext.gregs;
  out.rip = static_cast<u64>(gregs[REG_RIP]);
  out.eflags = static_cast<u32>(gregs[REG_EFL]);
  const u64 registers[16] = {static_cast<u64>(gregs[REG_RAX]), static_cast<u64>(gregs[REG_RCX]),
                             static_cast<u64>(gregs[REG_RDX]), static_cast<u64>(gregs[REG_RBX]),
                             static_cast<u64>(gregs[REG_RSP]), static_cast<u64>(gregs[REG_RBP]),
                             static_cast<u64>(gregs[REG_RSI]), static_cast<u64>(gregs[REG_RDI]),
                             static_cast<u64>(gregs[REG_R8]),  static_cast<u64>(gregs[REG_R9]),
                             static_cast<u64>(gregs[REG_R10]), static_cast<u64>(gregs[REG_R11]),
                             static_cast<u64>(gregs[REG_R12]), static_cast<u64>(gregs[REG_R13]),
                             static_cast<u64>(gregs[REG_R14]), static_cast<u64>(gregs[REG_R15])};
  access = (static_cast<u64>(gregs[REG_ERR]) & kWriteBit) ? "write" : "read";
#endif
  std::memcpy(out.int_registers, registers, sizeof(registers));
#elif REX_ARCH_ARM64
#if REX_PLATFORM_MAC
  const auto& state = ucontext->uc_mcontext->__ss;
  std::memcpy(out.x, state.__x, sizeof(state.__x));
#if __DARWIN_OPAQUE_ARM_THREAD_STATE64
  out.x[29] = reinterpret_cast<u64>(state.__opaque_fp);
  out.x[30] = reinterpret_cast<u64>(state.__opaque_lr);
  out.sp = reinterpret_cast<u64>(state.__opaque_sp);
  out.pc = reinterpret_cast<u64>(state.__opaque_pc);
#else
  out.x[29] = state.__fp;
  out.x[30] = state.__lr;
  out.sp = state.__sp;
  out.pc = state.__pc;
#endif
  out.pstate = state.__cpsr;
  const u32 esr = ucontext->uc_mcontext->__es.__esr;
  if (((esr >> 26) & 0b111110) == 0b100100) {
    access = (esr & (1u << 6)) ? "write" : "read";
  }
#else
  std::memcpy(out.x, ucontext->uc_mcontext.regs, sizeof(out.x));
  out.sp = ucontext->uc_mcontext.sp;
  out.pc = ucontext->uc_mcontext.pc;
  out.pstate = ucontext->uc_mcontext.pstate;
#endif
#endif
  return true;
}

u64 ContextPC(const HostThreadContext& context) {
#if REX_ARCH_AMD64
  return context.rip;
#elif REX_ARCH_ARM64
  return context.pc;
#else
  return 0;
#endif
}

void WriteAll(int fd, std::string_view text) {
  while (!text.empty()) {
    const ssize_t written = write(fd, text.data(), text.size());
    if (written <= 0) {
      return;
    }
    text.remove_prefix(static_cast<size_t>(written));
  }
}

void ReadModules() {
  g_modules_read = true;
#if REX_PLATFORM_LINUX
  memory::ForEachMapping([](const memory::ProcMapping& mapping) {
    std::string_view name = mapping.path;
    if (mapping.perms[2] != 'x' || name.empty() || name.front() == '[') {
      return true;
    }
    if (const size_t slash = name.rfind('/'); slash != std::string_view::npos) {
      name.remove_prefix(slash + 1);
    }
    if (name.empty()) {
      return true;
    }
    MappedModule& module = g_modules[g_module_count++];
    module.start = mapping.start;
    module.end = mapping.end;
    module.offset = mapping.offset;
    name = name.substr(0, module.name.size());
    std::ranges::copy(name, module.name.begin());
    module.name_length = static_cast<u8>(name.size());
    return g_module_count < g_modules.size();
  });
#endif
}

void ReporterSignal(int signal_number, siginfo_t* info, void* signal_context) {
  detail::ReportFatalSignal(signal_number, info, signal_context);
  detail::DieWithDefaultDisposition(signal_number);
}

}  // namespace

namespace detail {

[[noreturn]] void DieWithDefaultDisposition(int signal_number) {
  struct sigaction default_action{};
  default_action.sa_handler = SIG_DFL;
  sigemptyset(&default_action.sa_mask);
  sigaction(signal_number, &default_action, nullptr);
  sigset_t unblock;
  sigemptyset(&unblock);
  sigaddset(&unblock, signal_number);
  pthread_sigmask(SIG_UNBLOCK, &unblock, nullptr);
  raise(signal_number);
  _exit(128 + signal_number);
}

void ReportFatalSignal(int signal_number, siginfo_t* info, void* signal_context) {
  if (!IsCrashReporterInstalled()) {
    return;
  }
  if (!BeginReport()) {
    WaitForOtherReport();
    return;
  }
  UseRawOutput();
  struct sigaction watchdog{};
  watchdog.sa_handler = SIG_DFL;
  sigemptyset(&watchdog.sa_mask);
  sigaction(SIGALRM, &watchdog, nullptr);
  alarm(kReportWatchdogSeconds);
  HostThreadContext context{};
  std::string_view access;
  const bool has_context = ReadContext(signal_context, context, access);
  FaultReport fault;
  fault.kind = SignalName(signal_number);
  fault.code = static_cast<u64>(signal_number);
  fault.pc = has_context ? ContextPC(context) : 0;
  fault.context = has_context ? &context : nullptr;
  if (info && signal_number != SIGABRT) {
    fault.fault_address = reinterpret_cast<u64>(info->si_addr);
    if (signal_number == SIGSEGV || signal_number == SIGBUS) {
      fault.access = access;
    }
  }
  ReportFault(fault);
  EndReport();
}

void ForwardSignal(struct sigaction& original, int signal_number, siginfo_t* info,
                   void* signal_context) {
  thread_local int last_signal = 0;
  thread_local void* last_address = nullptr;
  thread_local u32 repeats = 0;
  void* address = info ? info->si_addr : nullptr;
  if (signal_number == last_signal && address == last_address) {
    ++repeats;
  } else {
    last_signal = signal_number;
    last_address = address;
    repeats = 0;
  }
  if (original.sa_handler == SIG_DFL || original.sa_handler == SIG_IGN ||
      repeats >= kMaxRepeatedFaults) {
    ReportFatalSignal(signal_number, info, signal_context);
    DieWithDefaultDisposition(signal_number);
  }
  const struct sigaction called = original;
  if (original.sa_flags & SA_RESETHAND) {
    original = {};
    original.sa_handler = SIG_DFL;
  }
  sigset_t previous;
  pthread_sigmask(SIG_BLOCK, &called.sa_mask, &previous);
  if (called.sa_flags & SA_SIGINFO) {
    called.sa_sigaction(signal_number, info, signal_context);
  } else {
    called.sa_handler(signal_number);
  }
  pthread_sigmask(SIG_SETMASK, &previous, nullptr);
}

bool IsReadable(u64 address, u64 size) {
  std::array<int, 2> pipe_fds{};
  if (pipe(pipe_fds.data()) != 0) {
    return false;
  }
  bool readable = true;
  for (u64 offset = 0; offset < size && readable;) {
    const u64 chunk = std::min<u64>(size - offset, 512);
    const ssize_t written = write(pipe_fds[1], std::bit_cast<const void*>(address + offset), chunk);
    readable = written == static_cast<ssize_t>(chunk);
    std::array<u8, 512> sink{};
    if (written > 0 && read(pipe_fds[0], sink.data(), static_cast<size_t>(written)) < 0) {
      readable = false;
    }
    offset += chunk;
  }
  close(pipe_fds[0]);
  close(pipe_fds[1]);
  return readable;
}

void WriteRaw(std::string_view text) {
  const int log_fd = log::GetLoggerInstance().crash_fd();
  for (const int fd : {STDERR_FILENO, log_fd}) {
    if (fd >= 0) {
      WriteAll(fd, text);
      WriteAll(fd, "\n");
    }
  }
}

std::string_view DescribeCode(u64 address, std::span<char> out) {
  if (!g_modules_read) {
    ReadModules();
  }
  const auto modules = std::span(g_modules).first(g_module_count);
  const auto found = std::ranges::find_if(modules, [address](const MappedModule& module) {
    return address >= module.start && address < module.end;
  });
  if (found == modules.end()) {
    return {};
  }
  const auto result =
      std::format_to_n(out.data(), static_cast<std::ptrdiff_t>(out.size()), "{}+{:#010x}",
                       std::string_view(found->name.data(), found->name_length),
                       address - found->start + found->offset);
  return {out.data(), std::min(static_cast<size_t>(result.size), out.size())};
}

void LogBacktrace() {
  void* frames[kBacktraceFrames] = {};
  const int count = backtrace(frames, kBacktraceFrames);
  for (int i = 0; i < count; ++i) {
    WriteBacktraceFrame(static_cast<u32>(i), reinterpret_cast<u64>(frames[i]));
  }
}

[[nodiscard]] Result<void> InstallPlatform() {
  void* primed = nullptr;
  backtrace(&primed, 1);
  for (size_t i = 0; i < kReporterSignals.size(); ++i) {
    struct sigaction current{};
    if (sigaction(kReporterSignals[i], nullptr, &current) != 0) {
      return Err(ErrorCategory::Platform, "signal action could not be read");
    }
    if (current.sa_handler != SIG_DFL) {
      continue;
    }
    struct sigaction action{};
    action.sa_sigaction = ReporterSignal;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    if (sigaction(kReporterSignals[i], &action, nullptr) != 0) {
      return Err(ErrorCategory::Platform, "signal action could not be installed");
    }
    g_owned[i] = true;
  }
  return Ok();
}

void UninstallPlatform() {
  for (size_t i = 0; i < kReporterSignals.size(); ++i) {
    if (!g_owned[i]) {
      continue;
    }
    struct sigaction current{};
    sigaction(kReporterSignals[i], nullptr, &current);
    if ((current.sa_flags & SA_SIGINFO) && current.sa_sigaction == ReporterSignal) {
      struct sigaction default_action{};
      default_action.sa_handler = SIG_DFL;
      sigemptyset(&default_action.sa_mask);
      sigaction(kReporterSignals[i], &default_action, nullptr);
    }
    g_owned[i] = false;
  }
}

}  // namespace detail

void PrepareThreadForCrash() {
  if (t_alt_stack.memory) {
    return;
  }
  stack_t current{};
  if (sigaltstack(nullptr, &current) == 0 && !(current.ss_flags & SS_DISABLE)) {
    return;
  }
  void* memory =
      mmap(nullptr, kAltStackSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (memory == MAP_FAILED) {
    return;
  }
  stack_t stack{};
  stack.ss_sp = memory;
  stack.ss_size = kAltStackSize;
  if (sigaltstack(&stack, nullptr) != 0) {
    munmap(memory, kAltStackSize);
    return;
  }
  t_alt_stack.memory = memory;
}

}  // namespace rex::platform

#endif
