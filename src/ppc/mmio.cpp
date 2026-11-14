#include <rex/logging/log_category.h>
#include <rex/ppc/mmio.h>

#include <atomic>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>

#include <rex/assert.h>

namespace rex::ppc {

namespace {

std::atomic_flag g_null_warned[2];

std::string_view BlockName(MMIOBlock block) {
  return block == MMIOBlock::kGPU ? "GPU" : "APU";
}

void WarnNullRecipient(u32 address, bool is_write) {
  const MMIOBlock block = MMIOBlockOf(address).value_or(MMIOBlock::kGPU);
  if (!g_null_warned[static_cast<size_t>(block)].test_and_set(std::memory_order_relaxed)) {
    REXLOG_WARN("No {} recipient is attached, the {} of 0x{:08X} {}", BlockName(block),
                is_write ? "write" : "read", address, is_write ? "is dropped" : "returns 0");
  }
}

u32 NullRead(void*, u32 address) {
  WarnNullRecipient(address, false);
  return 0;
}

void NullWrite(void*, u32 address, u32) {
  WarnNullRecipient(address, true);
}

constexpr MMIORecipient kNullRecipient{nullptr, NullRead, NullWrite};

MMIORecipient& Slot(MMIOBlock block) {
  return detail::mmio_slots[static_cast<size_t>(block)];
}

}  // namespace

namespace detail {

MMIORecipient mmio_slots[2]{kNullRecipient, kNullRecipient};

}  // namespace detail

u32 MMIOLoad32(u32 address) {
  const auto block = MMIOBlockOf(address);
  if (!block) {
    return 0;
  }
  const MMIORecipient& slot = Slot(*block);
  return slot.read(slot.context, address);
}

void MMIOStore32(u32 address, u32 value) {
  const auto block = MMIOBlockOf(address);
  if (!block) {
    return;
  }
  const MMIORecipient& slot = Slot(*block);
  slot.write(slot.context, address, value);
}

u8 MMIOLoad8(u32 address) {
  const u32 shift = (3 - (address & 3)) * 8;
  return static_cast<u8>(MMIOLoad32(address & ~3u) >> shift);
}

u16 MMIOLoad16(u32 address) {
  const u32 shift = (2 - (address & 2)) * 8;
  return static_cast<u16>(MMIOLoad32(address & ~3u) >> shift);
}

void MMIONarrowStore(u32 address, u32 size) {
  const std::string message = std::format(
      "A {} byte store to the register at 0x{:08X} is narrower than the register", size, address);
  REX_FATAL("{}", message);
}

void AttachMMIO(MMIOBlock block, const MMIORecipient& recipient) {
  if (!recipient.read || !recipient.write) {
    REXLOG_ERROR("Refused a {} recipient with no read or no write handler", BlockName(block));
    return;
  }
  Slot(block) = recipient;
}

void DetachMMIO(MMIOBlock block, const void* context) {
  MMIORecipient& slot = Slot(block);
  if (slot.context == context) {
    slot = kNullRecipient;
  }
}

}  // namespace rex::ppc
