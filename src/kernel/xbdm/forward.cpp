#include "kernel/xbdm/forward.h"

#include <format>
#include <unordered_map>
#include <unordered_set>

namespace rex::kernel {

namespace {

XBDMForward* g_head = nullptr;

}  // namespace

XBDMForward::XBDMForward(std::string_view name, PPCFunc* fallback)
    : name_(name), fallback_(fallback), next_(g_head) {
  g_head = this;
}

Result<void> BindXBDMForwards(std::span<const ::rex::xbdm::XBDMBody> bodies) {
  std::unordered_map<std::string_view, PPCFunc*> by_name;
  for (const ::rex::xbdm::XBDMBody& body : bodies) {
    if (!by_name.emplace(body.name, body.function).second) {
      return Err(ErrorCategory::kModule, std::format("rexxbdm has two bodies for {}", body.name));
    }
  }
  std::unordered_set<std::string_view> forwarded;
  for (XBDMForward* forward = g_head; forward; forward = forward->next_) {
    if (!by_name.contains(forward->name_)) {
      return Err(ErrorCategory::kModule,
                 std::format("rexxbdm has no body for the export {}", forward->name_));
    }
    forwarded.insert(forward->name_);
  }
  for (const ::rex::xbdm::XBDMBody& body : bodies) {
    if (!forwarded.contains(body.name)) {
      return Err(ErrorCategory::kModule,
                 std::format("rexxbdm has a body for {} and the kernel forwards nothing to it",
                             body.name));
    }
  }
  for (XBDMForward* forward = g_head; forward; forward = forward->next_) {
    forward->function_.store(by_name.at(forward->name_), std::memory_order_release);
  }
  return {};
}

void UnbindXBDMForwards() {
  for (XBDMForward* forward = g_head; forward; forward = forward->next_) {
    forward->function_.store(nullptr, std::memory_order_release);
  }
}

}  // namespace rex::kernel
