/**
 * @file        tests/unit/kernel/kernel_fixture.h
 * @brief       A real tool mode Runtime and guest threads for kernel export tests
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <filesystem>
#include <functional>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "codegen/tool_runtime.h"
#include "temp_folder.h"
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/runtime/exports.h>
#include <rex/types.h>

namespace rex::testing {

/** Calls exports by name the way recompiled code does. */
class Caller {
 public:
  Caller(PPCContext& ctx, u8* base, Runtime& runtime) : ctx_(ctx), base_(base), runtime_(runtime) {}

  /**
   * Calls an export of xboxkrnl by name through its translation entry point.
   *
   * @param name The export name.
   * @param args Integer arguments placed in r3 to r10.
   * @return The value of r3.
   */
  u32 Kernel(std::string_view name, std::initializer_list<u64> args = {});

  /**
   * Calls an export of XAM by name through its translation entry point.
   *
   * @param name The export name.
   * @param args Integer arguments placed in r3 to r10.
   * @return The value of r3.
   */
  u32 XAM(std::string_view name, std::initializer_list<u64> args = {});

  /**
   * Allocates zeroed guest memory.
   *
   * @param size The size in bytes.
   * @return The guest address.
   */
  u32 Alloc(u32 size);

  /**
   * Reads a big endian u32 from guest memory.
   *
   * @param address The guest address.
   * @return The value.
   */
  u32 Load32(u32 address) const;

  /**
   * Reads a big endian u32 from guest memory with an acquire load, as a title polling a
   * completion word must.
   *
   * @param address The guest address.
   * @return The value.
   */
  u32 LoadAcquire32(u32 address) const;

  /**
   * Reads a byte from guest memory.
   *
   * @param address The guest address.
   * @return The byte at the address.
   */
  u8 Load8(u32 address) const;

  /**
   * Writes a big endian u32 to guest memory.
   *
   * @param address The guest address.
   * @param value The value.
   */
  void Store32(u32 address, u32 value);

  /**
   * Returns the guest address of a NUL terminated copy of text.
   *
   * @param text The text.
   * @return The guest address.
   */
  u32 String(std::string_view text);

  /**
   * Writes a big endian u64 to guest memory.
   *
   * @param address The guest address.
   * @param value The value.
   */
  void Store64(u32 address, u64 value);

  /**
   * Returns the guest address of a big endian 8 byte value.
   *
   * @param value The value.
   * @return The guest address.
   */
  u32 Quad(i64 value);

  /**
   * Returns the guest address of an X_OBJECT_ATTRIBUTES whose name is the given text.
   *
   * @param name The object name.
   * @return The guest address.
   */
  u32 Attributes(std::string_view name);

  /**
   * Runs a body on a new guest thread created through the ExCreateThread export.
   *
   * @param body The body, called with a Caller bound to the new thread.
   * @param creation_flags The ExCreateThread creation flags, 1 creates the thread suspended.
   * @return The thread handle written by the export.
   */
  u32 Spawn(std::function<void(Caller&)> body, u32 creation_flags = 0);

  /**
   * Registers a body so a test can pass the returned id as the start context of ExCreateThread.
   *
   * @param body The body, called with a Caller bound to the started thread.
   * @return The start context id.
   */
  static u32 PutBodyForTest(std::function<void(Caller&)> body);

  inline static u32 start_thunk = 0;

  PPCContext& GetCtx() const { return ctx_; }
  Runtime& GetRuntime() const { return runtime_; }

 private:
  u32 Call(runtime::ExportModule module, std::string_view name, std::initializer_list<u64> args);

  PPCContext& ctx_;
  u8* base_;
  Runtime& runtime_;
};

/** Owns a tool mode Runtime and runs test bodies on real guest threads. */
class KernelFixture {
 public:
  /**
   * Creates the fixture and runs the Runtime on a game root.
   *
   * @param populate Called with the game root before the Runtime mounts it, to create host files.
   * @param modules Modules loaded after rexruntime.
   * @param ui The user interface the Runtime shows prompts through, null for the served one.
   */
  explicit KernelFixture(const std::function<void(const std::filesystem::path&)>& populate = {},
                         std::span<const std::string> modules = {},
                         ui::IUserInterface* ui = nullptr);
  ~KernelFixture();

  Runtime& GetRuntime() { return tool_.GetRuntime(); }

  /**
   * Returns the API of the tool runtime, which serves the runtime interfaces.
   *
   * @return The API.
   */
  API& GetAPI() { return tool_.GetAPI(); }

  const std::filesystem::path& GetRoot() const { return root_.path; }

  /**
   * Runs a body on a guest thread created through the ExCreateThread export and waits for it.
   *
   * @param body The test body, called with a Caller bound to the guest thread.
   */
  void RunOnGuestThread(const std::function<void(Caller&)>& body);

  /**
   * Runs a body on a guest thread and waits for the thread to end, whether the body returns or
   * the thread exits from inside an export such as one that terminates the title.
   *
   * @param body The test body, called with a Caller bound to the guest thread.
   */
  void RunUntilGuestThreadEnds(const std::function<void(Caller&)>& body);

 private:
  struct ThreadDrain {
    ThreadDrain();
    ~ThreadDrain();
    u64 baseline;
  };

  ThreadDrain drain_;
  test::TempFolder root_{"rexglue_kernel_fixture_"};
  codegen::ToolRuntime tool_;
};

/**
 * Copies bytes into freshly allocated guest memory, padded by 8 zero bytes.
 *
 * @param c The caller.
 * @param bytes The bytes.
 * @return The guest address.
 */
u32 Put(Caller& c, std::span<const u8> bytes);

/**
 * Fills freshly allocated guest memory with the repeating bytes DE AD BE EF.
 *
 * @param c The caller.
 * @param size The size in bytes.
 * @return The guest address.
 */
u32 PutFilled(Caller& c, u32 size);

/**
 * Reads bytes from guest memory.
 *
 * @param c The caller.
 * @param address The guest address.
 * @param size The size in bytes.
 * @return The bytes.
 */
std::vector<u8> Get(Caller& c, u32 address, u32 size);

/**
 * Decodes hexadecimal text into bytes.
 *
 * @param text Pairs of hexadecimal digits.
 * @return The bytes.
 */
std::vector<u8> Hex(std::string_view text);

/**
 * Formats bytes as lowercase hexadecimal text.
 *
 * @param bytes The bytes.
 * @return The text.
 */
std::string ToHex(std::span<const u8> bytes);

/**
 * Waits for a thread handle for up to 5 seconds, checks that the wait succeeded and closes the
 * handle.
 *
 * @param c The caller.
 * @param handle The thread handle.
 */
void Join(Caller& c, u32 handle);

/**
 * Reads the system time through KeQuerySystemTime.
 *
 * @param c The caller.
 * @return The time in 100ns ticks.
 */
u64 SystemTime(Caller& c);

}  // namespace rex::testing
