#include "gpu/xenos/host/surface.h"

#include <rex/platform.h>

#if REX_PLATFORM_WIN32
#include <windows.h>
#endif

namespace rex::gpu {

NativeSurface::NativeSurface(const NativeWindow& window) : window_(window) {}

Surface::TypeIndex NativeSurface::GetType() const {
  switch (window_.type) {
    case NativeWindow::Type::kWayland:
      return kTypeIndexWaylandSurface;
    case NativeWindow::Type::kX11:
      return kTypeIndexXCBWindow;
    case NativeWindow::Type::kCocoa:
      return kTypeIndexCAMetalLayer;
    default:
      return kTypeIndexWin32HWND;
  }
}

void* NativeSurface::GetHINSTANCE() const {
#if REX_PLATFORM_WIN32
  return ::GetModuleHandleW(nullptr);
#else
  return nullptr;
#endif
}

}  // namespace rex::gpu
