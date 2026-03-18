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

#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/string.h>
#include <rex/string/util.h>
#include <rex/system/flags.h>
#include <rex/system/kernel_state.h>

#include <imgui.h>

REXCVAR_DEFINE_BOOL(headless, false, "Kernel",
                    "Don't display any UI, using defaults for prompts as needed");
#include <rex/kernel/xam/private.h>
#include <rex/ppc/function.h>
#include <rex/ppc/types.h>
#include <rex/system/xtypes.h>
#include <rex/thread.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;

// TODO(gibbed): This is all one giant WIP that seems to work better than the
// previous immediate synchronous completion of dialogs.
//
// The deferred execution of dialog handling is done in such a way that there is
// a pre-, peri- (completion), and post- callback steps.
//
// pre();
// result = completion();
// CompleteOverlapped(result);
// post();
//
// There are games that are batshit insane enough to wait for the X_OVERLAPPED
// to be completed (ie not X_ERROR_PENDING) before creating a listener to
// receive a notification, which is why we have distinct pre- and post- steps.
//
// We deliberately delay the XN_SYS_UI = false notification to give games time
// to create a listener (if they're insane enough do this).

extern std::atomic<int> xam_dialogs_shown_;

class XamDialog : public rex::ui::ImGuiDialog {
 public:
  void set_close_callback(std::function<void()> close_callback) {
    close_callback_ = close_callback;
  }

 protected:
  XamDialog(rex::ui::ImGuiDrawer* imgui_drawer) : rex::ui::ImGuiDialog(imgui_drawer) {}

  void OnClose() override {
    if (close_callback_) {
      close_callback_();
    }
  }

 private:
  std::function<void()> close_callback_ = nullptr;
};

template <typename T>
X_RESULT xeXamDispatchDialog(T* dialog, std::function<X_RESULT(T*)> close_callback,
                             uint32_t overlapped) {
  auto pre = []() {
    // Broadcast XN_SYS_UI = true
    REX_KERNEL_STATE()->BroadcastNotification(0x9, true);
  };
  auto run = [dialog, close_callback]() -> X_RESULT {
    X_RESULT result;
    dialog->set_close_callback(
        [&dialog, &result, &close_callback]() { result = close_callback(dialog); });
    rex::thread::Fence fence;
    rex::ui::WindowedAppContext* app_context = REX_KERNEL_STATE()->emulator()->app_context();
    if (app_context &&
        app_context->CallInUIThreadSynchronous([&dialog, &fence]() { dialog->Then(&fence); })) {
      ++xam_dialogs_shown_;
      fence.Wait();
      --xam_dialogs_shown_;
    } else {
      delete dialog;
    }
    // dialog should be deleted at this point!
    return result;
  };
  auto post = []() {
    rex::thread::Sleep(std::chrono::milliseconds(100));
    // Broadcast XN_SYS_UI = false
    REX_KERNEL_STATE()->BroadcastNotification(0x9, false);
  };
  if (!overlapped) {
    pre();
    auto result = run();
    post();
    return result;
  } else {
    REX_KERNEL_STATE()->CompleteOverlappedDeferred(run, overlapped, pre, post);
    return X_ERROR_IO_PENDING;
  }
}

template <typename T>
X_RESULT xeXamDispatchDialogEx(T* dialog,
                               std::function<X_RESULT(T*, uint32_t&, uint32_t&)> close_callback,
                               uint32_t overlapped) {
  auto pre = []() {
    // Broadcast XN_SYS_UI = true
    REX_KERNEL_STATE()->BroadcastNotification(0x9, true);
  };
  auto run = [dialog, close_callback](uint32_t& extended_error, uint32_t& length) -> X_RESULT {
    rex::ui::WindowedAppContext* app_context = REX_KERNEL_STATE()->emulator()->app_context();
    X_RESULT result;
    dialog->set_close_callback([&dialog, &result, &extended_error, &length, &close_callback]() {
      result = close_callback(dialog, extended_error, length);
    });
    rex::thread::Fence fence;
    if (app_context &&
        app_context->CallInUIThreadSynchronous([&dialog, &fence]() { dialog->Then(&fence); })) {
      ++xam_dialogs_shown_;
      fence.Wait();
      --xam_dialogs_shown_;
    } else {
      delete dialog;
    }
    // dialog should be deleted at this point!
    return result;
  };
  auto post = []() {
    rex::thread::Sleep(std::chrono::milliseconds(100));
    // Broadcast XN_SYS_UI = false
    REX_KERNEL_STATE()->BroadcastNotification(0x9, false);
  };
  if (!overlapped) {
    pre();
    uint32_t extended_error, length;
    auto result = run(extended_error, length);
    post();
    // TODO(gibbed): do something with extended_error/length?
    return result;
  } else {
    REX_KERNEL_STATE()->CompleteOverlappedDeferredEx(run, overlapped, pre, post);
    return X_ERROR_IO_PENDING;
  }
}

X_RESULT xeXamDispatchHeadless(std::function<X_RESULT()> run_callback, uint32_t overlapped) {
  auto pre = []() {
    REXKRNL_DEBUG("xeXamDispatchHeadless: Broadcasting XN_SYS_UI = true");
    // Broadcast XN_SYS_UI = true
    REX_KERNEL_STATE()->BroadcastNotification(0x9, true);
  };
  auto post = []() {
    rex::thread::Sleep(std::chrono::milliseconds(100));
    REXKRNL_DEBUG("xeXamDispatchHeadless: Broadcasting XN_SYS_UI = false");
    // Broadcast XN_SYS_UI = false
    REX_KERNEL_STATE()->BroadcastNotification(0x9, false);
  };
  if (!overlapped) {
    pre();
    auto result = run_callback();
    post();
    return result;
  } else {
    REX_KERNEL_STATE()->CompleteOverlappedDeferred(run_callback, overlapped, pre, post);
    return X_ERROR_IO_PENDING;
  }
}

X_RESULT xeXamDispatchHeadlessEx(std::function<X_RESULT(uint32_t&, uint32_t&)> run_callback,
                                 uint32_t overlapped) {
  auto pre = []() {
    // Broadcast XN_SYS_UI = true
    REX_KERNEL_STATE()->BroadcastNotification(0x9, true);
  };
  auto post = []() {
    rex::thread::Sleep(std::chrono::milliseconds(100));
    // Broadcast XN_SYS_UI = false
    REX_KERNEL_STATE()->BroadcastNotification(0x9, false);
  };
  if (!overlapped) {
    pre();
    uint32_t extended_error, length;
    auto result = run_callback(extended_error, length);
    post();
    // TODO(gibbed): do something with extended_error/length?
    return result;
  } else {
    REX_KERNEL_STATE()->CompleteOverlappedDeferredEx(run_callback, overlapped, pre, post);
    return X_ERROR_IO_PENDING;
  }
}

ppc_u32_result_t XamIsUIActive_entry() {
  return xeXamIsUIActive();
}

class MessageBoxDialog : public XamDialog {
 public:
  MessageBoxDialog(rex::ui::ImGuiDrawer* imgui_drawer, std::string title, std::string description,
                   std::vector<std::string> buttons, uint32_t default_button)
      : XamDialog(imgui_drawer),
        title_(title),
        description_(description),
        buttons_(std::move(buttons)),
        default_button_(default_button),
        chosen_button_(default_button) {
    if (!title_.size()) {
      title_ = "Message Box";
    }
  }

  uint32_t chosen_button() const { return chosen_button_; }

  void OnDraw(ImGuiIO& io) override {
    bool first_draw = false;
    if (!has_opened_) {
      ImGui::OpenPopup(title_.c_str());
      has_opened_ = true;
      first_draw = true;
    }
    if (ImGui::BeginPopupModal(title_.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      if (description_.size()) {
        ImGui::Text("%s", description_.c_str());
      }
      if (first_draw) {
        ImGui::SetKeyboardFocusHere();
      }
      for (size_t i = 0; i < buttons_.size(); ++i) {
        if (ImGui::Button(buttons_[i].c_str())) {
          chosen_button_ = static_cast<uint32_t>(i);
          ImGui::CloseCurrentPopup();
          Close();
        }
        ImGui::SameLine();
      }
      ImGui::Spacing();
      ImGui::Spacing();
      ImGui::EndPopup();
    } else {
      Close();
    }
  }

 private:
  bool has_opened_ = false;
  std::string title_;
  std::string description_;
  std::vector<std::string> buttons_;
  uint32_t default_button_ = 0;
  uint32_t chosen_button_ = 0;
};

// https://www.se7ensins.com/forums/threads/working-xshowmessageboxui.844116/
ppc_u32_result_t XamShowMessageBoxUI_entry(ppc_u32_t user_index, ppc_pchar16_t title_ptr,
                                           ppc_pchar16_t text_ptr, ppc_u32_t button_count,
                                           ppc_pu32_t button_ptrs, ppc_u32_t active_button,
                                           ppc_u32_t flags, ppc_pu32_t result_ptr,
                                           ppc_pvoid_t overlapped) {
  REXKRNL_DEBUG(
      "XamShowMessageBoxUI({:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {:08X})",
      uint32_t(user_index), title_ptr.guest_address(), text_ptr.guest_address(),
      uint32_t(button_count), button_ptrs.guest_address(), uint32_t(active_button), uint32_t(flags),
      result_ptr.guest_address(), overlapped.guest_address());
  std::string title;
  if (title_ptr) {
    title = rex::string::to_utf8(title_ptr.value());
  } else {
    title = "";  // TODO(gibbed): default title based on flags?
  }

  std::vector<std::string> buttons;
  for (uint32_t i = 0; i < button_count; ++i) {
    uint32_t button_ptr = button_ptrs[i];
    auto button = rex::memory::load_and_swap<std::u16string>(
        REX_KERNEL_MEMORY()->TranslateVirtual(button_ptr));
    buttons.push_back(rex::string::to_utf8(button));
  }

  X_RESULT result;
  if (REXCVAR_GET(headless)) {
    // Auto-pick the focused button.
    auto run = [result_ptr, active_button]() -> X_RESULT {
      *result_ptr = static_cast<uint32_t>(active_button);
      return X_ERROR_SUCCESS;
    };
    result = xeXamDispatchHeadless(run, overlapped.guest_address());
  } else {
    // TODO(benvanik): setup icon states.
    switch (flags & 0xF) {
      case 0:
        // config.pszMainIcon = nullptr;
        break;
      case 1:
        // config.pszMainIcon = TD_ERROR_ICON;
        break;
      case 2:
        // config.pszMainIcon = TD_WARNING_ICON;
        break;
      case 3:
        // config.pszMainIcon = TD_INFORMATION_ICON;
        break;
    }
    auto close = [result_ptr](MessageBoxDialog* dialog) -> X_RESULT {
      *result_ptr = dialog->chosen_button();
      return X_ERROR_SUCCESS;
    };
    const Runtime* emulator = REX_KERNEL_STATE()->emulator();
    ui::ImGuiDrawer* imgui_drawer = emulator->imgui_drawer();
    if (imgui_drawer) {
      result = xeXamDispatchDialog<MessageBoxDialog>(
          new MessageBoxDialog(imgui_drawer, title, rex::string::to_utf8(text_ptr.value()), buttons,
                               active_button),
          close, overlapped.guest_address());
    } else {
      // Fallback to headless if no drawer available
      auto run = [result_ptr, active_button]() -> X_RESULT {
        *result_ptr = static_cast<uint32_t>(active_button);
        return X_ERROR_SUCCESS;
      };
      result = xeXamDispatchHeadless(run, overlapped.guest_address());
    }
  }
  return result;
}

class KeyboardInputDialog : public XamDialog {
 public:
  KeyboardInputDialog(rex::ui::ImGuiDrawer* imgui_drawer, std::string title,
                      std::string description, std::string default_text, size_t max_length)
      : XamDialog(imgui_drawer),
        title_(title),
        description_(description),
        default_text_(default_text),
        max_length_(max_length),
        text_buffer_() {
    if (!title_.size()) {
      if (!description_.size()) {
        title_ = "Keyboard Input";
      } else {
        title_ = description_;
        description_ = "";
      }
    }
    text_ = default_text;
    text_buffer_.resize(max_length);
    rex::string::util_copy_truncating(text_buffer_.data(), default_text_, text_buffer_.size());
  }

  const std::string& text() const { return text_; }
  bool cancelled() const { return cancelled_; }

  void OnDraw(ImGuiIO& io) override {
    bool first_draw = false;
    if (!has_opened_) {
      ImGui::OpenPopup(title_.c_str());
      has_opened_ = true;
      first_draw = true;
    }
    if (ImGui::BeginPopupModal(title_.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      if (description_.size()) {
        ImGui::TextWrapped("%s", description_.c_str());
      }
      if (first_draw) {
        ImGui::SetKeyboardFocusHere();
      }
      if (ImGui::InputText("##body", text_buffer_.data(), text_buffer_.size(),
                           ImGuiInputTextFlags_EnterReturnsTrue)) {
        text_ = std::string(text_buffer_.data(), text_buffer_.size());
        cancelled_ = false;
        ImGui::CloseCurrentPopup();
        Close();
      }
      if (ImGui::Button("OK")) {
        text_ = std::string(text_buffer_.data(), text_buffer_.size());
        cancelled_ = false;
        ImGui::CloseCurrentPopup();
        Close();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) {
        text_ = "";
        cancelled_ = true;
        ImGui::CloseCurrentPopup();
        Close();
      }
      ImGui::Spacing();
      ImGui::EndPopup();
    } else {
      Close();
    }
  }

 private:
  bool has_opened_ = false;
  std::string title_;
  std::string description_;
  std::string default_text_;
  size_t max_length_ = 0;
  std::vector<char> text_buffer_;
  std::string text_ = "";
  bool cancelled_ = true;
};

// https://www.se7ensins.com/forums/threads/release-how-to-use-xshowkeyboardui-release.906568/
ppc_u32_result_t XamShowKeyboardUI_entry(ppc_u32_t user_index, ppc_u32_t flags,
                                         ppc_pchar16_t default_text, ppc_pchar16_t title,
                                         ppc_pchar16_t description, ppc_pchar16_t buffer,
                                         ppc_u32_t buffer_length, ppc_pvoid_t overlapped) {
  REXKRNL_DEBUG("XamShowKeyboardUI({:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {:08X})",
                uint32_t(user_index), uint32_t(flags), default_text.guest_address(),
                title.guest_address(), description.guest_address(), buffer.guest_address(),
                uint32_t(buffer_length), overlapped.guest_address());
  if (!buffer) {
    return X_ERROR_INVALID_PARAMETER;
  }

  assert_true(overlapped.guest_address() != 0);

  auto buffer_size = static_cast<size_t>(buffer_length) * 2;

  X_RESULT result;
  if (REXCVAR_GET(headless)) {
    auto run = [default_text, buffer, buffer_length, buffer_size]() -> X_RESULT {
      // Redirect default_text back into the buffer.
      if (!default_text) {
        std::memset(buffer, 0, buffer_size);
      } else {
        rex::string::util_copy_and_swap_truncating(buffer, default_text.value(), buffer_length);
      }
      return X_ERROR_SUCCESS;
    };
    result = xeXamDispatchHeadless(run, overlapped.guest_address());
  } else {
    auto close = [buffer, buffer_length](KeyboardInputDialog* dialog, uint32_t& extended_error,
                                         uint32_t& length) -> X_RESULT {
      if (dialog->cancelled()) {
        extended_error = X_ERROR_CANCELLED;
        length = 0;
        return X_ERROR_SUCCESS;
      } else {
        // Zero the output buffer.
        auto text = rex::string::to_utf16(dialog->text());
        rex::string::util_copy_and_swap_truncating(buffer, text, buffer_length);
        extended_error = X_ERROR_SUCCESS;
        length = 0;
        return X_ERROR_SUCCESS;
      }
    };
    const Runtime* emulator = REX_KERNEL_STATE()->emulator();
    ui::ImGuiDrawer* imgui_drawer = emulator->imgui_drawer();

    // Read and convert title/description/default_text from guest memory as utf16 to utf8 strings
    std::string title_str = title
                                ? rex::string::to_utf8(rex::memory::load_and_swap<std::u16string>(
                                      REX_KERNEL_MEMORY()->TranslateVirtual(title.guest_address())))
                                : "";
    std::string desc_str =
        description ? rex::string::to_utf8(rex::memory::load_and_swap<std::u16string>(
                          REX_KERNEL_MEMORY()->TranslateVirtual(description.guest_address())))
                    : "";
    std::string def_text_str =
        default_text ? rex::string::to_utf8(rex::memory::load_and_swap<std::u16string>(
                           REX_KERNEL_MEMORY()->TranslateVirtual(default_text.guest_address())))
                     : "";

    if (imgui_drawer) {
      uint32_t buffer_length_safe = buffer_length + 1;  // +1 for null terminator, just in case
      result = xeXamDispatchDialogEx<KeyboardInputDialog>(
          new KeyboardInputDialog(imgui_drawer, title_str, desc_str, def_text_str,
                                  buffer_length_safe),
          close, overlapped.guest_address());
    } else {
      // Fallback to headless
      auto run = [default_text, buffer, buffer_length, buffer_size]() -> X_RESULT {
        if (!default_text) {
          std::memset(buffer, 0, buffer_size);
        } else {
          rex::string::util_copy_and_swap_truncating(buffer, default_text.value(), buffer_length);
        }
        return X_ERROR_SUCCESS;
      };
      result = xeXamDispatchHeadless(run, overlapped.guest_address());
    }
  }
  return result;
}

ppc_u32_result_t XamShowDeviceSelectorUI_entry(ppc_u32_t user_index, ppc_u32_t content_type,
                                               ppc_u32_t content_flags, ppc_u64_t total_requested,
                                               ppc_pu32_t device_id_ptr, ppc_pvoid_t overlapped) {
  REXKRNL_DEBUG("XamShowDeviceSelectorUI({:08X}, {:08X}, {:08X}, {:016X}, {:08X}, {:08X})",
                uint32_t(user_index), uint32_t(content_type), uint32_t(content_flags),
                uint64_t(total_requested), device_id_ptr.guest_address(),
                overlapped.guest_address());
  return xeXamDispatchHeadless(
      [device_id_ptr]() -> X_RESULT {
        // NOTE: 0x00000001 is our dummy device ID from xam_content.cc
        *device_id_ptr = 0x00000001;
        return X_ERROR_SUCCESS;
      },
      overlapped.guest_address());
}

void XamShowDirtyDiscErrorUI_entry(ppc_u32_t user_index) {
  REXKRNL_ERROR("XamShowDirtyDiscErrorUI called! user_index={}", uint32_t(user_index));
  REXKRNL_ERROR("This indicates a disc/file read error - check that all game files exist");

  const Runtime* emulator = REX_KERNEL_STATE()->emulator();
  ui::ImGuiDrawer* imgui_drawer = emulator->imgui_drawer();
  if (imgui_drawer) {
    xeXamDispatchDialog<MessageBoxDialog>(
        new MessageBoxDialog(imgui_drawer, "Disc Read Error",
                             "There's been an issue reading content from the game disc.\nThis is "
                             "likely caused by bad or unimplemented file IO calls.",
                             {"OK"}, 0),
        [](MessageBoxDialog*) -> X_RESULT { return X_ERROR_SUCCESS; }, 0);
  } else {
    // No UI available - log prominently and pause to let user see the error
    REXKRNL_ERROR("===========================================");
    REXKRNL_ERROR("FATAL: Disc Read Error (no UI to display)");
    REXKRNL_ERROR("Check that all game content files are present");
    REXKRNL_ERROR("Missing files or bad mounts cause this error");
    REXKRNL_ERROR("===========================================");
  }
  // This is death, and should never return.
  // TODO(benvanik): cleaner exit.
  exit(1);
}

ppc_u32_result_t XamShowPartyUI_entry(ppc_unknown_t r3, ppc_unknown_t r4) {
  return X_ERROR_FUNCTION_FAILED;
}

ppc_u32_result_t XamShowCommunitySessionsUI_entry(ppc_unknown_t r3, ppc_unknown_t r4) {
  return X_ERROR_FUNCTION_FAILED;
}

uint32_t XamShowMessageBoxUIEx_entry() {
  // TODO(tomc): implement properly
  static bool warned = false;
  if (!warned) {
    REXKRNL_WARN("[STUB] XamShowMessageBoxUIEx - not implemented");
    warned = true;
  }
  return 0;
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

XAM_EXPORT(__imp__XamIsUIActive, rex::kernel::xam::XamIsUIActive_entry)
XAM_EXPORT(__imp__XamShowMessageBoxUI, rex::kernel::xam::XamShowMessageBoxUI_entry)
XAM_EXPORT(__imp__XamShowKeyboardUI, rex::kernel::xam::XamShowKeyboardUI_entry)
XAM_EXPORT(__imp__XamShowDeviceSelectorUI, rex::kernel::xam::XamShowDeviceSelectorUI_entry)
XAM_EXPORT(__imp__XamShowDirtyDiscErrorUI, rex::kernel::xam::XamShowDirtyDiscErrorUI_entry)
XAM_EXPORT(__imp__XamShowPartyUI, rex::kernel::xam::XamShowPartyUI_entry)
XAM_EXPORT(__imp__XamShowCommunitySessionsUI, rex::kernel::xam::XamShowCommunitySessionsUI_entry)
XAM_EXPORT(__imp__XamShowMessageBoxUIEx, rex::kernel::xam::XamShowMessageBoxUIEx_entry)

XAM_EXPORT_STUB(__imp__XamIsGuideDisabled);
XAM_EXPORT_STUB(__imp__XamIsMessageBoxActive);
XAM_EXPORT_STUB(__imp__XamIsNuiUIActive);
XAM_EXPORT_STUB(__imp__XamIsSysUiInvokedByTitle);
XAM_EXPORT_STUB(__imp__XamIsSysUiInvokedByXenonButton);
XAM_EXPORT_STUB(__imp__XamIsUIThread);
XAM_EXPORT_STUB(__imp__XamNavigate);
XAM_EXPORT_STUB(__imp__XamNavigateBack);
XAM_EXPORT_STUB(__imp__XamOverrideHudOpenType);
XAM_EXPORT_STUB(__imp__XamShowAchievementDetailsUI);
XAM_EXPORT_STUB(__imp__XamShowAchievementsUI);
XAM_EXPORT_STUB(__imp__XamShowAchievementsUIEx);
XAM_EXPORT_STUB(__imp__XamShowAndWaitForMessageBoxEx);
XAM_EXPORT_STUB(__imp__XamShowAvatarAwardGamesUI);
XAM_EXPORT_STUB(__imp__XamShowAvatarAwardsUI);
XAM_EXPORT_STUB(__imp__XamShowAvatarMiniCreatorUI);
XAM_EXPORT_STUB(__imp__XamShowBadDiscErrorUI);
XAM_EXPORT_STUB(__imp__XamShowBeaconsUI);
XAM_EXPORT_STUB(__imp__XamShowBrandedKeyboardUI);
XAM_EXPORT_STUB(__imp__XamShowChangeGamerTileUI);
XAM_EXPORT_STUB(__imp__XamShowComplaintUI);
XAM_EXPORT_STUB(__imp__XamShowCreateProfileUI);
XAM_EXPORT_STUB(__imp__XamShowCreateProfileUIEx);
XAM_EXPORT_STUB(__imp__XamShowCsvTransitionUI);
XAM_EXPORT_STUB(__imp__XamShowCustomMessageComposeUI);
XAM_EXPORT_STUB(__imp__XamShowCustomPlayerListUI);
XAM_EXPORT_STUB(__imp__XamShowDirectAcquireUI);
XAM_EXPORT_STUB(__imp__XamShowEditProfileUI);
XAM_EXPORT_STUB(__imp__XamShowFirstRunWelcomeUI);
XAM_EXPORT_STUB(__imp__XamShowFitnessBodyProfileUI);
XAM_EXPORT_STUB(__imp__XamShowFitnessClearUI);
XAM_EXPORT_STUB(__imp__XamShowFitnessWarnAboutPrivacyUI);
XAM_EXPORT_STUB(__imp__XamShowFitnessWarnAboutTimeUI);
XAM_EXPORT_STUB(__imp__XamShowFofUI);
XAM_EXPORT_STUB(__imp__XamShowForcedNameChangeUI);
XAM_EXPORT_STUB(__imp__XamShowFriendRequestUI);
XAM_EXPORT_STUB(__imp__XamShowFriendsUI);
XAM_EXPORT_STUB(__imp__XamShowFriendsUIp);
XAM_EXPORT_STUB(__imp__XamShowGameInviteUI);
XAM_EXPORT_STUB(__imp__XamShowGameVoiceChannelUI);
XAM_EXPORT_STUB(__imp__XamShowGamerCardUI);
XAM_EXPORT_STUB(__imp__XamShowGamerCardUIForXUID);
XAM_EXPORT_STUB(__imp__XamShowGamerCardUIForXUIDp);
XAM_EXPORT_STUB(__imp__XamShowGamesUI);
XAM_EXPORT_STUB(__imp__XamShowGenericOnlineAppUI);
XAM_EXPORT_STUB(__imp__XamShowGoldUpgradeUI);
XAM_EXPORT_STUB(__imp__XamShowGraduateUserUI);
XAM_EXPORT_STUB(__imp__XamShowGuideUI);
XAM_EXPORT_STUB(__imp__XamShowJoinPartyUI);
XAM_EXPORT_STUB(__imp__XamShowJoinSessionByIdInProgressUI);
XAM_EXPORT_STUB(__imp__XamShowJoinSessionInProgressUI);
XAM_EXPORT_STUB(__imp__XamShowKeyboardUIMessenger);
XAM_EXPORT_STUB(__imp__XamShowLiveSignupUI);
XAM_EXPORT_STUB(__imp__XamShowLiveUpsellUI);
XAM_EXPORT_STUB(__imp__XamShowLiveUpsellUIEx);
XAM_EXPORT_STUB(__imp__XamShowMarketplaceDownloadItemsUI);
XAM_EXPORT_STUB(__imp__XamShowMarketplaceGetOrderReceipts);
XAM_EXPORT_STUB(__imp__XamShowMarketplacePurchaseOrderUI);
XAM_EXPORT_STUB(__imp__XamShowMarketplacePurchaseOrderUIEx);
XAM_EXPORT_STUB(__imp__XamShowMarketplaceUI);
XAM_EXPORT_STUB(__imp__XamShowMarketplaceUIEx);
XAM_EXPORT_STUB(__imp__XamShowMessageBox);
XAM_EXPORT_STUB(__imp__XamShowMessageComposeUI);
XAM_EXPORT_STUB(__imp__XamShowMessagesUI);
XAM_EXPORT_STUB(__imp__XamShowMessagesUIEx);
XAM_EXPORT_STUB(__imp__XamShowMessengerUI);
XAM_EXPORT_STUB(__imp__XamShowMultiplayerUpgradeUI);
XAM_EXPORT_STUB(__imp__XamShowNetworkStorageSyncUI);
XAM_EXPORT_STUB(__imp__XamShowNuiAchievementsUI);
XAM_EXPORT_STUB(__imp__XamShowNuiCommunitySessionsUI);
XAM_EXPORT_STUB(__imp__XamShowNuiControllerRequiredUI);
XAM_EXPORT_STUB(__imp__XamShowNuiDeviceSelectorUI);
XAM_EXPORT_STUB(__imp__XamShowNuiDirtyDiscErrorUI);
XAM_EXPORT_STUB(__imp__XamShowNuiFriendRequestUI);
XAM_EXPORT_STUB(__imp__XamShowNuiFriendsUI);
XAM_EXPORT_STUB(__imp__XamShowNuiGameInviteUI);
XAM_EXPORT_STUB(__imp__XamShowNuiGamerCardUIForXUID);
XAM_EXPORT_STUB(__imp__XamShowNuiGamesUI);
XAM_EXPORT_STUB(__imp__XamShowNuiGuideUI);
XAM_EXPORT_STUB(__imp__XamShowNuiHardwareRequiredUI);
XAM_EXPORT_STUB(__imp__XamShowNuiJoinSessionInProgressUI);
XAM_EXPORT_STUB(__imp__XamShowNuiMarketplaceDownloadItemsUI);
XAM_EXPORT_STUB(__imp__XamShowNuiMarketplaceUI);
XAM_EXPORT_STUB(__imp__XamShowNuiMessageBoxUI);
XAM_EXPORT_STUB(__imp__XamShowNuiMessagesUI);
XAM_EXPORT_STUB(__imp__XamShowNuiPartyUI);
XAM_EXPORT_STUB(__imp__XamShowNuiSigninUI);
XAM_EXPORT_STUB(__imp__XamShowNuiVideoRichPresenceUI);
XAM_EXPORT_STUB(__imp__XamShowOptionalMediaUpdateRequiredUI);
XAM_EXPORT_STUB(__imp__XamShowOptionalMediaUpdateRequiredUIEx);
XAM_EXPORT_STUB(__imp__XamShowOptionsUI);
XAM_EXPORT_STUB(__imp__XamShowPamUI);
XAM_EXPORT_STUB(__imp__XamShowPartyInviteUI);
XAM_EXPORT_STUB(__imp__XamShowPartyJoinInProgressUI);
XAM_EXPORT_STUB(__imp__XamShowPasscodeVerifyUI);
XAM_EXPORT_STUB(__imp__XamShowPasscodeVerifyUIEx);
XAM_EXPORT_STUB(__imp__XamShowPaymentOptionsUI);
XAM_EXPORT_STUB(__imp__XamShowPersonalizationUI);
XAM_EXPORT_STUB(__imp__XamShowPlayerReviewUI);
XAM_EXPORT_STUB(__imp__XamShowPlayersUI);
XAM_EXPORT_STUB(__imp__XamShowPrivateChatInviteUI);
XAM_EXPORT_STUB(__imp__XamShowQuickChatUI);
XAM_EXPORT_STUB(__imp__XamShowQuickChatUIp);
XAM_EXPORT_STUB(__imp__XamShowQuickLaunchUI);
XAM_EXPORT_STUB(__imp__XamShowRecentMessageUI);
XAM_EXPORT_STUB(__imp__XamShowRecentMessageUIEx);
XAM_EXPORT_STUB(__imp__XamShowReputationUI);
XAM_EXPORT_STUB(__imp__XamShowSigninUIEx);
XAM_EXPORT_STUB(__imp__XamShowSigninUIp);
XAM_EXPORT_STUB(__imp__XamShowSignupCreditCardUI);
XAM_EXPORT_STUB(__imp__XamShowSocialPostUI);
XAM_EXPORT_STUB(__imp__XamShowStorePickerUI);
XAM_EXPORT_STUB(__imp__XamShowTFAUI);
XAM_EXPORT_STUB(__imp__XamShowTermsOfUseUI);
XAM_EXPORT_STUB(__imp__XamShowUpdaterUI);
XAM_EXPORT_STUB(__imp__XamShowVideoChatInviteUI);
XAM_EXPORT_STUB(__imp__XamShowVideoRichPresenceUI);
XAM_EXPORT_STUB(__imp__XamShowVoiceMailUI);
XAM_EXPORT_STUB(__imp__XamShowVoiceSettingsUI);
XAM_EXPORT_STUB(__imp__XamShowWhatsOnUI);
XAM_EXPORT_STUB(__imp__XamShowWordRegisterUI);
XAM_EXPORT_STUB(__imp__XamSysUiDisableAutoClose);
