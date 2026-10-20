/**
 * @file        rex/graphics/gpu_progress.h
 *
 * @brief       Lets a CPU thread that waits for the emulated GPU sleep until
 *              the GPU emulation actually moves on, instead of spinning.
 *
 * Xbox 360 games wait for the GPU by spinning on values the GPU writes to
 * memory: the ring buffer's read pointer, fences, frame counters. On the
 * console that spin costs a hardware thread the game owns anyway. On a PC it
 * keeps a core at 100% for as long as the game waits: power and boost clock
 * taken from the threads doing the work, and on a PC with few cores, a core
 * taken from them outright.
 *
 * The GPU emulation calls Signal() after every write a guest thread may be
 * waiting for (the read pointer write-back, memory and event writes from the
 * command stream, interrupts). A game hook on its spin loop can then Wait()
 * for the next signal and re-check. Wait() has a timeout, so a write this
 * doesn't cover costs a little latency, never a hang.
 *
 * Cost: Signal() is one atomic add and one atomic load while nobody waits; a
 * mutex and a wake-up while somebody does.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstdint>

// A game built against SDKs with and without this can test it.
#define REX_GPU_PROGRESS_VERSION 1

namespace rex::graphics::progress {

// How many times the GPU emulation has signaled progress so far.
uint64_t Count();

// GPU emulation: a guest-visible write a CPU thread may be waiting for has
// just been made.
void Signal();

// Blocks until Count() differs from `seen` or `timeout_us` microseconds pass,
// whichever is first; returns Count(). Read `seen` before checking what the
// wait is for, so a signal in between is not missed.
uint64_t Wait(uint64_t seen, uint32_t timeout_us);

}  // namespace rex::graphics::progress
