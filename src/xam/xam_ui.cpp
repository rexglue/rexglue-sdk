/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

#include <rex/memory/mapped_ptr.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/title_exit.h>
#include <rex/kernel/thread_registry.h>
#include <rex/kernel/object/kernel_thread.h>
#include <rex/logging/log.h>
#include "xam/categories.h"
#include <rex/string/text.h>

#include "xam/xam_ui.h"
#include <rex/ppc/hook.h>
#include <rex/types.h>
#include <rex/kernel/types/status.h>
#include <rex/platform/thread.h>
#include <rex/platform/sync.h>
#include <rex/ui/ui_system.h>
#include "xam/stub_log.h"
#include <rex/kernel/thread_state.h>
#include <rex/utility.h>
#include <rex/kernel/object/notifier.h>
#include "xam/xam_module.h"
#include "xam/xam_state.h"
#include "xam/xam_bodies.h"

namespace rex {
namespace xam {

using namespace rex::kernel;

namespace {

class ScopedTitleInputBlock : public rex::NonCopyable {
 public:
  ScopedTitleInputBlock() : input_(CurrentXAM().GetInput()) {
    if (input_) {
      input_->AcquireUIBlock();
    }
  }
  ~ScopedTitleInputBlock() {
    if (input_) {
      input_->ReleaseUIBlock();
    }
  }

 private:
  rex::hid::IInputSystem* input_;
};

bool HasWindow() {
  rex::ui::IUISystem* user_interface = CurrentXAM().GetUI();
  return user_interface != nullptr && user_interface->HasWindow();
}

struct PromptWait {
  std::mutex mutex;
  bool abandoned = false;
  kernel::X_RESULT result = kernel::X_ERROR_CANCELLED;
  std::unique_ptr<rex::platform::Event> done = rex::platform::Event::CreateManualResetEvent(false);
};

class PromptShown : public rex::NonCopyable {
 public:
  explicit PromptShown(PromptWait& wait) : wait_(wait) { CurrentXAM().IncrementShownPrompts(); }
  ~PromptShown() {
    std::lock_guard lock(wait_.mutex);
    wait_.abandoned = true;
    CurrentXAM().DecrementShownPrompts();
  }

 private:
  PromptWait& wait_;
};

template <std::invocable<const ui::PromptResult&> F>
kernel::X_RESULT XAMPromptAndWait(const ui::PromptConfig& config, const F& complete) {
  ScopedTitleInputBlock input_block;
  auto wait = std::make_shared<PromptWait>();
  PromptShown shown(*wait);
  CurrentXAM().GetUI()->ShowPrompt(config, [wait, &complete](const ui::PromptResult& prompt) {
    std::lock_guard lock(wait->mutex);
    if (wait->abandoned) {
      return;
    }
    wait->result = complete(prompt);
    wait->done->Set();
  });
  CurrentTitle().threads.GetObjectWaiter().WaitHostObject(wait->done.get(),
                                                          std::chrono::milliseconds::max());
  std::lock_guard lock(wait->mutex);
  return wait->result;
}

}  // namespace

template <typename Close>
kernel::X_RESULT XAMDispatchPrompt(ui::PromptConfig config, Close close, u32 overlapped) {
  auto pre = []() { CurrentTitle().notifier.BroadcastNotification(0x9, true); };
  auto post = []() {
    CurrentTitle().threads.SleepCurrentThread(std::chrono::milliseconds(100));
    CurrentTitle().notifier.BroadcastNotification(0x9, false);
  };
  if constexpr (std::is_invocable_r_v<kernel::X_RESULT, Close&, const ui::PromptResult&, u32&,
                                      u32&>) {
    auto run = [config = std::move(config), close = std::move(close)](
                   u32& extended_error, u32& length) -> kernel::X_RESULT {
      return XAMPromptAndWait(config, [&](const ui::PromptResult& prompt) {
        return close(prompt, extended_error, length);
      });
    };
    if (!overlapped) {
      pre();
      u32 extended_error = 0;
      u32 length = 0;
      auto result = run(extended_error, length);
      post();
      return result;
    }
    CurrentTitle().overlapped_dispatcher.CompleteDeferred(
        overlapped,
        [run = std::move(run)]() mutable {
          u32 extended_error = 0;
          u32 length = 0;
          const kernel::X_RESULT result = run(extended_error, length);
          return kernel::CompletionOptions{
              .result = result, .extended_error = extended_error, .length = length};
        },
        pre, post);
    return kernel::X_ERROR_IO_PENDING;
  } else {
    auto run = [config = std::move(config), close = std::move(close)]() -> kernel::X_RESULT {
      return XAMPromptAndWait(config,
                              [&](const ui::PromptResult& prompt) { return close(prompt); });
    };
    if (!overlapped) {
      pre();
      auto result = run();
      post();
      return result;
    }
    CurrentTitle().overlapped_dispatcher.CompleteDeferred(
        overlapped,
        [run = std::move(run)]() mutable {
          return kernel::CompletionOptions{.result = run(), .length = 0};
        },
        pre, post);
    return kernel::X_ERROR_IO_PENDING;
  }
}

kernel::X_RESULT XAMDispatchHeadless(const std::function<kernel::X_RESULT()>& run_callback,
                                     u32 overlapped) {
  auto pre = []() {
    REXXAM_DEBUG("XAMDispatchHeadless: Broadcasting XN_SYS_UI = true");
    CurrentTitle().notifier.BroadcastNotification(0x9, true);
  };
  auto post = []() {
    CurrentTitle().threads.SleepCurrentThread(std::chrono::milliseconds(100));
    REXXAM_DEBUG("XAMDispatchHeadless: Broadcasting XN_SYS_UI = false");
    CurrentTitle().notifier.BroadcastNotification(0x9, false);
  };
  if (!overlapped) {
    pre();
    auto result = run_callback();
    post();
    return result;
  } else {
    CurrentTitle().overlapped_dispatcher.CompleteDeferred(
        overlapped,
        [run_callback]() {
          return kernel::CompletionOptions{.result = run_callback(), .length = 0};
        },
        pre, post);
    return kernel::X_ERROR_IO_PENDING;
  }
}

u32 XamIsUIActive_entry() {
  return XAMIsUIActive();
}

namespace {

u32 ShowMessageBox(MappedWString title_ptr, MappedWString text_ptr, u32 button_count,
                   MappedU32 button_ptrs, u32 active_button, MappedU32 result_ptr,
                   MappedVoid overlapped) {
  std::string title;
  if (title_ptr) {
    title = rex::string::ToUTF8(rex::memory::load_and_swap<std::u16string>(
        CurrentTitle().membase.TranslateVirtual(title_ptr.GetGuestAddress())));
  }
  std::string text_str =
      text_ptr ? rex::string::ToUTF8(rex::memory::load_and_swap<std::u16string>(
                     CurrentTitle().membase.TranslateVirtual(text_ptr.GetGuestAddress())))
               : "";

  std::vector<std::string> buttons;
  for (u32 i = 0; i < button_count; ++i) {
    u32 button_ptr = button_ptrs[i];
    auto button = rex::memory::load_and_swap<std::u16string>(
        CurrentTitle().membase.TranslateVirtual(button_ptr));
    buttons.push_back(rex::string::ToUTF8(button));
  }

  kernel::X_RESULT result;
  if (GetXAMSettings().headless->Get() || !HasWindow()) {
    auto run = [result_ptr, active_button]() -> kernel::X_RESULT {
      *result_ptr = static_cast<u32>(active_button);
      return kernel::X_ERROR_SUCCESS;
    };
    result = XAMDispatchHeadless(run, overlapped.GetGuestAddress());
  } else {
    ui::PromptConfig config{
        .title = title.empty() ? "Message Box" : title, .body = text_str, .buttons = buttons};
    auto close = [result_ptr, active_button](const ui::PromptResult& prompt) -> kernel::X_RESULT {
      *result_ptr = prompt.canceled ? static_cast<u32>(active_button) : prompt.button;
      return kernel::X_ERROR_SUCCESS;
    };
    result = XAMDispatchPrompt(std::move(config), close, overlapped.GetGuestAddress());
  }
  return result;
}

}  // namespace

u32 XamShowMessageBoxUI_entry(u32 user_index, MappedWString title_ptr, MappedWString text_ptr,
                              u32 button_count, MappedU32 button_ptrs, u32 active_button, u32 flags,
                              MappedU32 result_ptr, MappedVoid overlapped) {
  REXXAM_DEBUG(
      "XamShowMessageBoxUI({:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {:08X})",
      static_cast<u32>(user_index), title_ptr.GetGuestAddress(), text_ptr.GetGuestAddress(),
      static_cast<u32>(button_count), button_ptrs.GetGuestAddress(),
      static_cast<u32>(active_button), static_cast<u32>(flags), result_ptr.GetGuestAddress(),
      overlapped.GetGuestAddress());
  return ShowMessageBox(title_ptr, text_ptr, button_count, button_ptrs, active_button, result_ptr,
                        overlapped);
}

u32 XamShowMessageBoxUIEx_entry(u32 user_index, MappedWString title_ptr, MappedWString text_ptr,
                                u32 button_count, MappedU32 button_ptrs, u32 active_button,
                                u32 flags, u32 extra, MappedU32 result_ptr, MappedVoid overlapped) {
  REXXAM_DEBUG(
      "XamShowMessageBoxUIEx({:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {:08X}, "
      "{:08X}, {:08X}, {:08X})",
      static_cast<u32>(user_index), title_ptr.GetGuestAddress(), text_ptr.GetGuestAddress(),
      static_cast<u32>(button_count), button_ptrs.GetGuestAddress(),
      static_cast<u32>(active_button), static_cast<u32>(flags), static_cast<u32>(extra),
      result_ptr.GetGuestAddress(), overlapped.GetGuestAddress());
  return ShowMessageBox(title_ptr, text_ptr, button_count, button_ptrs, active_button, result_ptr,
                        overlapped);
}

u32 XamShowKeyboardUI_entry(u32 user_index, u32 flags, MappedWString default_text,
                            MappedWString title, MappedWString description, MappedWString buffer,
                            u32 buffer_length, MappedVoid overlapped) {
  REXXAM_DEBUG("XamShowKeyboardUI({:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {:08X})",
               static_cast<u32>(user_index), static_cast<u32>(flags),
               default_text.GetGuestAddress(), title.GetGuestAddress(),
               description.GetGuestAddress(), buffer.GetGuestAddress(),
               static_cast<u32>(buffer_length), overlapped.GetGuestAddress());
  if (!buffer) {
    return kernel::X_ERROR_INVALID_PARAMETER;
  }

  REX_ASSERT(overlapped.GetGuestAddress() != 0);

  auto buffer_size = static_cast<size_t>(buffer_length) * 2;

  kernel::X_RESULT result;
  if (GetXAMSettings().headless->Get() || !HasWindow()) {
    auto run = [default_text, buffer, buffer_length, buffer_size]() -> kernel::X_RESULT {
      if (!default_text) {
        std::memset(buffer, 0, buffer_size);
      } else {
        rex::string::CopyTruncating(buffer, default_text.GetValue(), buffer_length);
      }
      return kernel::X_ERROR_SUCCESS;
    };
    result = XAMDispatchHeadless(run, overlapped.GetGuestAddress());
  } else {
    auto close = [buffer, buffer_length](const ui::PromptResult& prompt, u32& extended_error,
                                         u32& length) -> kernel::X_RESULT {
      length = 0;
      if (prompt.canceled || prompt.button != 0) {
        extended_error = kernel::X_ERROR_CANCELLED;
        return kernel::X_ERROR_SUCCESS;
      }
      std::string padded = prompt.text;
      padded.resize(static_cast<size_t>(buffer_length) + 1, '\0');
      auto text = rex::string::ToUTF16(padded);
      rex::string::CopyAndSwapTruncating(buffer, text, buffer_length);
      extended_error = kernel::X_ERROR_SUCCESS;
      return kernel::X_ERROR_SUCCESS;
    };

    std::string title_str =
        title ? rex::string::ToUTF8(rex::memory::load_and_swap<std::u16string>(
                    CurrentTitle().membase.TranslateVirtual(title.GetGuestAddress())))
              : "";
    std::string desc_str =
        description ? rex::string::ToUTF8(rex::memory::load_and_swap<std::u16string>(
                          CurrentTitle().membase.TranslateVirtual(description.GetGuestAddress())))
                    : "";
    std::string def_text_str =
        default_text ? rex::string::ToUTF8(rex::memory::load_and_swap<std::u16string>(
                           CurrentTitle().membase.TranslateVirtual(default_text.GetGuestAddress())))
                     : "";
    if (title_str.empty()) {
      if (desc_str.empty()) {
        title_str = "Keyboard Input";
      } else {
        title_str = desc_str;
        desc_str.clear();
      }
    }

    ui::PromptConfig config{.title = title_str,
                            .body = desc_str,
                            .buttons = {"OK", "Cancel"},
                            .text_input = true,
                            .default_text = def_text_str,
                            .max_length = buffer_length};
    result = XAMDispatchPrompt(std::move(config), close, overlapped.GetGuestAddress());
  }
  return result;
}

u32 XamShowDeviceSelectorUI_entry(u32 user_index, u32 content_type, u32 content_flags,
                                  u64 total_requested, MappedU32 device_id_ptr,
                                  MappedVoid overlapped) {
  REXXAM_DEBUG("XamShowDeviceSelectorUI({:08X}, {:08X}, {:08X}, {:016X}, {:08X}, {:08X})",
               static_cast<u32>(user_index), static_cast<u32>(content_type),
               static_cast<u32>(content_flags), static_cast<u64>(total_requested),
               device_id_ptr.GetGuestAddress(), overlapped.GetGuestAddress());
  return XAMDispatchHeadless(
      [device_id_ptr]() -> kernel::X_RESULT {
        // 0x00000001 is DummyDeviceID::kHDD.
        *device_id_ptr = 0x00000001;
        return kernel::X_ERROR_SUCCESS;
      },
      overlapped.GetGuestAddress());
}

void XamShowDirtyDiscErrorUI_entry(u32 user_index) {
  REXXAM_ERROR("XamShowDirtyDiscErrorUI called! user_index={}", static_cast<u32>(user_index));
  REXXAM_ERROR("This indicates a disc or file read error. Check that all game files exist.");

  if (HasWindow()) {
    XAMDispatchPrompt(
        ui::PromptConfig{.title = "Disc Read Error",
                         .body =
                             "There's been an issue reading content from the game disc.\nThis is "
                             "likely caused by bad or unimplemented file IO calls.",
                         .buttons = {"OK"}},
        [](const ui::PromptResult&) -> kernel::X_RESULT { return kernel::X_ERROR_SUCCESS; }, 0);
  } else {
    REXXAM_ERROR("===========================================");
    REXXAM_ERROR("FATAL: Disc Read Error (no UI to display)");
    REXXAM_ERROR("Check that all game content files are present");
    REXXAM_ERROR("Missing files or bad mounts cause this error");
    REXXAM_ERROR("===========================================");
  }
  CurrentTitle().exit.ExitTitle(rex::kernel::TitleExitReason::kTitleDataError);
}

u32 XamShowPartyUI_entry([[maybe_unused]] u32 r3, [[maybe_unused]] u32 r4) {
  return kernel::X_ERROR_FUNCTION_FAILED;
}

u32 XamShowCommunitySessionsUI_entry([[maybe_unused]] u32 r3, [[maybe_unused]] u32 r4) {
  return kernel::X_ERROR_FUNCTION_FAILED;
}

}  // namespace xam
}  // namespace rex

REX_XAM_BODY(__imp__XamIsUIActive, rex::xam::XamIsUIActive_entry)
REX_XAM_BODY(__imp__XamShowMessageBoxUI, rex::xam::XamShowMessageBoxUI_entry)
REX_XAM_BODY(__imp__XamShowKeyboardUI, rex::xam::XamShowKeyboardUI_entry)
REX_XAM_BODY(__imp__XamShowDeviceSelectorUI, rex::xam::XamShowDeviceSelectorUI_entry)
REX_XAM_BODY(__imp__XamShowDirtyDiscErrorUI, rex::xam::XamShowDirtyDiscErrorUI_entry)
REX_XAM_BODY(__imp__XamShowPartyUI, rex::xam::XamShowPartyUI_entry)
REX_XAM_BODY(__imp__XamShowCommunitySessionsUI, rex::xam::XamShowCommunitySessionsUI_entry)
REX_XAM_BODY(__imp__XamShowMessageBoxUIEx, rex::xam::XamShowMessageBoxUIEx_entry)
