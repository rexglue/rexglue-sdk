#include <cstdint>
#include <cstring>
#include <format>

#include <rex/kernel/guest_memory.h>
#include <rex/kernel/memory_view.h>
#include <rex/kernel/heap.h>

namespace rex::kernel {

namespace {

constexpr u64 kAddressSpace = 0x100000000ull;

Result<void> BadRange(u32 address, u64 length, const char* reason) {
  return Err<void>(ErrorCategory::kMemory,
                   std::format("guest range 0x{:08X} length {} {}", address, length, reason));
}

}  // namespace

struct GuestMemory::State {
  kernel::GuestAddressSpace* memory = nullptr;

  Result<u8*> Resolve(u32 address, u64 length, u32 required) const {
    const u64 end = static_cast<u64>(address) + length;
    if (end > kAddressSpace) {
      return Err<u8*>(
          ErrorCategory::kMemory,
          std::format("guest range 0x{:08X} length {} wraps the address space", address, length));
    }
    kernel::BaseHeap* heap = memory->LookupHeap(address);
    if (heap == nullptr) {
      return Err<u8*>(ErrorCategory::kMemory,
                      std::format("guest address 0x{:08X} is in no heap", address));
    }
    const u64 heap_end = static_cast<u64>(heap->GetHeapBase()) + heap->GetHeapSize();
    if (end > heap_end) {
      return Err<u8*>(ErrorCategory::kMemory,
                      std::format("guest range 0x{:08X} length {} crosses the end of its heap",
                                  address, length));
    }
    if (length != 0) {
      const u64 page_size = heap->GetPageSize();
      for (u64 page = address & ~(page_size - 1); page < end; page += page_size) {
        u32 protect = 0;
        if (!heap->QueryProtect(static_cast<u32>(page), &protect) ||
            (protect & required) != required) {
          return Err<u8*>(ErrorCategory::kMemory,
                          std::format("guest page 0x{:08X} is not mapped with the needed access",
                                      static_cast<u32>(page)));
        }
      }
    }
    return memory->TranslateVirtual<u8*>(address);
  }
};

GuestMemory::GuestMemory(kernel::GuestAddressSpace& memory) : state_(new State{&memory}) {}

kernel::GuestAddressSpace& GuestMemory::GetAddressSpace() const {
  return *state_->memory;
}

GuestMemory::~GuestMemory() {
  delete state_;
}

Result<void> GuestMemory::CopyFrom(u32 address, std::span<std::byte> out) const {
  REX_TRY(u8 * source, state_->Resolve(address, out.size(), kernel::kMemoryProtectRead));
  std::memcpy(out.data(), source, out.size());
  return {};
}

Result<void> GuestMemory::CopyTo(u32 address, std::span<const std::byte> data) {
  REX_TRY(u8 * destination, state_->Resolve(address, data.size(), kernel::kMemoryProtectWrite));
  std::memcpy(destination, data.data(), data.size());
  return {};
}

Result<std::span<const std::byte>> GuestMemory::GetReadOnlySpan(u32 address, u32 length) const {
  REX_TRY(u8 * source, state_->Resolve(address, length, kernel::kMemoryProtectRead));
  return std::span<const std::byte>(reinterpret_cast<const std::byte*>(source), length);
}

Result<std::span<std::byte>> GuestMemory::GetWritableSpan(u32 address, u32 length) {
  REX_TRY(u8 * destination, state_->Resolve(address, length, kernel::kMemoryProtectWrite));
  return std::span<std::byte>(reinterpret_cast<std::byte*>(destination), length);
}

Result<std::string> GuestMemory::ReadString(u32 address, u32 max_length) const {
  std::string text;
  for (u32 i = 0; i < max_length; ++i) {
    u8 character = 0;
    REX_TRY_VOID(CopyFrom(address + i, std::as_writable_bytes(std::span(&character, 1))));
    if (character == 0) {
      return text;
    }
    text.push_back(static_cast<char>(character));
  }
  return text;
}

Result<std::u16string> GuestMemory::ReadString16(u32 address, u32 max_length) const {
  std::u16string text;
  for (u32 i = 0; i < max_length; ++i) {
    be_u16 character;
    REX_TRY_VOID(CopyFrom(address + i * 2, std::as_writable_bytes(std::span(&character, 1))));
    if (static_cast<u16>(character) == 0) {
      return text;
    }
    text.push_back(static_cast<char16_t>(static_cast<u16>(character)));
  }
  return text;
}

Result<u32> GuestMemory::Allocate(u32 size, u32 alignment, GuestProtect protect, GuestHeap where) {
  const u32 flags = protect == GuestProtect::kReadOnly
                        ? kernel::kMemoryProtectRead
                        : kernel::kMemoryProtectRead | kernel::kMemoryProtectWrite;
  if (where == GuestHeap::kSystem) {
    const u32 system_address = state_->memory->SystemHeapAlloc(size, alignment);
    if (system_address == 0) {
      return Err<u32>(ErrorCategory::kMemory,
                      std::format("cannot allocate {} bytes from the system heap", size));
    }
    if (protect == GuestProtect::kReadOnly) {
      kernel::BaseHeap* system_heap = state_->memory->LookupHeap(system_address);
      if (system_heap == nullptr || !system_heap->Protect(system_address, size, flags)) {
        state_->memory->SystemHeapFree(system_address);
        return Err<u32>(ErrorCategory::kMemory, "cannot protect the system allocation");
      }
    }
    return system_address;
  }
  const bool physical = where == GuestHeap::kPhysical;
  kernel::BaseHeap* heap = state_->memory->LookupHeapByType(physical, 0x10000);
  const u32 type = kernel::kMemoryAllocationReserve | kernel::kMemoryAllocationCommit |
                   (physical ? kernel::kMemoryAllocationTitle : 0);
  u32 address = 0;
  if (heap == nullptr || !heap->Alloc(size, alignment, type, flags, false, &address)) {
    return Err<u32>(ErrorCategory::kMemory, std::format("cannot allocate {} bytes", size));
  }
  return address;
}

Result<void> GuestMemory::Free(u32 address) {
  kernel::BaseHeap* heap = state_->memory->LookupHeap(address);
  if (heap == nullptr || !heap->Release(address)) {
    return BadRange(address, 0, "is not an allocation");
  }
  return {};
}

Result<void> GuestMemory::Protect(u32 address, u32 length, GuestProtect protect) {
  REX_TRY(u8 * mapped, state_->Resolve(address, length, 0));
  (void)mapped;
  kernel::BaseHeap* heap = state_->memory->LookupHeap(address);
  const u32 flags = protect == GuestProtect::kReadOnly
                        ? kernel::kMemoryProtectRead
                        : kernel::kMemoryProtectRead | kernel::kMemoryProtectWrite;
  if (!heap->Protect(address, length, flags)) {
    return BadRange(address, length, "cannot change its access");
  }
  return {};
}

kernel::MemoryView GuestMemory::GetView() const {
  return kernel::MemoryView(*state_->memory);
}

Result<u32> GuestMemory::HostToGuest(const void* host) const {
  const u8* membase = state_->memory->GetVirtualMembase();
  const auto base = reinterpret_cast<std::uintptr_t>(membase);
  const auto pointer = reinterpret_cast<std::uintptr_t>(host);
  if (host == nullptr || pointer < base || pointer - base >= kAddressSpace + 0x1000) {
    return Err<u32>(ErrorCategory::kMemory, "the pointer is outside guest memory");
  }
  return rex::memory::HostPtrToGuest(membase, host);
}

}  // namespace rex::kernel
