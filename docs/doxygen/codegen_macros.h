/**
 * @file        codegen_macros.h
 * @brief       Macros that the generated precompiled header defines, listed for the reference
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * Documentation only. `rexglue codegen` writes the real definitions into `<p>_pch.h` from the
 * template `codegen/pch_h.inja`, and nothing includes this file.
 */

#pragma once

/**
 * Declares a recompiled function and its original entry point `__imp__name`.
 *
 * @param name The function name.
 */
#define DECLARE_REX_FUNC(name)

/**
 * Starts the body of a recompiled function. Defines `__imp__name` and makes `name` a weak alias of
 * it, so a hook that defines a strong `name` replaces the function and the original stays
 * reachable. The body is placed in the `rexguest` section.
 *
 * @param name The function name.
 */
#define DEFINE_REX_FUNC(name)

/**
 * Declares that `base` is 32 byte aligned. Every recompiled function starts with it.
 */
#define REX_FUNC_PROLOGUE()

/**
 * The extra host offset of a guest address in the physical range from `0xE0000000`, which is
 * `0x1000` on Windows and macOS on arm64, where the host mapping rounds the offset away, and 0
 * elsewhere.
 *
 * @param addr The guest address.
 */
#define REX_PHYS_HOST_OFFSET(addr)

/**
 * The host address of a guest address, with no MMIO test.
 *
 * @param x The guest address.
 */
#define REX_RAW_ADDR(x)

/**
 * A volatile byte load at `base + x`.
 *
 * @param x The guest address.
 */
#define REX_LOAD_U8(x)

/**
 * A volatile 16 bit load at `base + x`, byte swapped.
 *
 * @param x The guest address.
 */
#define REX_LOAD_U16(x)

/**
 * A volatile 32 bit load at `base + x`, byte swapped.
 *
 * @param x The guest address.
 */
#define REX_LOAD_U32(x)

/**
 * A volatile 64 bit load at `base + x`, byte swapped.
 *
 * @param x The guest address.
 */
#define REX_LOAD_U64(x)

/**
 * A `std::string_view` over guest memory.
 *
 * @param x The guest address.
 * @param len The length in bytes.
 */
#define REX_LOAD_STRING(x, len)

/**
 * A volatile byte store at `base + x`.
 *
 * @param x The guest address.
 * @param y The value.
 */
#define REX_STORE_U8(x, y)

/**
 * A volatile 16 bit store at `base + x`, byte swapped.
 *
 * @param x The guest address.
 * @param y The value.
 */
#define REX_STORE_U16(x, y)

/**
 * A volatile 32 bit store at `base + x`, byte swapped.
 *
 * @param x The guest address.
 * @param y The value.
 */
#define REX_STORE_U32(x, y)

/**
 * A volatile 64 bit store at `base + x`, byte swapped.
 *
 * @param x The guest address.
 * @param y The value.
 */
#define REX_STORE_U64(x, y)

/**
 * The host address of a stack access, checked to be below `0xE0000000` in debug builds.
 *
 * @param x The guest address.
 */
#define REX_STACK_ADDR(x)

/**
 * A byte load from the guest stack.
 *
 * @param x The guest address.
 */
#define REX_STACK_LOAD_U8(x)

/**
 * A 16 bit load from the guest stack, byte swapped.
 *
 * @param x The guest address.
 */
#define REX_STACK_LOAD_U16(x)

/**
 * A 32 bit load from the guest stack, byte swapped.
 *
 * @param x The guest address.
 */
#define REX_STACK_LOAD_U32(x)

/**
 * A 64 bit load from the guest stack, byte swapped.
 *
 * @param x The guest address.
 */
#define REX_STACK_LOAD_U64(x)

/**
 * A byte store to the guest stack.
 *
 * @param x The guest address.
 * @param y The value.
 */
#define REX_STACK_STORE_U8(x, y)

/**
 * A 16 bit store to the guest stack, byte swapped.
 *
 * @param x The guest address.
 * @param y The value.
 */
#define REX_STACK_STORE_U16(x, y)

/**
 * A 32 bit store to the guest stack, byte swapped.
 *
 * @param x The guest address.
 * @param y The value.
 */
#define REX_STACK_STORE_U32(x, y)

/**
 * A 64 bit store to the guest stack, byte swapped.
 *
 * @param x The guest address.
 * @param y The value.
 */
#define REX_STACK_STORE_U64(x, y)

/**
 * Reads a 32 bit GPU register through the GPU MMIO recipient.
 *
 * @param addr The guest address.
 */
#define REX_GPU_LOAD32(addr)

/**
 * Writes a 32 bit GPU register through the GPU MMIO recipient.
 *
 * @param addr The guest address.
 * @param val The value.
 */
#define REX_GPU_STORE32(addr, val)

/**
 * Reads a 32 bit APU register through the APU MMIO recipient.
 *
 * @param addr The guest address.
 */
#define REX_APU_LOAD32(addr)

/**
 * Writes a 32 bit APU register through the APU MMIO recipient.
 *
 * @param addr The guest address.
 * @param val The value.
 */
#define REX_APU_STORE32(addr, val)

/**
 * A byte load that tests for an MMIO block first and uses the plain access otherwise.
 *
 * @param addr The guest address.
 */
#define REX_MM_LOAD_U8(addr)

/**
 * A 16 bit load that tests for an MMIO block first and uses the plain access otherwise.
 *
 * @param addr The guest address.
 */
#define REX_MM_LOAD_U16(addr)

/**
 * A 32 bit load that tests for an MMIO block first and uses the plain access otherwise.
 *
 * @param addr The guest address.
 */
#define REX_MM_LOAD_U32(addr)

/**
 * A 64 bit load that tests for an MMIO block first and uses the plain access otherwise.
 *
 * @param addr The guest address.
 */
#define REX_MM_LOAD_U64(addr)

/**
 * A byte store that tests for an MMIO block first and uses the plain access otherwise.
 *
 * @param addr The guest address.
 * @param val The value.
 */
#define REX_MM_STORE_U8(addr, val)

/**
 * A 16 bit store that tests for an MMIO block first and uses the plain access otherwise.
 *
 * @param addr The guest address.
 * @param val The value.
 */
#define REX_MM_STORE_U16(addr, val)

/**
 * A 32 bit store that tests for an MMIO block first and uses the plain access otherwise.
 *
 * @param addr The guest address.
 * @param val The value.
 */
#define REX_MM_STORE_U32(addr, val)

/**
 * A 64 bit store that tests for an MMIO block first and uses the plain access otherwise.
 *
 * @param addr The guest address.
 * @param val The value.
 */
#define REX_MM_STORE_U64(addr, val)

/**
 * Calls a recompiled function with the current `ctx` and `base`.
 *
 * @param x The function name.
 */
#define REX_CALL_FUNC(x)

/**
 * Looks up the host function pointer of a guest address in the table placed after the image.
 *
 * @param x The `base` pointer.
 * @param y The guest address.
 */
#define REX_LOOKUP_FUNC(x, y)

/**
 * Calls the function at a guest address. Reads the pointer from the table placed after the image
 * and calls `rex::ppc::ResolveFunction` when the slot is empty or the target is out of range.
 *
 * @param x The guest address.
 */
#define REX_CALL_INDIRECT_FUNC(x)

/**
 * Enables or disables flush to zero mode in `ctx.fpscr`.
 *
 * @param enable True to enable it.
 */
#define REX_SET_FLUSH_MODE(enable)

/**
 * Reports an instruction that has no recompiled form.
 *
 * @param addr The guest address.
 * @param opcode The instruction word.
 */
#define REX_UNIMPLEMENTED(addr, opcode)

/**
 * The guest timebase tick count.
 */
#define REX_QUERY_TIMEBASE()

/**
 * Saves a guest jump buffer and returns 0, or the value that `ppc_longjmp` passes.
 *
 * @param guest_buf_addr The guest address of the buffer.
 */
#define ppc_setjmp(guest_buf_addr)

/**
 * Jumps back to a buffer that `ppc_setjmp` saved.
 *
 * @param guest_buf_addr The guest address of the buffer.
 * @param val The value `ppc_setjmp` returns.
 */
#define ppc_longjmp(guest_buf_addr, val)

/**
 * Raises a guest trap.
 *
 * @param ctx The guest context.
 * @param base The guest memory base.
 * @param trap_type The trap type.
 */
#define ppc_trap(ctx, base, trap_type)
