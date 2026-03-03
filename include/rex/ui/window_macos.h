/**
 ******************************************************************************
 * ReXGlue SDK — macOS Window (Cocoa / AppKit)                                *
 ******************************************************************************
 * Phase 2: Native NSWindow / NSView windowing, replacing GTK on macOS.       *
 ******************************************************************************
 */

#pragma once

#include <memory>
#include <string>

#include <rex/platform.h>
#include <rex/ui/menu_item.h>
#include <rex/ui/window.h>

// Forward-declare Objective-C types for plain C++ translation units.
#ifdef __OBJC__
@class NSWindow;
@class NSView;
@class CAMetalLayer;
@class RexWindowDelegate;
@class RexMetalView;
#else
typedef void NSWindow;
typedef void NSView;
typedef void CAMetalLayer;
typedef void RexWindowDelegate;
typedef void RexMetalView;
#endif

namespace rex {
namespace ui {

class MacOSWindow : public Window {
  using super = Window;

 public:
  MacOSWindow(WindowedAppContext& app_context, const std::string_view title,
              uint32_t desired_logical_width, uint32_t desired_logical_height);
  ~MacOSWindow() override;

  NSWindow* ns_window() const { return ns_window_; }

  uint32_t GetMediumDpi() const override { return 72; }  // macOS points are 72 ppi

 protected:
  uint32_t GetLatestDpiImpl() const override;

  bool OpenImpl() override;
  void RequestCloseImpl() override;

  void ApplyNewFullscreen() override;
  void ApplyNewTitle() override;
  void FocusImpl() override;

  std::unique_ptr<Surface> CreateSurfaceImpl(
      Surface::TypeFlags allowed_types) override;
  void RequestPaintImpl() override;

 public:
  // Called by the Objective-C delegate.
  void OnNativeResize();
  void OnNativeFocusChange(bool focused);
  void OnNativeCloseRequest();

 private:
  friend class MacOSWindowBridge;

  NSWindow* ns_window_ = nullptr;
  RexMetalView* metal_view_ = nullptr;
  RexWindowDelegate* window_delegate_ = nullptr;
};

}  // namespace ui
}  // namespace rex
