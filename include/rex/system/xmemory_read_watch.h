/**
 * @file        rex/system/xmemory_read_watch.h
 *
 * @brief       Read watches on guest physical memory ("data providers"): pages
 *              kept inaccessible until the guest first touches them, so the
 *              GPU emulation can put data there just in time.
 *
 * A watched page is inaccessible in each of the guest's physical views
 * (0xA0000000, 0xC0000000, 0xE0000000) where the guest can access it. The
 * first read or write of it, by guest code or host code going through those
 * views, calls the read callbacks on that thread before the access goes ahead,
 * and the page is unwatched in all views. Accesses through TranslatePhysical
 * (the host's own copies) are never caught.
 *
 * The watch is dropped without calling the callbacks where the memory is
 * written past the views (a file read into it, for example, which triggers the
 * write watch's invalidation), decommitted, released, or made writable again
 * with a protection change: the data the callbacks would put there is no
 * longer wanted.
 *
 * Separate from xmemory.h on purpose: every file of a recompiled game includes
 * that header, so changing it would rebuild the whole game.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstdint>
#include <mutex>
#include <utility>

namespace rex::memory {

class Memory;

// Called on the thread that touched a watched page, with the global critical
// region locked once through `global_lock`. A callback may unlock it (to wait
// for another thread, for instance), but must return with it locked again.
// Receives the physical range of the touched host page; returns a physical
// range to unwatch along with it (length 0 for none).
using PhysicalMemoryReadCallback = std::pair<uint32_t, uint32_t> (*)(
    void* context, std::unique_lock<std::recursive_mutex>& global_lock,
    uint32_t physical_address_start, uint32_t length);

void* RegisterPhysicalMemoryReadCallback(PhysicalMemoryReadCallback callback, void* context);
void UnregisterPhysicalMemoryReadCallback(void* callback_handle);

// Watches the host pages covering a physical range, in every view where the
// guest can access them. Pages already watched stay as they are.
void EnablePhysicalMemoryReadWatch(Memory& memory, uint32_t physical_address, uint32_t length);
// Stops watching a physical range. The read callbacks are called for it first
// if any page in it is still watched (unregister first to just stop).
void DisablePhysicalMemoryReadWatch(Memory& memory, uint32_t physical_address, uint32_t length);
// Whether the host page holding a physical address is watched in any view.
// The caller must hold the global critical region.
bool IsPhysicalMemoryReadWatched(Memory& memory, uint32_t physical_address);

}  // namespace rex::memory
