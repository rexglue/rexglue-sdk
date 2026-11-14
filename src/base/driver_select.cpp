/**
 * @file        base/driver_select.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/driver.h>
#include <rex/driver_context.h>
#include <rex/driver_registration.h>
#include <rex/driver_module.h>

#include <algorithm>
#include <format>
#include <ranges>
#include <string>

#include <rex/string/text.h>

namespace rex {

namespace {

std::vector<std::string> SplitNames(std::string_view names) {
  std::vector<std::string> wanted;
  for (const auto part : std::views::split(names, ',')) {
    const std::string_view name = string::Trim(std::string_view(part.begin(), part.end()));
    if (name.empty() || name == "none" ||
        std::ranges::find(wanted, std::string(name)) != wanted.end()) {
      continue;
    }
    wanted.emplace_back(name);
  }
  return wanted;
}

std::string GetModuleName(DriverType type, std::string_view name, GraphicsFamily family) {
  switch (type) {
    case DriverType::kAudio:
      return std::format("rexapu-{}", name);
    case DriverType::kInput:
      return std::format("rexhid-{}", name);
    case DriverType::kUI:
      return std::format("rexui-{}", name);
    case DriverType::kGraphics:
      return family == GraphicsFamily::kNone
                 ? std::string()
                 : std::format("rexgpu-{}-{}", GetGraphicsFamilyName(family), name);
  }
  return {};
}

const DriverRegistration* FindRegistration(IInterfaceFactory& factory, DriverType type,
                                           std::string_view name) {
  for (IDriverModule* module : factory.FindAll<IDriverModule>()) {
    for (const DriverRegistration& registration : module->GetDrivers()) {
      if (registration.type == type && registration.name == name) {
        return &registration;
      }
    }
  }
  return nullptr;
}

void ShutdownAll(std::vector<std::unique_ptr<IDriver>>& drivers) {
  for (auto it = drivers.rbegin(); it != drivers.rend(); ++it) {
    (*it)->Shutdown();
  }
  drivers.clear();
}

}  // namespace

std::string_view GetGraphicsFamilyName(GraphicsFamily family) {
  switch (family) {
    case GraphicsFamily::kNone:
      return {};
    case GraphicsFamily::kXenos:
      return "xenos";
  }
  return {};
}

std::vector<std::string> GetDriverModules(DriverType type, std::string_view names,
                                          GraphicsFamily family, IInterfaceFactory* connected) {
  std::vector<std::string> modules;
  for (const std::string& name : SplitNames(names)) {
    if (connected && FindRegistration(*connected, type, name)) {
      continue;
    }
    std::string module = GetModuleName(type, name, family);
    if (!module.empty()) {
      modules.push_back(std::move(module));
    }
  }
  return modules;
}

Result<std::vector<std::unique_ptr<IDriver>>> SelectDrivers(IInterfaceFactory& factory,
                                                            DriverType type, std::string_view names,
                                                            IDriverContext& context) {
  std::vector<std::unique_ptr<IDriver>> drivers;
  for (const std::string& name : SplitNames(names)) {
    const DriverRegistration* registration = FindRegistration(factory, type, name);
    if (!registration) {
      ShutdownAll(drivers);
      return Err<std::vector<std::unique_ptr<IDriver>>>(
          ErrorCategory::kNotFound,
          std::format("driver '{}': no connected module registrations '{}'", name, name));
    }
    std::unique_ptr<IDriver> driver = registration->create();
    if (auto started = driver->Initialize(context); !started) {
      ShutdownAll(drivers);
      return Err<std::vector<std::unique_ptr<IDriver>>>(
          started.error().category,
          std::format("driver '{}' failed to initialize: {}", name, started.error().message),
          started.error().code);
    }
    drivers.push_back(std::move(driver));
  }
  return drivers;
}

}  // namespace rex
