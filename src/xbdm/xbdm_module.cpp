#include "xbdm/xbdm_module.h"

namespace rex::xbdm {

std::string_view XBDMModule::GetName() const {
  return "rexxbdm";
}

Result<void> XBDMModule::Connect(IInterfaceFactory&) {
  return {};
}

void XBDMModule::Disconnect() {}

Result<void> XBDMModule::Init() {
  return {};
}

void XBDMModule::Shutdown() {}

IInterface* XBDMModule::QueryInterface(std::string_view name, u32 version) {
  if (name == IXBDMExports::kInterface.name && version == IXBDMExports::kInterface.version) {
    return static_cast<IXBDMExports*>(&exports_);
  }
  return IModule::QueryInterface(name, version);
}

}  // namespace rex::xbdm
