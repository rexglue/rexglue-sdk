/**
 ******************************************************************************
 * ReXGlue SDK — macOS Windowed App Context Implementation                    *
 ******************************************************************************
 * Phase 2: NSApplication run-loop integration.                               *
 ******************************************************************************
 */

#import <Cocoa/Cocoa.h>

#include <rex/ui/windowed_app_context_macos.h>

namespace rex {
namespace ui {

MacOSWindowedAppContext::~MacOSWindowedAppContext() {
  // Ensure pending functions are drained before the context is destroyed.
  ExecutePendingFunctionsFromUIThread();
}

void MacOSWindowedAppContext::NotifyUILoopOfPendingFunctions() {
  // Post a do-nothing event to wake the run loop so it will execute our
  // pending functions at the next opportunity.  CFRunLoopWakeUp alone isn't
  // sufficient because -[NSApp run] may be blocked in the modal-event path.
  @autoreleasepool {
    NSEvent* wake_event =
        [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                           location:NSMakePoint(0, 0)
                      modifierFlags:0
                          timestamp:0
                       windowNumber:0
                            context:nil
                            subtype:0
                              data1:0
                              data2:0];
    [NSApp postEvent:wake_event atStart:YES];
  }
}

void MacOSWindowedAppContext::PlatformQuitFromUIThread() {
  [NSApp stop:nil];
  // -[NSApp stop:] sets a flag that causes -[NSApp run] to return after the
  // *current* event has been dispatched.  Post a dummy event so that return
  // happens immediately if the run loop is currently idle.
  NotifyUILoopOfPendingFunctions();
}

void MacOSWindowedAppContext::RunLoop() {
  // Safety: if quit was requested before we got here, bail out immediately.
  if (HasQuitFromUIThread()) {
    return;
  }

  @autoreleasepool {
    // Drain pending functions once before entering the loop so anything
    // queued during OnInitialize is picked up.
    ExecutePendingFunctionsFromUIThread();

    // Standard NSApplication run loop.
    // -[NSApp run] internally calls -[NSApp nextEventMatchingMask:…] and
    // -[NSApp sendEvent:] in a loop until -[NSApp stop:] is called.
    [NSApp run];
  }

  // Something else might have stopped the app (e.g. Cmd-Q).
  // Make the context aware.
  QuitFromUIThread();
}

}  // namespace ui
}  // namespace rex
