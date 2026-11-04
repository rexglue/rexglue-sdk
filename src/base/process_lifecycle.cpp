#include <atomic>

#include <rex/module.h>

#include "base/app_state.h"

#if REX_PLATFORM_WIN32
#include <windows.h>
#include <timeapi.h>
#endif

namespace rex {
namespace {

std::atomic<IInterfaceFactory*> g_api_factory{nullptr};

}  // namespace

namespace detail {

#if REX_PLATFORM_WIN32
void BeginTimerResolution() {
  timeBeginPeriod(1);
}

void EndTimerResolution() {
  timeEndPeriod(1);
}
#else
void BeginTimerResolution() {}

void EndTimerResolution() {}
#endif

IInterfaceFactory* GetAppInterfaceFactory() {
  return g_api_factory.load();
}

void SetAPIFactory(IInterfaceFactory* factory) {
  g_api_factory.store(factory);
}

}  // namespace detail
}  // namespace rex
