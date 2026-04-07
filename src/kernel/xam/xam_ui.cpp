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
#include <rex/hook.h>
#include <rex/types.h>
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

u32 XamIsUIActive_entry() {
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
u32 XamShowMessageBoxUI_entry(u32 user_index, mapped_wstring title_ptr, mapped_wstring text_ptr,
                              u32 button_count, mapped_u32 button_ptrs, u32 active_button,
                              u32 flags, mapped_u32 result_ptr, mapped_void overlapped) {
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
u32 XamShowKeyboardUI_entry(u32 user_index, u32 flags, mapped_wstring default_text,
                            mapped_wstring title, mapped_wstring description, mapped_wstring buffer,
                            u32 buffer_length, mapped_void overlapped) {
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

u32 XamShowDeviceSelectorUI_entry(u32 user_index, u32 content_type, u32 content_flags,
                                  u64 total_requested, mapped_u32 device_id_ptr,
                                  mapped_void overlapped) {
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

void XamShowDirtyDiscErrorUI_entry(u32 user_index) {
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

u32 XamShowPartyUI_entry(u32 r3, u32 r4) {
  return X_ERROR_FUNCTION_FAILED;
}

u32 XamShowCommunitySessionsUI_entry(u32 r3, u32 r4) {
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

REX_EXPORT(__imp__XamIsUIActive, rex::kernel::xam::XamIsUIActive_entry)
REX_EXPORT(__imp__XamShowMessageBoxUI, rex::kernel::xam::XamShowMessageBoxUI_entry)
REX_EXPORT(__imp__XamShowKeyboardUI, rex::kernel::xam::XamShowKeyboardUI_entry)
REX_EXPORT(__imp__XamShowDeviceSelectorUI, rex::kernel::xam::XamShowDeviceSelectorUI_entry)
REX_EXPORT(__imp__XamShowDirtyDiscErrorUI, rex::kernel::xam::XamShowDirtyDiscErrorUI_entry)
REX_EXPORT(__imp__XamShowPartyUI, rex::kernel::xam::XamShowPartyUI_entry)
REX_EXPORT(__imp__XamShowCommunitySessionsUI, rex::kernel::xam::XamShowCommunitySessionsUI_entry)
REX_EXPORT(__imp__XamShowMessageBoxUIEx, rex::kernel::xam::XamShowMessageBoxUIEx_entry)

REX_EXPORT_STUB(__imp__XamIsGuideDisabled);
REX_EXPORT_STUB(__imp__XamIsMessageBoxActive);
REX_EXPORT_STUB(__imp__XamIsNuiUIActive);
REX_EXPORT_STUB(__imp__XamIsSysUiInvokedByTitle);
REX_EXPORT_STUB(__imp__XamIsSysUiInvokedByXenonButton);
REX_EXPORT_STUB(__imp__XamIsUIThread);
REX_EXPORT_STUB(__imp__XamNavigate);
REX_EXPORT_STUB(__imp__XamNavigateBack);
REX_EXPORT_STUB(__imp__XamOverrideHudOpenType);
REX_EXPORT_STUB(__imp__XamShowAchievementDetailsUI);
REX_EXPORT_STUB(__imp__XamShowAchievementsUI);
REX_EXPORT_STUB(__imp__XamShowAchievementsUIEx);
REX_EXPORT_STUB(__imp__XamShowAndWaitForMessageBoxEx);
REX_EXPORT_STUB(__imp__XamShowAvatarAwardGamesUI);
REX_EXPORT_STUB(__imp__XamShowAvatarAwardsUI);
REX_EXPORT_STUB(__imp__XamShowAvatarMiniCreatorUI);
REX_EXPORT_STUB(__imp__XamShowBadDiscErrorUI);
REX_EXPORT_STUB(__imp__XamShowBeaconsUI);
REX_EXPORT_STUB(__imp__XamShowBrandedKeyboardUI);
REX_EXPORT_STUB(__imp__XamShowChangeGamerTileUI);
REX_EXPORT_STUB(__imp__XamShowComplaintUI);
REX_EXPORT_STUB(__imp__XamShowCreateProfileUI);
REX_EXPORT_STUB(__imp__XamShowCreateProfileUIEx);
REX_EXPORT_STUB(__imp__XamShowCsvTransitionUI);
REX_EXPORT_STUB(__imp__XamShowCustomMessageComposeUI);
REX_EXPORT_STUB(__imp__XamShowCustomPlayerListUI);
REX_EXPORT_STUB(__imp__XamShowDirectAcquireUI);
REX_EXPORT_STUB(__imp__XamShowEditProfileUI);
REX_EXPORT_STUB(__imp__XamShowFirstRunWelcomeUI);
REX_EXPORT_STUB(__imp__XamShowFitnessBodyProfileUI);
REX_EXPORT_STUB(__imp__XamShowFitnessClearUI);
REX_EXPORT_STUB(__imp__XamShowFitnessWarnAboutPrivacyUI);
REX_EXPORT_STUB(__imp__XamShowFitnessWarnAboutTimeUI);
REX_EXPORT_STUB(__imp__XamShowFofUI);
REX_EXPORT_STUB(__imp__XamShowForcedNameChangeUI);
REX_EXPORT_STUB(__imp__XamShowFriendRequestUI);
REX_EXPORT_STUB(__imp__XamShowFriendsUI);
REX_EXPORT_STUB(__imp__XamShowFriendsUIp);
REX_EXPORT_STUB(__imp__XamShowGameInviteUI);
REX_EXPORT_STUB(__imp__XamShowGameVoiceChannelUI);
REX_EXPORT_STUB(__imp__XamShowGamerCardUI);
REX_EXPORT_STUB(__imp__XamShowGamerCardUIForXUID);
REX_EXPORT_STUB(__imp__XamShowGamerCardUIForXUIDp);
REX_EXPORT_STUB(__imp__XamShowGamesUI);
REX_EXPORT_STUB(__imp__XamShowGenericOnlineAppUI);
REX_EXPORT_STUB(__imp__XamShowGoldUpgradeUI);
REX_EXPORT_STUB(__imp__XamShowGraduateUserUI);
REX_EXPORT_STUB(__imp__XamShowGuideUI);
REX_EXPORT_STUB(__imp__XamShowJoinPartyUI);
REX_EXPORT_STUB(__imp__XamShowJoinSessionByIdInProgressUI);
REX_EXPORT_STUB(__imp__XamShowJoinSessionInProgressUI);
REX_EXPORT_STUB(__imp__XamShowKeyboardUIMessenger);
REX_EXPORT_STUB(__imp__XamShowLiveSignupUI);
REX_EXPORT_STUB(__imp__XamShowLiveUpsellUI);
REX_EXPORT_STUB(__imp__XamShowLiveUpsellUIEx);
REX_EXPORT_STUB(__imp__XamShowMarketplaceDownloadItemsUI);
REX_EXPORT_STUB(__imp__XamShowMarketplaceGetOrderReceipts);
REX_EXPORT_STUB(__imp__XamShowMarketplacePurchaseOrderUI);
REX_EXPORT_STUB(__imp__XamShowMarketplacePurchaseOrderUIEx);
REX_EXPORT_STUB(__imp__XamShowMarketplaceUI);
REX_EXPORT_STUB(__imp__XamShowMarketplaceUIEx);
REX_EXPORT_STUB(__imp__XamShowMessageBox);
REX_EXPORT_STUB(__imp__XamShowMessageComposeUI);
REX_EXPORT_STUB(__imp__XamShowMessagesUI);
REX_EXPORT_STUB(__imp__XamShowMessagesUIEx);
REX_EXPORT_STUB(__imp__XamShowMessengerUI);
REX_EXPORT_STUB(__imp__XamShowMultiplayerUpgradeUI);
REX_EXPORT_STUB(__imp__XamShowNetworkStorageSyncUI);
REX_EXPORT_STUB(__imp__XamShowNuiAchievementsUI);
REX_EXPORT_STUB(__imp__XamShowNuiCommunitySessionsUI);
REX_EXPORT_STUB(__imp__XamShowNuiControllerRequiredUI);
REX_EXPORT_STUB(__imp__XamShowNuiDeviceSelectorUI);
REX_EXPORT_STUB(__imp__XamShowNuiDirtyDiscErrorUI);
REX_EXPORT_STUB(__imp__XamShowNuiFriendRequestUI);
REX_EXPORT_STUB(__imp__XamShowNuiFriendsUI);
REX_EXPORT_STUB(__imp__XamShowNuiGameInviteUI);
REX_EXPORT_STUB(__imp__XamShowNuiGamerCardUIForXUID);
REX_EXPORT_STUB(__imp__XamShowNuiGamesUI);
REX_EXPORT_STUB(__imp__XamShowNuiGuideUI);
REX_EXPORT_STUB(__imp__XamShowNuiHardwareRequiredUI);
REX_EXPORT_STUB(__imp__XamShowNuiJoinSessionInProgressUI);
REX_EXPORT_STUB(__imp__XamShowNuiMarketplaceDownloadItemsUI);
REX_EXPORT_STUB(__imp__XamShowNuiMarketplaceUI);
REX_EXPORT_STUB(__imp__XamShowNuiMessageBoxUI);
REX_EXPORT_STUB(__imp__XamShowNuiMessagesUI);
REX_EXPORT_STUB(__imp__XamShowNuiPartyUI);
REX_EXPORT_STUB(__imp__XamShowNuiSigninUI);
REX_EXPORT_STUB(__imp__XamShowNuiVideoRichPresenceUI);
REX_EXPORT_STUB(__imp__XamShowOptionalMediaUpdateRequiredUI);
REX_EXPORT_STUB(__imp__XamShowOptionalMediaUpdateRequiredUIEx);
REX_EXPORT_STUB(__imp__XamShowOptionsUI);
REX_EXPORT_STUB(__imp__XamShowPamUI);
REX_EXPORT_STUB(__imp__XamShowPartyInviteUI);
REX_EXPORT_STUB(__imp__XamShowPartyJoinInProgressUI);
REX_EXPORT_STUB(__imp__XamShowPasscodeVerifyUI);
REX_EXPORT_STUB(__imp__XamShowPasscodeVerifyUIEx);
REX_EXPORT_STUB(__imp__XamShowPaymentOptionsUI);
REX_EXPORT_STUB(__imp__XamShowPersonalizationUI);
REX_EXPORT_STUB(__imp__XamShowPlayerReviewUI);
REX_EXPORT_STUB(__imp__XamShowPlayersUI);
REX_EXPORT_STUB(__imp__XamShowPrivateChatInviteUI);
REX_EXPORT_STUB(__imp__XamShowQuickChatUI);
REX_EXPORT_STUB(__imp__XamShowQuickChatUIp);
REX_EXPORT_STUB(__imp__XamShowQuickLaunchUI);
REX_EXPORT_STUB(__imp__XamShowRecentMessageUI);
REX_EXPORT_STUB(__imp__XamShowRecentMessageUIEx);
REX_EXPORT_STUB(__imp__XamShowReputationUI);
REX_EXPORT_STUB(__imp__XamShowSigninUIEx);
REX_EXPORT_STUB(__imp__XamShowSigninUIp);
REX_EXPORT_STUB(__imp__XamShowSignupCreditCardUI);
REX_EXPORT_STUB(__imp__XamShowSocialPostUI);
REX_EXPORT_STUB(__imp__XamShowStorePickerUI);
REX_EXPORT_STUB(__imp__XamShowTFAUI);
REX_EXPORT_STUB(__imp__XamShowTermsOfUseUI);
REX_EXPORT_STUB(__imp__XamShowUpdaterUI);
REX_EXPORT_STUB(__imp__XamShowVideoChatInviteUI);
REX_EXPORT_STUB(__imp__XamShowVideoRichPresenceUI);
REX_EXPORT_STUB(__imp__XamShowVoiceMailUI);
REX_EXPORT_STUB(__imp__XamShowVoiceSettingsUI);
REX_EXPORT_STUB(__imp__XamShowWhatsOnUI);
REX_EXPORT_STUB(__imp__XamShowWordRegisterUI);
REX_EXPORT_STUB(__imp__XamSysUiDisableAutoClose);
