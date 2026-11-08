#include "xam/xam_bodies.h"

#include <vector>

namespace rex::xam {

namespace {

XAMBodyRegistrar* g_first = nullptr;
XAMBodyRegistrar* g_last = nullptr;

}  // namespace

std::span<const XAMBody> XAMExports::GetBodies() const {
  static const std::vector<XAMBody> bodies = [] {
    std::vector<XAMBody> collected;
    for (const XAMBodyRegistrar* registrar = g_first; registrar; registrar = registrar->next_) {
      collected.push_back(registrar->body_);
    }
    return collected;
  }();
  return bodies;
}

XAMBodyRegistrar::XAMBodyRegistrar(std::string_view name, PPCFunc* function) noexcept
    : body_{name, function} {
  (g_last ? g_last->next_ : g_first) = this;
  g_last = this;
}

}  // namespace rex::xam
