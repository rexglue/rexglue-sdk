#include <cstddef>
#include <format>
#include <iterator>
#include <optional>
#include <string>
#include <utility>

#include <rex/logging/log.h>
#include <rex/ppc/detour.h>
#include <rex/runtime.h>

static_assert(offsetof(PPCContext, f31) - offsetof(PPCContext, f0) == 31 * sizeof(PPCRegister));
static_assert(offsetof(PPCContext, v127) - offsetof(PPCContext, v0) == 127 * sizeof(PPCVRegister));

namespace rex::ppc::detail {

namespace {

using Register = PPCRegister PPCContext::*;
using CRField = PPCCRRegister PPCContext::*;

constexpr std::pair<Register, size_t> kNonArgumentGPRs[] = {
    {&PPCContext::r0, 0},
    {&PPCContext::r11, 11},
    {&PPCContext::r12, 12},
};

constexpr std::pair<Register, size_t> kNonVolatileGPRs[] = {
    {&PPCContext::r14, 14}, {&PPCContext::r15, 15}, {&PPCContext::r16, 16}, {&PPCContext::r17, 17},
    {&PPCContext::r18, 18}, {&PPCContext::r19, 19}, {&PPCContext::r20, 20}, {&PPCContext::r21, 21},
    {&PPCContext::r22, 22}, {&PPCContext::r23, 23}, {&PPCContext::r24, 24}, {&PPCContext::r25, 25},
    {&PPCContext::r26, 26}, {&PPCContext::r27, 27}, {&PPCContext::r28, 28}, {&PPCContext::r29, 29},
    {&PPCContext::r30, 30}, {&PPCContext::r31, 31},
};

constexpr CRField kCRFields[] = {
    &PPCContext::cr0, &PPCContext::cr1, &PPCContext::cr2, &PPCContext::cr3,
    &PPCContext::cr4, &PPCContext::cr5, &PPCContext::cr6, &PPCContext::cr7,
};

template <size_t N>
std::string FindChangedGPR(const std::pair<Register, size_t> (&registers)[N],
                           const PPCContext& at_entry, const PPCContext& at_exit) {
  for (const auto& [member, index] : registers) {
    if ((at_entry.*member).u64 != (at_exit.*member).u64) {
      return std::format("r{}", index);
    }
  }
  return {};
}

std::string FindChangedFPR(size_t first, size_t last, const PPCContext& at_entry,
                           const PPCContext& at_exit) {
  for (size_t index = first; index <= last; ++index) {
    if ((&at_entry.f0)[index].u64 != (&at_exit.f0)[index].u64) {
      return std::format("f{}", index);
    }
  }
  return {};
}

std::string FindChangedVR(size_t first, size_t last, const PPCContext& at_entry,
                          const PPCContext& at_exit) {
  for (size_t index = first; index <= last; ++index) {
    const PPCVRegister& before = (&at_entry.v0)[index];
    const PPCVRegister& after = (&at_exit.v0)[index];
    if (before.u64[0] != after.u64[0] || before.u64[1] != after.u64[1]) {
      return std::format("v{}", index);
    }
  }
  return {};
}

std::string FindChangedRegister(const PPCCodegenFlags& flags, const PPCContext& at_entry,
                                const PPCContext& at_exit) {
  std::string changed;
  if (flags.non_argument_as_local) {
    changed = FindChangedGPR(kNonArgumentGPRs, at_entry, at_exit);
    if (changed.empty()) {
      changed = FindChangedFPR(0, 0, at_entry, at_exit);
    }
    if (changed.empty()) {
      changed = FindChangedVR(32, 63, at_entry, at_exit);
    }
  }
  if (changed.empty() && flags.non_volatile_as_local) {
    changed = FindChangedGPR(kNonVolatileGPRs, at_entry, at_exit);
    if (changed.empty()) {
      changed = FindChangedFPR(14, 31, at_entry, at_exit);
    }
    if (changed.empty()) {
      changed = FindChangedVR(14, 31, at_entry, at_exit);
    }
    if (changed.empty()) {
      changed = FindChangedVR(64, 127, at_entry, at_exit);
    }
  }
  if (changed.empty() && flags.cr_as_local) {
    for (size_t index = 0; index < std::size(kCRFields); ++index) {
      if ((at_entry.*kCRFields[index]).GetRaw() != (at_exit.*kCRFields[index]).GetRaw()) {
        changed = std::format("cr{}", index);
        break;
      }
    }
  }
  if (changed.empty() && flags.ctr_as_local && at_entry.ctr.u64 != at_exit.ctr.u64) {
    changed = "ctr";
  }
  if (changed.empty() && flags.xer_as_local &&
      (at_entry.xer.so != at_exit.xer.so || at_entry.xer.ov != at_exit.xer.ov ||
       at_entry.xer.ca != at_exit.xer.ca)) {
    changed = "xer";
  }
  if (changed.empty() && flags.reserved_as_local && at_entry.reserved.u64 != at_exit.reserved.u64) {
    changed = "reserved";
  }
  return changed;
}

}  // namespace

DetourFrame*& GetInnermostDetourFrame() {
  thread_local DetourFrame* innermost = nullptr;
  return innermost;
}

std::string CheckDetourLocalWrites(const char* name, const PPCCodegenFlags& flags,
                                   const PPCContext& at_entry, const PPCContext& at_exit,
                                   std::atomic_flag& warned) {
  std::string changed = FindChangedRegister(flags, at_entry, at_exit);
  if (!changed.empty() && !warned.test_and_set()) {
    REXLOG_WARN(cpu,
                "REX_DETOUR {} wrote {} through ctx, but the image keeps it in a local and the "
                "caller never reads the write",
                name, changed);
  }
  return changed;
}

void CheckBoundDetourLocalWrites(const char* name, const PPCContext& at_entry,
                                 const PPCContext& at_exit, std::atomic_flag& warned) {
  const rex::runtime::ThreadState* state = rex::runtime::ThreadState::Get();
  if (state == nullptr || state->GetRuntime() == nullptr) {
    return;
  }
  CheckDetourLocalWrites(name, state->GetRuntime()->GetCodegenFlags(), at_entry, at_exit, warned);
}

bool CheckDetourArguments(const char* name, const PPCCallUsage& usage, u8 declared_gpr,
                          u16 declared_fpr) {
  const u32 missing_gpr = usage.gpr_mask & ~u32{declared_gpr};
  const u32 missing_fpr = usage.fpr_mask & ~u32{declared_fpr};
  if (missing_gpr == 0 && missing_fpr == 0) {
    return false;
  }
  std::string registers;
  for (u32 bit = 0; bit < 8; ++bit) {
    if ((missing_gpr >> bit) & 1) {
      registers += std::format("{}r{}", registers.empty() ? "" : ", ", 3 + bit);
    }
  }
  for (u32 bit = 0; bit < 13; ++bit) {
    if ((missing_fpr >> bit) & 1) {
      registers += std::format("{}f{}", registers.empty() ? "" : ", ", 1 + bit);
    }
  }
  REXLOG_WARN(cpu, "REX_DETOUR {} does not declare {}, which the function reads at entry", name,
              registers);
  return true;
}

void CheckBoundDetourArguments(const char* name, PPCFunc* host, u8 declared_gpr, u16 declared_fpr) {
  const rex::runtime::ThreadState* state = rex::runtime::ThreadState::Get();
  if (state == nullptr || state->GetRuntime() == nullptr) {
    return;
  }
  const std::optional<PPCCallUsage> usage = state->GetRuntime()->GetCallUsage(host);
  if (usage) {
    CheckDetourArguments(name, *usage, declared_gpr, declared_fpr);
  }
}

}  // namespace rex::ppc::detail
