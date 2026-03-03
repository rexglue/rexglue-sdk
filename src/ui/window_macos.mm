/**
 ******************************************************************************
 * ReXGlue SDK — macOS Window Implementation                                  *
 ******************************************************************************
 * Phase 2: Cocoa NSWindow + CAMetalLayer-backed NSView for Vulkan/MoltenVK. *
 ******************************************************************************
 */

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>

#include <rex/logging.h>
#include <rex/ui/surface_macos.h>
#include <rex/ui/window_macos.h>

// ---------------------------------------------------------------------------
// Objective-C helper: NSView subclass whose layer is a CAMetalLayer.
// ---------------------------------------------------------------------------
@interface RexMetalView : NSView
@property(nonatomic, readonly) CAMetalLayer* metalLayer;
@end

@implementation RexMetalView

- (instancetype)initWithFrame:(NSRect)frameRect {
  self = [super initWithFrame:frameRect];
  if (self) {
    self.wantsLayer = YES;
    self.layerContentsRedrawPolicy =
        NSViewLayerContentsRedrawDuringViewResize;
  }
  return self;
}

- (CALayer*)makeBackingLayer {
  CAMetalLayer* layer = [CAMetalLayer layer];
  // Let the compositor handle pixel format; MoltenVK / Vulkan will set the
  // actual pixel format on the layer through VK_EXT_metal_surface.
  layer.contentsScale = self.window.backingScaleFactor;
  return layer;
}

- (CAMetalLayer*)metalLayer {
  return (CAMetalLayer*)self.layer;
}

- (BOOL)acceptsFirstResponder {
  return YES;
}

// Keep the Metal layer's drawable size in sync with the view's pixel size.
- (void)setFrameSize:(NSSize)newSize {
  [super setFrameSize:newSize];
  CGSize drawableSize =
      [self convertSizeToBacking:self.bounds.size];
  self.metalLayer.drawableSize = drawableSize;
}

- (void)viewDidChangeBackingProperties {
  [super viewDidChangeBackingProperties];
  self.metalLayer.contentsScale = self.window.backingScaleFactor;
  CGSize drawableSize =
      [self convertSizeToBacking:self.bounds.size];
  self.metalLayer.drawableSize = drawableSize;
}

@end

// ---------------------------------------------------------------------------
// Objective-C helper: NSWindowDelegate that bridges events to C++.
// ---------------------------------------------------------------------------
@interface RexWindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic, assign) rex::ui::MacOSWindow* cppWindow;
@end

@implementation RexWindowDelegate

- (void)windowDidResize:(NSNotification*)notification {
  if (_cppWindow) {
    _cppWindow->OnNativeResize();
  }
}

- (void)windowDidBecomeKey:(NSNotification*)notification {
  if (_cppWindow) {
    _cppWindow->OnNativeFocusChange(true);
  }
}

- (void)windowDidResignKey:(NSNotification*)notification {
  if (_cppWindow) {
    _cppWindow->OnNativeFocusChange(false);
  }
}

- (BOOL)windowShouldClose:(NSWindow*)sender {
  if (_cppWindow) {
    _cppWindow->OnNativeCloseRequest();
  }
  // We handle closing ourselves via RequestClose flow.
  return NO;
}

- (void)windowWillClose:(NSNotification*)notification {
  // Prevent the delegate from dangling.
  _cppWindow = nullptr;
}

@end

// ---------------------------------------------------------------------------
// C++ implementation of MacOSWindow.
// ---------------------------------------------------------------------------

namespace rex {
namespace ui {

// Static factory — Window::Create routes to the platform implementation.
std::unique_ptr<Window> Window::Create(WindowedAppContext& app_context,
                                       const std::string_view title,
                                       uint32_t desired_logical_width,
                                       uint32_t desired_logical_height) {
  return std::make_unique<MacOSWindow>(app_context, title,
                                       desired_logical_width,
                                       desired_logical_height);
}

MacOSWindow::MacOSWindow(WindowedAppContext& app_context,
                         const std::string_view title,
                         uint32_t desired_logical_width,
                         uint32_t desired_logical_height)
    : Window(app_context, title, desired_logical_width,
             desired_logical_height) {}

MacOSWindow::~MacOSWindow() {
  EnterDestructor();
  if (ns_window_) {
    // Detach the delegate so it doesn't call back into freed memory.
    window_delegate_.cppWindow = nullptr;
    [ns_window_ close];
    ns_window_ = nil;
    window_delegate_ = nil;
    metal_view_ = nil;
  }
}

uint32_t MacOSWindow::GetLatestDpiImpl() const {
  if (ns_window_) {
    return static_cast<uint32_t>(72.0 * ns_window_.backingScaleFactor);
  }
  return 72;
}

bool MacOSWindow::OpenImpl() {
  @autoreleasepool {
    uint32_t physical_width = SizeToPhysical(GetDesiredLogicalWidth());
    uint32_t physical_height = SizeToPhysical(GetDesiredLogicalHeight());

    NSRect content_rect = NSMakeRect(0, 0, physical_width, physical_height);
    NSUInteger style_mask = NSWindowStyleMaskTitled |
                            NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable |
                            NSWindowStyleMaskResizable;

    NSWindow* window =
        [[NSWindow alloc] initWithContentRect:content_rect
                                    styleMask:style_mask
                                      backing:NSBackingStoreBuffered
                                        defer:NO];
    [window setTitle:[NSString stringWithUTF8String:GetTitle().c_str()]];
    [window center];
    [window setReleasedWhenClosed:NO];

    // Create the Metal-layer-backed view.
    RexMetalView* metal_view =
        [[RexMetalView alloc] initWithFrame:content_rect];
    [window setContentView:metal_view];

    // Set the delegate for window events.
    RexWindowDelegate* delegate = [[RexWindowDelegate alloc] init];
    delegate.cppWindow = this;
    [window setDelegate:delegate];

    // ARC retains these automatically via strong member pointers.
    ns_window_ = window;
    metal_view_ = metal_view;
    window_delegate_ = delegate;

    // Show the window.
    [window makeKeyAndOrderFront:nil];

    // Update actual size.
    WindowDestructionReceiver destruction_receiver(this);
    NSSize backing = [metal_view convertSizeToBacking:metal_view.bounds.size];
    OnActualSizeUpdate(static_cast<uint32_t>(backing.width),
                       static_cast<uint32_t>(backing.height),
                       destruction_receiver);
    if (destruction_receiver.IsWindowDestroyed()) {
      return true;
    }

    // Report focus (window was just shown, so it has focus).
    OnFocusUpdate(true, destruction_receiver);
    if (destruction_receiver.IsWindowDestroyed()) {
      return true;
    }

    // Enter fullscreen if that's the desired state.
    if (IsFullscreen()) {
      [window toggleFullScreen:nil];
    }
  }

  return true;
}

void MacOSWindow::RequestCloseImpl() {
  if (ns_window_) {
    // Trigger the standard close flow which calls windowShouldClose → our
    // OnNativeCloseRequest → OnBeforeClose.
    WindowDestructionReceiver destruction_receiver(this);
    OnBeforeClose(destruction_receiver);
    if (destruction_receiver.IsWindowDestroyed()) {
      return;
    }
    if (ns_window_) {
      // Detach delegate before closing.
      window_delegate_.cppWindow = nullptr;
      [ns_window_ close];
      ns_window_ = nil;
      window_delegate_ = nil;
      metal_view_ = nil;
    }
    OnAfterClose();
  }
}

void MacOSWindow::ApplyNewFullscreen() {
  if (ns_window_) {
    bool currently_fullscreen =
        (ns_window_.styleMask & NSWindowStyleMaskFullScreen) != 0;
    if (IsFullscreen() != currently_fullscreen) {
      [ns_window_ toggleFullScreen:nil];
    }
  }
}

void MacOSWindow::ApplyNewTitle() {
  if (ns_window_) {
    [ns_window_ setTitle:[NSString stringWithUTF8String:GetTitle().c_str()]];
  }
}

void MacOSWindow::FocusImpl() {
  if (ns_window_) {
    [ns_window_ makeKeyAndOrderFront:nil];
  }
}

std::unique_ptr<Surface> MacOSWindow::CreateSurfaceImpl(
    Surface::TypeFlags allowed_types) {
  if (!(allowed_types & Surface::kTypeFlag_MacOSMetalLayer)) {
    return nullptr;
  }
  if (!metal_view_) {
    return nullptr;
  }
  return std::make_unique<MacOSMetalSurface>(
      (NSView*)metal_view_, metal_view_.metalLayer);
}

void MacOSWindow::RequestPaintImpl() {
  if (metal_view_) {
    // Mark the view as needing display; the Presenter drives actual
    // rendering via Vulkan command submission, so this is mostly a
    // hint to the compositor.
    [metal_view_ setNeedsDisplay:YES];
  }
  OnPaint();
}

// -- Native callbacks from the Objective-C delegate -------------------------

void MacOSWindow::OnNativeResize() {
  if (!metal_view_) return;
  WindowDestructionReceiver destruction_receiver(this);
  NSSize backing = [metal_view_ convertSizeToBacking:metal_view_.bounds.size];
  OnActualSizeUpdate(static_cast<uint32_t>(backing.width),
                     static_cast<uint32_t>(backing.height),
                     destruction_receiver);
}

void MacOSWindow::OnNativeFocusChange(bool focused) {
  WindowDestructionReceiver destruction_receiver(this);
  OnFocusUpdate(focused, destruction_receiver);
}

void MacOSWindow::OnNativeCloseRequest() {
  RequestClose();
}

}  // namespace ui
}  // namespace rex
