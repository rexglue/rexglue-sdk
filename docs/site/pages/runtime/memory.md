# Guest Memory

Guest memory is the 32 bit address space of the Xbox 360. Recompiled code keeps the guest addresses of the original code, so the runtime maps the whole address space into the host process, where recompiled code and host code read and write it. The PowerPC CPU of the console is big endian, so every value in guest memory is big endian and each access on a little endian host swaps the bytes.

## Host Mapping

The runtime maps the whole guest address space at one host base, the `u8* base` that every recompiled function receives.

- `GuestAddressSpace::GetVirtualMembase` returns it.
- Physical RAM is visible at `base + 0x100000000`.

## Address Translation

A guest address `A` maps to the host address `base + A`. `rex::memory::GuestPtr` and `rex::memory::HostPtrToGuest` translate in both directions.

!!! note "The range from 0xE0000000"
    The range from `0xE0000000` to `0xFFFFFFFF` shows physical memory one 4 KiB page in: guest address `0xE0000000` is physical address `0x1000`. The SDK maps this range as a view of the physical memory at offset `0x1000`.

    A host can only map a view at a multiple of its allocation granularity. Windows allocates in 64 KiB units and macOS on arm64 uses 16 KiB pages, so neither can start a view at offset `0x1000`. On those hosts the view starts at offset 0, and recompiled code adds `0x1000` to every address at or above `0xE0000000` through the `REX_PHYS_HOST_OFFSET` macro. Linux and macOS on x86_64 use 4 KiB pages and map the offset. On a host whose granularity does not match its platform, `TitleRuntime::Load` logs the granularity and fails.

## Address Map

The runtime divides the address space into heaps. A heap manages one address range in pages of one size, and tracks which pages are reserved, committed and with what protection. The address of an allocation selects its heap.

| Heap | Start | Size | Page size | Purpose |
|---|---|---|---|---|
| `v00000000` | `0x00000000` | 1 GiB | 4 KiB | Provides virtual memory. |
| `v40000000` | `0x40000000` | 1008 MiB | 64 KiB | Provides virtual memory. |
| None | `0x7F000000` | 16 MiB | None | Contains the MMIO register blocks: the GPU at `0x7FC80000` and the APU at `0x7FEA0000`. |
| `v80000000` | `0x80000000` | 256 MiB | 64 KiB | Contains the image area. |
| `v90000000` | `0x90000000` | 256 MiB | 4 KiB | Mirrors the image area at `0x80000000`. |
| `va0000000` | `0xA0000000` | 512 MiB | 64 KiB | Maps physical RAM. |
| `vc0000000` | `0xC0000000` | 512 MiB | 16 MiB | Maps physical RAM. |
| `ve0000000` | `0xE0000000` | 509 MiB | 4 KiB | Maps physical RAM offset by `0x1000`, up to `0xFFCFFFFF`. |
| `physical` | Host only | 512 MiB | 4 KiB | Provides physical RAM. |

- The heaps at `0xA0000000`, `0xC0000000` and `0xE0000000` are the three physical views. They show the same 512 MiB of RAM, and an allocation in any of them reserves the matching range in `physical`.
- `0x80000000` and `0x90000000` show the same bytes.
- A title image loads in `v80000000` or `v90000000`, depending on its base address.
- `NtAllocateEncryptedMemory` allocates from `0x8C000000` to `0x8FFFFFFF`, at most 16 MiB at a time. The SDK does not encrypt this memory.

### System Ranges

The two virtual heaps keep their top range for allocations that the system makes on behalf of the title, apart from the memory that the title allocates itself.

| Heap | Title allocations | System range |
|---|---|---|
| 4 KiB virtual | Below `0x30000000` | `0x30000000` to `0x3FFFFFFF` |
| 64 KiB virtual | Below `0x70000000` | `0x70000000` to `0x7EFFFFFF` |

`GuestAddressSpace::SystemHeapAlloc` allocates zeroed memory in the system range of the 4 KiB heap, or of `ve0000000` when the caller asks for physical memory.

Guest thread stacks are in `0x70000000` to `0x7F000000`, each with a no access guard page at its low end.

## MMIO Blocks

MMIO, memory mapped I/O, means that a device exposes its registers at memory addresses, so the CPU controls the device with plain loads and stores. On the Xbox 360, the `0x7F000000` range holds two 64 KiB register blocks, the only addresses where an access reaches a device:

- `MMIOBlock::kGPU` at `0x7FC80000`, the registers of the Xenos GPU
- `MMIOBlock::kAPU` at `0x7FEA0000`, the registers of the XMA audio decoder

Codegen recognizes a load or store to these blocks and turns it into a call to the recipient of the block. A recipient is the host object that implements the registers of one block. The pages of both blocks have no access, so an access that codegen did not recognize faults, and the [access violation handler](#access-violation-handler) reports an MMIO miss.

### Attaching a Recipient

An `MMIORecipient` from `rex/ppc/mmio.h` is a context pointer with a read function and a write function. `AttachMMIO` routes a block to a recipient and `DetachMMIO` removes the route.

- `TitleRuntime::Launch` attaches the GPU block, and routes it to the graphics system. The XMA decoder attaches the APU block when guest audio starts.
- Attach and detach only while no guest thread runs.
- A recipient must have both a read and a write function.
- `DetachMMIO(kGPU, context)` acts only when the block routes to that context.

### Misses

- A block with no recipient returns 0 for a read and discards a write, and logs a warning on the first access.
- A write narrower than 32 bits to a block is fatal.

## Reserved Pages

The first 64 KiB at `0x00000000` has no access while `runtime.protect_zero` is true, the default, so a null pointer dereference faults. With the setting false it has read and write access.

The runtime commits the first 256 KiB of `v80000000` and writes the value `0x2A6E3F38` at `0x8000001C`, because titles read a constant at that address.

## Title Budget

`runtime.title_memory_mb` caps the memory that a title can commit, in MiB.

- It defaults to 480 and refuses a value above 512.
- 0 removes the cap.
- Allocations with the flag `kMemoryAllocationTitle` count against it.

## Access Violation Handler

The runtime installs an access violation handler for faults inside the guest mapping:

- A fault in a register block is an MMIO miss. The crash text names the block, the access and the guest address.
- A fault on a page that the heap allows is a stale host protection. The handler applies the protection again and the access resumes.
- A write fault on a watched page of a physical view runs the write listeners, and the access resumes. See [Write Watches](#write-watches).

Any other fault is unhandled, and the crash text gives the guest address. For a write to a page of a physical view that is neither watched nor writable, it also gives the physical address and the protection of the page in the view and in the `physical` heap.

## Checked Access

Host code reaches guest memory through `GuestMemory` from `rex/kernel/guest_memory.h`, which `TitleRuntime::GetGuestMemory` and `REX_GUEST_MEMORY()` return. Unlike a raw pointer, each call checks the range first. A call returns a `kMemory` error and leaves guest memory unchanged when any page of the range lacks the access it needs or the range is outside a heap.

```cpp
REX_TRY(std::string name, memory.ReadString(name_ptr));
```

See `GuestMemory` in the API reference for the calls.

- One call cannot cross from one heap into the next. See [Address Map](#address-map).
- The access of a span is checked when the call returns the span, not when you use it. Spans and views are valid until `TitleRuntime::Unload`.
- At `max_length` without a terminator, `ReadString` and `ReadString16` return the characters read so far.
- A write through `CopyTo`, `Store` or a writable span to a watched page in a physical view calls the write listeners like a write from the title. See [Write Watches](#write-watches).

`GuestMemory::Allocate` takes a size, a power of two alignment, the protection `GuestProtect::kReadOnly` or `GuestProtect::kReadWrite`, and the heap:

| Heap | Purpose |
|---|---|
| `GuestHeap::kTitle` (default) | Allocates virtual memory in 64 KiB pages that does not count against the title budget. Needs an alignment of at least 4096. |
| `GuestHeap::kSystem` | Allocates zeroed memory from the system range. |
| `GuestHeap::kPhysical` | Allocates physical memory in 64 KiB pages at `0xA0000000` and counts it against the title budget. |

- `Free` needs the first address of an allocation.
- `Protect` needs a non empty range inside one allocation with every page committed. It sets every page from the one that holds the first byte to the one that holds the last byte, in the page size of the heap.
- In a physical view, a `Protect` to read write first calls the write listeners of the watched pages in the range.

## Heap Calls

The heaps themselves are declared in `rex/kernel/heap.h`. Their calls do fewer checks than `GuestMemory` and are meant for code that manages memory for the title.

- The allocation flags are `kMemoryAllocationReserve`, `kMemoryAllocationCommit` and `kMemoryAllocationTitle`.
- `BaseHeap::AllocFixed` fails a reserve when any page of the range is reserved.
- A commit may cover pages that are reserved or committed.
- `GuestAddressSpace::LookupHeap` returns null for the `0x7F000000` range and for the last 3 MiB of the address space.
- `GuestAddressSpace::TranslateVirtual` does not check the address.
- `GuestAddressSpace::TryTranslateVirtual` returns null for address 0, a misaligned address, an unmapped address or a page without the required access.
- `MemoryView` provides both translations, typed loads and stores, and string reads.

`GuestAllocator::Create` reserves and commits a range through a `GuestMemory` and serves blocks from it, such as a range of the physical heap with `physical` true and a 64 KiB page size. The title workspace of XAM is such an allocator. See [Title Workspace](kernel.md#title-workspace).

## Physical Memory

Physical memory is the 512 MiB of RAM of the console. The runtime maps it at host address `base + 0x100000000`. The `physical` heap tracks it in 4 KiB pages, and the three physical views map it into the guest address space:

- In `va0000000`, guest address `A` is physical address `A - 0xA0000000`.
- In `vc0000000`, guest address `A` is physical address `A - 0xC0000000`.
- In `ve0000000`, guest address `A` is physical address `A - 0xE0000000 + 0x1000`. See [Address Translation](#address-translation) for where the `0x1000` is added.
- `GuestAddressSpace::GetPhysicalAddress` returns the physical address of a view address.
- An allocation in a view rounds its size and alignment up to the page size of the view. `Alloc` takes the highest free range of `physical` that the view covers.
- Each view has its own page table, so the same physical page can have a different protection in each view.

Systems and drivers read and write physical memory through `IPhysicalMemory` from `rex/guest.h`, which takes physical addresses from `0` to `0x1FFFFFFF`. Its calls go through the host mapping at `base + 0x100000000`, which is committed read write for all 512 MiB, so they reach pages the title has not allocated or has protected. See [Driver Context](../devices/driver-context.md#physical-memory) for the calls.

## Write Watches

A write watch makes the next write to a physical page call the write listeners. `IPhysicalWriteWatch` from `rex/guest.h` sets watches. See [Driver Context](../devices/driver-context.md#write-watches) for its calls and the listener rules.

Watches are kept per host page, the size `rex::memory::GetPageSize` returns, and per physical view:

- `WatchPhysicalWrites` watches the host pages that cover the range in each of the three views.
- In each view, it watches only the pages whose protection there is read write, and sets their host protection to read only.

These writes call the listeners:

- A write through a view address to a watched page faults. The [access violation handler](#access-violation-handler) calls every listener on the writing thread with the physical range of that host page and `exact` false. This covers title code and host writes through `GuestMemory`.
- After the listeners return, the handler unwatches a range in the view that was written, restores write access to it, and resumes the write. The range is the overlap of the ranges the listeners return, extended to cover the written page and limited to the 4 MiB aligned block that holds it.
- `CopyToPhysical` writes the bytes, then calls the listeners once for each view that has a watched page in the range. It passes the host pages that cover the range and `exact` true, and unwatches exactly those pages in that view.
- Releasing or decommitting pages in a view, and setting them read write, calls the listeners for the watched pages of that view with `exact` true.
- Writes through `GetWritablePhysicalSpan`, `StoreRelease32` and other writes through the host mapping at `base + 0x100000000` call no listener.

A watch fires once. After it fires, the page stays unwatched in that view until `WatchPhysicalWrites` covers it again. A page watched in two views fires again on a write through the other view.

## Pointers and Byte Order

- `be_u16`, `be_u32`, `be_u64`, `be_f32`, `be_f64` and their signed forms swap on access.
- `be_u8` is `u8`.

| Type | Meaning |
|---|---|
| `MappedPtr` | Holds a host pointer and its guest address. Provides `MappedPtr::GetGuestAddress`, `MappedPtr::GetHostPtr`, and `+` and `-` in elements. |
| `MappedU32` | Is a `MappedPtr` of `be_u32`. `MappedU8`, `MappedU16`, `MappedU64`, `MappedF32` and `MappedF64` follow the same pattern. |
| `MappedVoid` | Uses byte offsets. `GetValue` returns the `u32` guest address. |
| `MappedString` | `GetValue` returns a `std::string_view`. |
| `MappedWString` | `GetValue` returns a `std::u16string_view`. |
| `GuestPtrT` | Is an alias of `MappedPtr`. |
