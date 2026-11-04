#include "apu/audio_module.h"

#include <rex/logging/log.h>
#include <rex/logging/logger.h>
#include <rex/settings.h>
#include <rex/driver_module.h>

namespace rex::apu {

std::span<const InterfaceType> AudioModule::GetRequiredInterfaces() const {
  static constexpr InterfaceType kRequired[] = {IDriverContext::kInterface, IAppInfo::kInterface,
                                                ISettings::kInterface, ILogger::kInterface};
  return kRequired;
}

AudioSystem& AudioModule::GetOrCreateSystem() {
  if (!system_ || retired_) {
    system_ = std::make_unique<AudioSystem>();
    retired_ = false;
  }
  return *system_;
}

Result<void> AudioModule::Connect(IFactory& factory) {
  GetOrCreateSystem();
  context_ = factory.Get<IDriverContext>();
  needs_ = &factory.Get<IAppInfo>()->GetRequirements();
  settings_ = factory.Get<ISettings>();
  logger_ = factory.Get<ILogger>();
  factory_ = &factory;
  REX_TRY_VOID(logger_->AddCategories(log::kAudioLog));
  if (auto registered = settings_->Register(system_->GetSettings()); !registered) {
    logger_->RemoveCategories(log::kAudioLog);
    return registered;
  }
  return Ok();
}

void AudioModule::Disconnect() {
  settings_->Unregister(system_->GetSettings());
  logger_->RemoveCategories(log::kAudioLog);
  retired_ = true;
  settings_ = nullptr;
  logger_ = nullptr;
  needs_ = nullptr;
  context_ = nullptr;
  factory_ = nullptr;
}

Result<void> AudioModule::Init() {
  auto selected =
      SelectDrivers(*factory_, DriverType::kAudio, system_->GetSettings().driver.Get(), *context_);
  if (selected) {
    drivers_ = std::move(*selected);
  } else {
    REXAPU_WARN("audio driver: {}", selected.error().message);
  }
  if (drivers_.empty() && needs_->audio) {
    return Err(ErrorCategory::kConfig, "no audio driver is available");
  }
  rex::apu::IAudioDriver* driver =
      drivers_.empty() ? nullptr : static_cast<rex::apu::IAudioDriver*>(drivers_.front().get());
  if (auto started = system_->Start(driver); !started) {
    Shutdown();
    return started;
  }
  return Ok();
}

void AudioModule::Shutdown() {
  if (system_) {
    system_->Stop();
  }
  for (auto it = drivers_.rbegin(); it != drivers_.rend(); ++it) {
    (*it)->Shutdown();
  }
  drivers_.clear();
}

IInterface* AudioModule::QueryInterface(std::string_view name, u32 version) {
  return FindModuleInterface<rex::apu::IAudioSystem>(GetOrCreateSystem(), name, version);
}

}  // namespace rex::apu
