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

#include <rex/runtime/dispatcher.h>
#include <rex/runtime/notifier.h>
#include <rex/runtime/xthread.h>
#include <rex/api.h>
#include <rex/logging/log.h>
#include <rex/runtime.h>
#include <rex/string.h>

#include <rex/kernel/xam/private.h>
#include <rex/hook.h>
#include <rex/hid.h>
#include <rex/types.h>
#include <rex/runtime/xtypes.h>
#include <rex/platform/thread.h>
#include <rex/ui/user_interface.h>
#include "kernel/stub_log.h"
#include "runtime/thread_wait.h"

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::runtime;

extern std::atomic<int> g_xam_dialogs_shown;

namespace {

class ScopedTitleInputBlock : public rex::NonCopyable {
 public:
  ScopedTitleInputBlock() : hid_(runtime::GetCurrent().GetHID()) { hid_.AcquireUIBlock(); }
  ~ScopedTitleInputBlock() { hid_.ReleaseUIBlock(); }

 private:
  rex::hid::IHIDBackend& hid_;
};

bool HasWindow() {
  ui::IUserInterface* user_interface = runtime::GetCurrent().GetUI();
  return user_interface != nullptr && user_interface->GetWindow() != nullptr;
}

struct PromptWait {
  std::mutex mutex;
  bool abandoned = false;
  X_RESULT result = X_ERROR_CANCELLED;
  std::unique_ptr<rex::platform::Event> done = rex::platform::Event::CreateManualResetEvent(false);
};

class PromptShown : public rex::NonCopyable {
 public:
  explicit PromptShown(PromptWait& wait) : wait_(wait) { ++g_xam_dialogs_shown; }
  ~PromptShown() {
    std::lock_guard lock(wait_.mutex);
    wait_.abandoned = true;
    --g_xam_dialogs_shown;
  }

 private:
  PromptWait& wait_;
};

template <std::invocable<const ui::PromptResult&> F>
X_RESULT XAMPromptAndWait(const ui::PromptDesc& desc, const F& complete) {
  ScopedTitleInputBlock input_block;
  auto wait = std::make_shared<PromptWait>();
  PromptShown shown(*wait);
  runtime::GetCurrent().GetUI()->Prompt(desc, [wait, &complete](const ui::PromptResult& prompt) {
    std::lock_guard lock(wait->mutex);
    if (wait->abandoned) {
      return;
    }
    wait->result = complete(prompt);
    wait->done->Set();
  });
  runtime::WaitHostObject(wait->done.get(), std::chrono::milliseconds::max());
  std::lock_guard lock(wait->mutex);
  return wait->result;
}

}  // namespace

template <typename Close>
X_RESULT XAMDispatchPrompt(ui::PromptDesc desc, Close close, u32 overlapped) {
  auto pre = []() { runtime::GetCurrent().GetNotifier().BroadcastNotification(0x9, true); };
  auto post = []() {
    rex::runtime::KernelThread::Sleep(std::chrono::milliseconds(100));
    runtime::GetCurrent().GetNotifier().BroadcastNotification(0x9, false);
  };
  if constexpr (std::is_invocable_r_v<X_RESULT, Close&, const ui::PromptResult&, u32&, u32&>) {
    auto run = [desc = std::move(desc), close = std::move(close)](u32& extended_error,
                                                                  u32& length) -> X_RESULT {
      return XAMPromptAndWait(desc, [&](const ui::PromptResult& prompt) {
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
    runtime::GetCurrent().GetDispatcher().CompleteDeferred(
        overlapped,
        [run = std::move(run)]() mutable {
          u32 extended_error = 0;
          u32 length = 0;
          const X_RESULT result = run(extended_error, length);
          return runtime::CompletionOptions{
              .result = result, .extended_error = extended_error, .length = length};
        },
        pre, post);
    return X_ERROR_IO_PENDING;
  } else {
    auto run = [desc = std::move(desc), close = std::move(close)]() -> X_RESULT {
      return XAMPromptAndWait(desc, [&](const ui::PromptResult& prompt) { return close(prompt); });
    };
    if (!overlapped) {
      pre();
      auto result = run();
      post();
      return result;
    }
    runtime::GetCurrent().GetDispatcher().CompleteDeferred(
        overlapped,
        [run = std::move(run)]() mutable {
          return runtime::CompletionOptions{.result = run(), .length = 0};
        },
        pre, post);
    return X_ERROR_IO_PENDING;
  }
}

X_RESULT XAMDispatchHeadless(const std::function<X_RESULT()>& run_callback, u32 overlapped) {
  auto pre = []() {
    REXLOG_DEBUG(kernel, "XAMDispatchHeadless: Broadcasting XN_SYS_UI = true");
    runtime::GetCurrent().GetNotifier().BroadcastNotification(0x9, true);
  };
  auto post = []() {
    rex::runtime::KernelThread::Sleep(std::chrono::milliseconds(100));
    REXLOG_DEBUG(kernel, "XAMDispatchHeadless: Broadcasting XN_SYS_UI = false");
    runtime::GetCurrent().GetNotifier().BroadcastNotification(0x9, false);
  };
  if (!overlapped) {
    pre();
    auto result = run_callback();
    post();
    return result;
  } else {
    runtime::GetCurrent().GetDispatcher().CompleteDeferred(
        overlapped,
        [run_callback]() {
          return runtime::CompletionOptions{.result = run_callback(), .length = 0};
        },
        pre, post);
    return X_ERROR_IO_PENDING;
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
    title = rex::ToUTF8(rex::memory::load_and_swap<std::u16string>(
        runtime::GetCurrent().GetMemory()->TranslateVirtual(title_ptr.GetGuestAddress())));
  }
  std::string text_str =
      text_ptr
          ? rex::ToUTF8(rex::memory::load_and_swap<std::u16string>(
                runtime::GetCurrent().GetMemory()->TranslateVirtual(text_ptr.GetGuestAddress())))
          : "";

  std::vector<std::string> buttons;
  for (u32 i = 0; i < button_count; ++i) {
    u32 button_ptr = button_ptrs[i];
    auto button = rex::memory::load_and_swap<std::u16string>(
        runtime::GetCurrent().GetMemory()->TranslateVirtual(button_ptr));
    buttons.push_back(rex::ToUTF8(button));
  }

  X_RESULT result;
  if (runtime::GetCurrent().GetConfig().runtime.kernel.headless || !HasWindow()) {
    auto run = [result_ptr, active_button]() -> X_RESULT {
      *result_ptr = static_cast<u32>(active_button);
      return X_ERROR_SUCCESS;
    };
    result = XAMDispatchHeadless(run, overlapped.GetGuestAddress());
  } else {
    ui::PromptDesc desc{
        .title = title.empty() ? "Message Box" : title, .body = text_str, .buttons = buttons};
    auto close = [result_ptr, active_button](const ui::PromptResult& prompt) -> X_RESULT {
      *result_ptr = prompt.canceled ? static_cast<u32>(active_button) : prompt.button;
      return X_ERROR_SUCCESS;
    };
    result = XAMDispatchPrompt(std::move(desc), close, overlapped.GetGuestAddress());
  }
  return result;
}

}  // namespace

u32 XamShowMessageBoxUI_entry(u32 user_index, MappedWString title_ptr, MappedWString text_ptr,
                              u32 button_count, MappedU32 button_ptrs, u32 active_button, u32 flags,
                              MappedU32 result_ptr, MappedVoid overlapped) {
  REXLOG_DEBUG(
      kernel,
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
  REXLOG_DEBUG(kernel,
               "XamShowMessageBoxUIEx({:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {:08X}, "
               "{:08X}, {:08X}, {:08X})",
               static_cast<u32>(user_index), title_ptr.GetGuestAddress(),
               text_ptr.GetGuestAddress(), static_cast<u32>(button_count),
               button_ptrs.GetGuestAddress(), static_cast<u32>(active_button),
               static_cast<u32>(flags), static_cast<u32>(extra), result_ptr.GetGuestAddress(),
               overlapped.GetGuestAddress());
  return ShowMessageBox(title_ptr, text_ptr, button_count, button_ptrs, active_button, result_ptr,
                        overlapped);
}

u32 XamShowKeyboardUI_entry(u32 user_index, u32 flags, MappedWString default_text,
                            MappedWString title, MappedWString description, MappedWString buffer,
                            u32 buffer_length, MappedVoid overlapped) {
  REXLOG_DEBUG(
      kernel, "XamShowKeyboardUI({:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {:08X})",
      static_cast<u32>(user_index), static_cast<u32>(flags), default_text.GetGuestAddress(),
      title.GetGuestAddress(), description.GetGuestAddress(), buffer.GetGuestAddress(),
      static_cast<u32>(buffer_length), overlapped.GetGuestAddress());
  if (!buffer) {
    return X_ERROR_INVALID_PARAMETER;
  }

  REX_ASSERT(overlapped.GetGuestAddress() != 0);

  auto buffer_size = static_cast<size_t>(buffer_length) * 2;

  X_RESULT result;
  if (runtime::GetCurrent().GetConfig().runtime.kernel.headless || !HasWindow()) {
    auto run = [default_text, buffer, buffer_length, buffer_size]() -> X_RESULT {
      if (!default_text) {
        std::memset(buffer, 0, buffer_size);
      } else {
        rex::CopyTruncating(buffer, default_text.GetValue(), buffer_length);
      }
      return X_ERROR_SUCCESS;
    };
    result = XAMDispatchHeadless(run, overlapped.GetGuestAddress());
  } else {
    auto close = [buffer, buffer_length](const ui::PromptResult& prompt, u32& extended_error,
                                         u32& length) -> X_RESULT {
      length = 0;
      if (prompt.canceled || prompt.button != 0) {
        extended_error = X_ERROR_CANCELLED;
        return X_ERROR_SUCCESS;
      }
      std::string padded = prompt.text;
      padded.resize(static_cast<size_t>(buffer_length) + 1, '\0');
      auto text = rex::ToUTF16(padded);
      rex::CopyAndSwapTruncating(buffer, text, buffer_length);
      extended_error = X_ERROR_SUCCESS;
      return X_ERROR_SUCCESS;
    };

    std::string title_str =
        title ? rex::ToUTF8(rex::memory::load_and_swap<std::u16string>(
                    runtime::GetCurrent().GetMemory()->TranslateVirtual(title.GetGuestAddress())))
              : "";
    std::string desc_str = description ? rex::ToUTF8(rex::memory::load_and_swap<std::u16string>(
                                             runtime::GetCurrent().GetMemory()->TranslateVirtual(
                                                 description.GetGuestAddress())))
                                       : "";
    std::string def_text_str = default_text
                                   ? rex::ToUTF8(rex::memory::load_and_swap<std::u16string>(
                                         runtime::GetCurrent().GetMemory()->TranslateVirtual(
                                             default_text.GetGuestAddress())))
                                   : "";
    if (title_str.empty()) {
      if (desc_str.empty()) {
        title_str = "Keyboard Input";
      } else {
        title_str = desc_str;
        desc_str.clear();
      }
    }

    ui::PromptDesc desc{.title = title_str,
                        .body = desc_str,
                        .buttons = {"OK", "Cancel"},
                        .text_input = true,
                        .default_text = def_text_str,
                        .max_length = buffer_length};
    result = XAMDispatchPrompt(std::move(desc), close, overlapped.GetGuestAddress());
  }
  return result;
}

u32 XamShowDeviceSelectorUI_entry(u32 user_index, u32 content_type, u32 content_flags,
                                  u64 total_requested, MappedU32 device_id_ptr,
                                  MappedVoid overlapped) {
  REXLOG_DEBUG(kernel, "XamShowDeviceSelectorUI({:08X}, {:08X}, {:08X}, {:016X}, {:08X}, {:08X})",
               static_cast<u32>(user_index), static_cast<u32>(content_type),
               static_cast<u32>(content_flags), static_cast<u64>(total_requested),
               device_id_ptr.GetGuestAddress(), overlapped.GetGuestAddress());
  return XAMDispatchHeadless(
      [device_id_ptr]() -> X_RESULT {
        // 0x00000001 is DummyDeviceId::kHDD.
        *device_id_ptr = 0x00000001;
        return X_ERROR_SUCCESS;
      },
      overlapped.GetGuestAddress());
}

void XamShowDirtyDiscErrorUI_entry(u32 user_index) {
  REXLOG_ERROR(kernel, "XamShowDirtyDiscErrorUI called! user_index={}",
               static_cast<u32>(user_index));
  REXLOG_ERROR(kernel,
               "This indicates a disc or file read error. Check that all game files exist.");

  if (HasWindow()) {
    XAMDispatchPrompt(
        ui::PromptDesc{.title = "Disc Read Error",
                       .body = "There's been an issue reading content from the game disc.\nThis is "
                               "likely caused by bad or unimplemented file IO calls.",
                       .buttons = {"OK"}},
        [](const ui::PromptResult&) -> X_RESULT { return X_ERROR_SUCCESS; }, 0);
  } else {
    REXLOG_ERROR(kernel, "===========================================");
    REXLOG_ERROR(kernel, "FATAL: Disc Read Error (no UI to display)");
    REXLOG_ERROR(kernel, "Check that all game content files are present");
    REXLOG_ERROR(kernel, "Missing files or bad mounts cause this error");
    REXLOG_ERROR(kernel, "===========================================");
  }
  runtime::GetCurrent().ExitTitle(rex::runtime::TitleExitReason::kTitleDataError);
}

u32 XamShowPartyUI_entry([[maybe_unused]] u32 r3, [[maybe_unused]] u32 r4) {
  return X_ERROR_FUNCTION_FAILED;
}

u32 XamShowCommunitySessionsUI_entry([[maybe_unused]] u32 r3, [[maybe_unused]] u32 r4) {
  return X_ERROR_FUNCTION_FAILED;
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
