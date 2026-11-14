/**
 * @file        xam/xam_title_lifecycle.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>

#include <rex/xam/title_lifecycle.h>

namespace rex::xam {

class XAMState;

/** Creates and destroys the XAM state of a title. */
class XAMTitleLifecycle final : public IXAMTitleLifecycle, public IXAMTitleServices {
 public:
  XAMTitleLifecycle();
  ~XAMTitleLifecycle() override;

  [[nodiscard]] Result<void> AttachTitle(const XAMKernelInterfaces& title) override;
  void AttachSystems(rex::ui::IUISystem* ui, rex::hid::IInputSystem* input,
                     const VideoMode& video_mode) override;
  void SetTitleLanguage(rex::kernel::XLanguage language) override;
  void DetachSystems() override;
  void ShutdownSockets() override;
  void DestroyApps() override;
  void DetachTitle() override;
  [[nodiscard]] IContentManager& GetContentManager() override;
  [[nodiscard]] IAchievementManager& GetAchievementManager() override;
  [[nodiscard]] u32 GetTitleID() const override;
  [[nodiscard]] std::unique_ptr<filesystem::IXDBFTitleData> GetTitleXDBF() const override;

 private:
  std::unique_ptr<XAMState> state_;
};

}  // namespace rex::xam
