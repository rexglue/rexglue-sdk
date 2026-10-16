#include "hid/module_base.h"

#include <array>

#include <rex/logging/log.h>

#include "hid/cvar_table.h"

namespace rex::hid {

std::span<const char* const> HIDModuleBase::GetRequiredInterfaces() const {
  static constexpr std::array<const char*, 1> kRequired = {"rex.cvar@1"};
  return kRequired;
}

Result<void> HIDModuleBase::Connect(IInterfaceFactory* factory, const Config& config) {
  REX_TRY(cvars_, factory->Get<cvar::ICvarRegistry>());
  if (auto added = cvars_->Add(cvars::kHIDCvars); !added) {
    cvars_ = nullptr;
    return added;
  }
  hub_.AttachCvars(*cvars_);
  config_ = config.hid;
  if (config_.shared_pads) {
    hub_.SetDeviceAssignment(std::make_unique<SharedAssignment>());
  } else {
    hub_.SetDeviceAssignment(std::make_unique<SlotAssignment>());
  }
  return Ok();
}

void HIDModuleBase::Disconnect() {
  if (cvars_) {
    hub_.DetachCvars();
    cvars_->Unregister(cvars::kHIDCvars);
  }
  cvars_ = nullptr;
}

Result<void> HIDModuleBase::Init() {
  auto driver = CreatePadDriver(config_);
  if (!driver) {
    REXLOG_WARN(hid, "{} runs without its pad driver, {}", GetName(), driver.error().message);
    return Ok();
  }
  hub_.AddPadDriver(std::move(*driver));
  return Ok();
}

void HIDModuleBase::Shutdown() {
  hub_.ClearPadDrivers();
}

}  // namespace rex::hid
