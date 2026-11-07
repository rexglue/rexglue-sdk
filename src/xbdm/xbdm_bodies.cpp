#include "xbdm/xbdm_bodies.h"

#include <vector>

namespace rex::xbdm {

namespace {

std::vector<XBDMBody>& GetRegistered() {
  static std::vector<XBDMBody> bodies;
  return bodies;
}

}  // namespace

std::span<const XBDMBody> XBDMExports::GetBodies() const {
  return GetRegistered();
}

XBDMBodyRegistrar::XBDMBodyRegistrar(std::string_view name, PPCFunc* function) {
  GetRegistered().push_back(XBDMBody{name, function});
}

}  // namespace rex::xbdm
