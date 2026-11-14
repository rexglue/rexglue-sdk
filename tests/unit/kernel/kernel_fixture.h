/**
 * @file        tests/unit/kernel/kernel_fixture.h
 * @brief       A real tool mode TitleRuntime and guest threads for kernel export tests
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
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rex/kernel/export.h>
#include <rex/ppc/context.h>
#include <rex/types.h>

namespace rex {
class IModule;
namespace apu {
class IAudioSystem;
}  // namespace apu
namespace kernel {
class TitleRuntime;
}  // namespace kernel
namespace ui {
class IUIControl;
class IUISystem;
}  // namespace ui
}  // namespace rex

namespace rex::test {

/** Calls exports by name the way recompiled code does. */
class Caller {
 public:
  Caller(PPCContext& ctx, u8* base, rex::kernel::TitleRuntime& runtime)
      : ctx_(ctx), base_(base), runtime_(runtime) {}

  /**
   * Calls a kernel export by name.
   *
   * @param args Arguments placed in r3 to r10.
   * @return The value of r3.
   */
  u32 Kernel(std::string_view name, std::initializer_list<u64> args = {});

  /**
   * Calls an XAM export by name.
   *
   * @param args Arguments placed in r3 to r10.
   * @return The value of r3.
   */
  u32 XAM(std::string_view name, std::initializer_list<u64> args = {});

  /** Allocates zeroed guest memory and returns its guest address. */
  u32 Alloc(u32 size);

  /** Reads a big endian u32 from guest memory. */
  u32 Load32(u32 address) const;

  /** Reads a big endian u32 from guest memory with an acquire load. */
  u32 LoadAcquire32(u32 address) const;

  u8 Load8(u32 address) const;

  /** Writes a big endian u32 to guest memory. */
  void Store32(u32 address, u32 value);

  /** Returns the guest address of a NUL terminated copy of text. */
  u32 AllocString(std::string_view text);

  /** Writes a big endian u64 to guest memory. */
  void Store64(u32 address, u64 value);

  /** Returns the guest address of a big endian 8 byte value. */
  u32 AllocQuad(i64 value);

  /**
   * Returns the guest address of an X_OBJECT_ATTRIBUTES named name.
   *
   * @param attributes The value of the attributes field.
   */
  u32 AllocAttributes(std::string_view name, u32 attributes = 0);

  /**
   * Runs a body on a new guest thread created through the ExCreateThread export.
   *
   * @param body The body, called with a Caller bound to the new thread.
   * @param creation_flags The ExCreateThread creation flags, 1 creates the thread suspended.
   * @return The thread handle written by the export.
   */
  u32 Spawn(std::function<void(Caller&)> body, u32 creation_flags = 0);

  /**
   * Registers a body that runs as the start routine of a thread created through ExCreateThread.
   *
   * @param body The body, called with a Caller bound to the started thread.
   * @return The ID to pass as the ExCreateThread start context.
   */
  static u32 RegisterBody(std::function<void(Caller&)> body);

  inline static u32 start_thunk = 0;

  PPCContext& GetContext() const { return ctx_; }
  u8* GetBase() const { return base_; }
  rex::kernel::TitleRuntime& GetRuntime() const { return runtime_; }

 private:
  u32 Call(kernel::ExportModule module, std::string_view name, std::initializer_list<u64> args);

  PPCContext& ctx_;
  u8* base_;
  rex::kernel::TitleRuntime& runtime_;
};

/** Owns a tool mode TitleRuntime and runs test bodies on real guest threads. */
class KernelFixture {
 public:
  /**
   * Creates the fixture and runs the TitleRuntime on a game root.
   *
   * @param populate Called with the game root before it is mounted, to create host files.
   * @param providers Extra driver providers the app registers.
   * @param audio_driver The audio driver to select, or empty for none.
   * @param ui_driver The UI driver to select, or empty for none.
   */
  explicit KernelFixture(const std::function<void(const std::filesystem::path&)>& populate = {},
                         std::vector<IModule*> providers = {}, std::string audio_driver = {},
                         std::string ui_driver = {});
  ~KernelFixture();

  rex::kernel::TitleRuntime& GetRuntime();

  rex::apu::IAudioSystem& GetAudio();

  rex::ui::IUISystem& GetUI();
  ui::IUIControl* GetUIControl();

  /** Runs one UI frame on the calling thread and returns false once the app quits. */
  bool Pump();

  const std::filesystem::path& GetRoot() const;

  /** Runs a body on a guest thread created through the ExCreateThread export and waits for it. */
  void RunOnGuestThread(const std::function<void(Caller&)>& body);

  /**
   * Runs a body on a guest thread and waits for the thread to end, including an exit from inside an
   * export.
   */
  void RunUntilGuestThreadEnds(const std::function<void(Caller&)>& body);

 private:
  struct Impl;

  std::unique_ptr<Impl> impl_;
};

/** Copies bytes into new guest memory padded by 8 zero bytes and returns its guest address. */
u32 CopyToGuest(Caller& c, std::span<const u8> bytes);

/** Fills freshly allocated guest memory with the repeating bytes DE AD BE EF. */
u32 PutFilled(Caller& c, u32 size);

std::vector<u8> ReadBytes(Caller& c, u32 address, u32 size);

/** Decodes pairs of hexadecimal digits into bytes. */
std::vector<u8> ParseHex(std::string_view text);

std::string ToHex(std::span<const u8> bytes);

/** Waits up to 5 seconds for a thread handle, checks the wait succeeded and closes the handle. */
void Join(Caller& c, u32 handle);

/** Reads the system time through KeQuerySystemTime in 100ns ticks. */
u64 SystemTime(Caller& c);

}  // namespace rex::test
