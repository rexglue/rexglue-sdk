#include "xbdm/xbdm_bodies.h"

#include <vector>

namespace rex::xbdm {

namespace {

XBDMBodyRegistrar* g_first = nullptr;
XBDMBodyRegistrar* g_last = nullptr;

}  // namespace

std::span<const XBDMBody> XBDMExports::GetBodies() const {
  static const std::vector<XBDMBody> kBodies = [] {
    std::vector<XBDMBody> collected;
    for (const XBDMBodyRegistrar* registrar = g_first; registrar; registrar = registrar->next_) {
      collected.push_back(registrar->body_);
    }
    return collected;
  }();
  return kBodies;
}

XBDMBodyRegistrar::XBDMBodyRegistrar(std::string_view name, PPCFunc* function) noexcept
    : body_{name, function} {
  (g_last ? g_last->next_ : g_first) = this;
  g_last = this;
}

}  // namespace rex::xbdm
