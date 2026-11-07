/**
 * @file        xam/xam_state.h
 * @brief       What the XAM services own for one running title
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/hid/input_system.h>
#include <rex/kernel/types/content.h>
#include <rex/kernel/title_runtime.h>
#include <rex/xam/app_manager.h>
#include <rex/xam/title_lifecycle.h>
#include "xam/xam_title.h"
#include <rex/xam/user_profile.h>
#include <rex/kernel/heap.h>
#include <rex/types.h>
#include <rex/utility.h>

#include "xam/achievement_manager.h"
#include "xam/content_manager.h"

namespace rex::ui {
class IUISystem;
}  // namespace rex::ui

namespace rex::xam {

/** The launch request state that the XAM loader exports keep. */
struct XAMLoaderState {
  bool launch_data_present = false; /**< True once the title stored launch data. */
  std::vector<u8> launch_data;
  u32 launch_flags = 0;
  std::string launch_path; /**< `game:\default.xex` when the title passed no path. */
};

class XAMState;

/** Returns the state of the attached title, null while no title is attached. */
XAMState* FindCurrentXAM();

/** Returns the state of the attached title, which must exist. */
XAMState& CurrentXAM();

/** Returns the kernel interfaces of the attached title, which must exist. */
const XAMTitle& CurrentTitle();

/**
 * The content, profile, achievement and app managers, the XAM heap and the input connection of
 * one running title.
 */
class XAMState : public rex::NonCopyable {
 public:
  /**
   * Creates the managers and registers the apps.
   *
   * @param title The kernel interfaces of the title. Each outlives this object.
   */
  explicit XAMState(const XAMKernelInterfaces& title);

  /** Frees every live XAM heap block. */
  ~XAMState();

  ContentManager& GetContentManager() { return *content_manager_; }

  AchievementManager& GetAchievementManager() { return *achievement_manager_; }

  rex::xam::UserProfile& GetUserProfile() { return *user_profile_; }

  rex::xam::AppManager& GetAppManager() { return *app_manager_; }

  /** Returns the title ID of the executable, 0 when the executable has no execution info. */
  u32 GetTitleID() const;

  /**
   * Returns the XDBF data of the executable, which reports invalid when the executable has no XDBF
   * section.
   */
  std::unique_ptr<rex::filesystem::IXDBFTitleData> OpenTitleXDBF() const;

  /** Destroys the app manager, which stops the registered apps. */
  void ReleaseApps();

  /**
   * Allocates a block from the XAM heap, which XamAlloc hands out for the system heap flag.
   *
   * @param size The size in bytes.
   * @return The guest address, 0 when the heap has no room.
   */
  u32 AllocHeap(u32 size);

  /**
   * Frees a block from AllocHeap.
   *
   * @return False when the address is not a live XAM heap block.
   */
  bool FreeHeap(u32 address);

  /**
   * Connects the input system the XamInput exports read and watches its pads. Null leaves the
   * title with no pads.
   */
  void AttachInput(rex::hid::IInputSystem* input);

  /** Stops watching the pads and forgets the input system. */
  void DetachInput();

  rex::hid::IInputSystem* GetInput() const { return input_; }

  u32 GetOnlineSchemaAddress() const { return online_schema_address_; }

  void SetOnlineSchemaAddress(u32 address) { online_schema_address_ = address; }

  /** Returns the XNetStartup parameter bytes, all 0 until the title sets them. */
  std::array<u8, 13>& GetNetStartupParams() { return net_startup_params_; }

  void IncrementShownPrompts() { ++shown_prompts_; }

  void DecrementShownPrompts() { --shown_prompts_; }

  int GetShownPrompts() const { return shown_prompts_; }

  void SetUI(rex::ui::IUISystem* ui) { ui_ = ui; }

  rex::ui::IUISystem* GetUI() const { return ui_; }

  void SetVideoMode(const VideoMode& mode) { video_mode_ = mode; }

  const VideoMode& GetVideoMode() const { return video_mode_; }

  void SetUserLanguage(rex::kernel::XLanguage language) { user_language_ = language; }

  rex::kernel::XLanguage GetUserLanguage() const { return user_language_; }

  const XAMTitle& GetTitle() const { return title_; }

  XAMLoaderState& GetLoaderState() { return loader_state_; }

  /**
   * Counts XamInputGetState calls so the log can stop after the first few.
   *
   * @return The number of calls counted, this one included.
   */
  int CountInputStateCall() { return ++input_state_calls_; }

 private:
  XAMTitle title_;
  std::unique_ptr<rex::xam::AppManager> app_manager_;
  std::unique_ptr<rex::xam::UserProfile> user_profile_;
  std::unique_ptr<ContentManager> content_manager_;
  std::unique_ptr<AchievementManager> achievement_manager_;
  std::mutex heap_mutex_;
  std::unordered_map<u32, u32> heap_blocks_;
  rex::hid::IInputSystem* input_ = nullptr;
  u32 online_schema_address_ = 0;
  rex::ui::IUISystem* ui_ = nullptr;
  VideoMode video_mode_;
  rex::kernel::XLanguage user_language_ = rex::kernel::XLanguage::kEnglish;
  XAMLoaderState loader_state_;
  std::array<u8, 13> net_startup_params_{};
  std::atomic<int> shown_prompts_{0};
  std::atomic<int> input_state_calls_{0};
  ScopeGuard pad_watch_;
};

}  // namespace rex::xam
