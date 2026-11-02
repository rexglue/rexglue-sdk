#include "kernel/xam/forward.h"

#include <format>
#include <unordered_map>
#include <unordered_set>

#include <rex/logging/log.h>

#include "kernel/categories.h"

namespace rex::kernel {

namespace {

XAMForward* g_head = nullptr;

}  // namespace

XAMForward::XAMForward(std::string_view name) : name_(name), next_(g_head) {
  g_head = this;
}

void XAMForward::ReportMissing() const {
  REXLOG_ERROR(kernel, "{} was called and the rexxam module has no body for it", name_);
}

Result<void> BindXAMForwards(std::span<const xam::XAMBody> bodies) {
  std::unordered_map<std::string_view, PPCFunc*> by_name;
  for (const xam::XAMBody& body : bodies) {
    if (!by_name.emplace(body.name, body.function).second) {
      return Err(ErrorCategory::kModule, std::format("rexxam has two bodies for {}", body.name));
    }
  }
  std::unordered_set<std::string_view> forwarded;
  for (XAMForward* forward = g_head; forward; forward = forward->next_) {
    if (!by_name.contains(forward->name_)) {
      return Err(ErrorCategory::kModule,
                 std::format("rexxam has no body for the export {}", forward->name_));
    }
    forwarded.insert(forward->name_);
  }
  for (const xam::XAMBody& body : bodies) {
    if (!forwarded.contains(body.name)) {
      return Err(
          ErrorCategory::kModule,
          std::format("rexxam has a body for {} and the kernel forwards nothing to it", body.name));
    }
  }
  for (XAMForward* forward = g_head; forward; forward = forward->next_) {
    forward->function_.store(by_name.at(forward->name_), std::memory_order_release);
  }
  return {};
}

void UnbindXAMForwards() {
  for (XAMForward* forward = g_head; forward; forward = forward->next_) {
    forward->function_.store(nullptr, std::memory_order_release);
  }
}

}  // namespace rex::kernel
