#include "xam/xam_title_lifecycle.h"

#include "xam/socket_object.h"
#include "xam/xam_state.h"
#include <rex/filesystem/xdbf.h>
#include <rex/xam/content_manager.h>
#include <rex/xam/achievement_manager.h>
#include <rex/xam/title_lifecycle.h>

namespace rex::xam {

XAMTitleLifecycle::XAMTitleLifecycle() = default;

XAMTitleLifecycle::~XAMTitleLifecycle() = default;

Result<void> XAMTitleLifecycle::AttachTitle(const XAMKernelInterfaces& title) {
  state_ = std::make_unique<XAMState>(title);
  return {};
}

void XAMTitleLifecycle::AttachSystems(rex::ui::IUISystem* ui, rex::hid::IInputSystem* input,
                                      const VideoMode& video_mode) {
  state_->AttachInput(input);
  state_->SetVideoMode(video_mode);
  state_->SetUI(ui);
}

void XAMTitleLifecycle::SetTitleLanguage(rex::kernel::XLanguage language) {
  state_->SetUserLanguage(language);
}

void XAMTitleLifecycle::DetachSystems() {
  if (state_) {
    state_->DetachInput();
    state_->SetUI(nullptr);
  }
}

void XAMTitleLifecycle::ShutdownSockets() {
  if (state_) {
    ::rex::xam::ShutdownSockets(state_->GetTitle().objects);
  }
}

void XAMTitleLifecycle::DestroyApps() {
  if (state_) {
    state_->ReleaseApps();
  }
}

void XAMTitleLifecycle::DetachTitle() {
  state_.reset();
}

IContentManager& XAMTitleLifecycle::GetContentManager() {
  return state_->GetContentManager();
}

IAchievementManager& XAMTitleLifecycle::GetAchievementManager() {
  return state_->GetAchievementManager();
}

u32 XAMTitleLifecycle::GetTitleID() const {
  return state_->GetTitleID();
}

std::unique_ptr<filesystem::IXDBFTitleData> XAMTitleLifecycle::GetTitleXDBF() const {
  return state_->OpenTitleXDBF();
}

}  // namespace rex::xam
