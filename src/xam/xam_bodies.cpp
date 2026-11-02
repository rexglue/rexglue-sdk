#include "xam/xam_bodies.h"

#include <vector>

namespace rex::xam {

namespace {

std::vector<XAMBody>& GetRegistered() {
  static std::vector<XAMBody> bodies;
  return bodies;
}

}  // namespace

std::span<const XAMBody> XAMExports::GetBodies() const {
  return GetRegistered();
}

XAMBodyRegistrar::XAMBodyRegistrar(std::string_view name, PPCFunc* function) {
  GetRegistered().push_back(XAMBody{name, function});
}

}  // namespace rex::xam
