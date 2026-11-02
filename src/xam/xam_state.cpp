#include "xam/xam_state.h"

#include <atomic>

#include <rex/assert.h>
#include <rex/logging/log.h>
#include <rex/xam/title_lifecycle.h>
#include <rex/kernel/object/notifier.h>

#include "xam/app/apps.h"

namespace rex::xam {

namespace {

constexpr u32 kXNotificationSystemInputDevicesChanged = 0x00000012;

std::atomic<XAMState*> g_current{nullptr};

}  // namespace

XAMState* FindCurrentXAM() {
  return g_current.load(std::memory_order_acquire);
}

XAMState& CurrentXAM() {
  XAMState* state = FindCurrentXAM();
  REX_ASSERT(state != nullptr);
  return *state;
}

const XAMKernelInterfaces& CurrentTitle() {
  return CurrentXAM().GetTitle();
}

XAMState::XAMState(const XAMKernelInterfaces& title) : title_(title) {
  app_manager_ = std::make_unique<rex::xam::AppManager>();
  user_profile_ = std::make_unique<rex::xam::UserProfile>();
  content_manager_ = std::make_unique<ContentManager>(title_, *user_profile_);
  user_profile_->SetContentManager(*content_manager_,
                                   [this] { return content_manager_->GetTitleID(); });
  achievement_manager_ = std::make_unique<AchievementManager>();
  app_manager_->RegisterApp(std::make_unique<apps::XMPApp>(title_));
  app_manager_->RegisterApp(std::make_unique<apps::XGIApp>(title_, *achievement_manager_));
  app_manager_->RegisterApp(std::make_unique<apps::XLiveBaseApp>(title_));
  app_manager_->RegisterApp(std::make_unique<apps::XAMApp>(title_, *content_manager_));
  g_current.store(this, std::memory_order_release);
}

XAMState::~XAMState() {
  XAMState* self = this;
  g_current.compare_exchange_strong(self, nullptr, std::memory_order_acq_rel);
  std::lock_guard lock(heap_mutex_);
  for (const auto& block : heap_blocks_) {
    title_.heap.SystemHeapFree(block.first);
  }
  heap_blocks_.clear();
}

u32 XAMState::GetTitleID() const {
  return content_manager_->GetTitleID();
}

std::unique_ptr<rex::filesystem::IXDBFTitleData> XAMState::OpenTitleXDBF() const {
  return content_manager_->GetTitleXDBF();
}

void XAMState::ReleaseApps() {
  app_manager_.reset();
}

u32 XAMState::AllocHeap(u32 size) {
  const u32 address = title_.heap.SystemHeapAlloc(size);
  if (address) {
    std::lock_guard lock(heap_mutex_);
    heap_blocks_.insert_or_assign(address, size);
  }
  return address;
}

bool XAMState::FreeHeap(u32 address) {
  {
    std::lock_guard lock(heap_mutex_);
    if (heap_blocks_.erase(address) == 0) {
      return false;
    }
  }
  title_.heap.SystemHeapFree(address);
  return true;
}

void XAMState::AttachInput(rex::hid::IInputSystem* input) {
  kernel::INotifier& notifier = title_.notifier;
  input_ = input;
  if (input_) {
    pad_watch_ = input_->OnConnectionChange([&notifier](u32, bool) {
      notifier.BroadcastNotification(kXNotificationSystemInputDevicesChanged, 0);
    });
  } else {
    REXLOG_INFO(hid, "no input system is attached, the title sees no pads");
  }
}

void XAMState::DetachInput() {
  pad_watch_ = {};
  input_ = nullptr;
}

}  // namespace rex::xam
