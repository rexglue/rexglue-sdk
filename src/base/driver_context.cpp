#include "base/driver_context.h"

#include <atomic>

#include <rex/logging/log_category.h>

#include <rex/utility.h>

namespace rex {

AppDriverContext::AppDriverContext(ISettingsRegistry& settings, ILogger& logger)
    : settings_(settings), logger_(logger), init_thread_(std::this_thread::get_id()) {}

ISettingsRegistry& AppDriverContext::GetSettingsRegistry() {
  return settings_;
}

ILogger& AppDriverContext::GetLogger() {
  return logger_;
}

Result<void> AppDriverContext::RunOnUIThread(MoveOnlyFunction<void()> fn) {
  if (!ui_) {
    if (std::this_thread::get_id() != init_thread_) {
      return Err(ErrorCategory::kRuntime, "the app has no UI thread to run the function on");
    }
    fn();
    return Ok();
  }
  if (ui_->IsUIThread()) {
    fn();
    return Ok();
  }
  if (!ui_->RunSynchronous(std::move(fn))) {
    return Err(ErrorCategory::kRuntime, "the UI system stopped");
  }
  return Ok();
}

void AppDriverContext::PostToUIThread(MoveOnlyFunction<void()> fn) {
  if (!ui_) {
    if (std::this_thread::get_id() == init_thread_) {
      fn();
    } else {
      static std::atomic<bool> warned{false};
      if (!warned.exchange(true)) {
        REXLOG_WARN(
            "work posted to the UI thread from another thread is dropped, the app has "
            "no UI");
      }
    }
    return;
  }
  if (ui_->IsUIThread()) {
    fn();
    return;
  }
  ui_->PostTask(std::move(fn));
}

void AppDriverContext::SetUI(IUIThread* ui) {
  ui_ = ui;
}

}  // namespace rex
