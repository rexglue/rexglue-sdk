/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <chrono>

#include <rex/logging.h>
#include <rex/system/xevent.h>

namespace rex::system {

EventObject::EventObject(bool manual_reset_in, bool initial_state) : manual_reset(manual_reset_in) {
  event = manual_reset ? rex::platform::Event::CreateManualResetEvent(initial_state)
                       : rex::platform::Event::CreateAutoResetEvent(initial_state);
  assert_not_null(event);
}

EventObject::EventObject(const X_DISPATCH_HEADER& header) {
  switch (header.type) {
    case 0x00:
      manual_reset = true;
      break;
    case 0x01:
      manual_reset = false;
      break;
    default:
      assert_always();
      return;
  }
  const bool initial_state = header.signal_state ? true : false;
  event = manual_reset ? rex::platform::Event::CreateManualResetEvent(initial_state)
                       : rex::platform::Event::CreateAutoResetEvent(initial_state);
  assert_not_null(event);
}

void EventQuery(EventObject& event, u32* out_type, u32* out_state) {
  if (out_type) {
    *out_type = event.manual_reset ? 0x00 : 0x01;
  }
  if (out_state) {
    auto result = rex::platform::Wait(event.event.get(), false, std::chrono::milliseconds(0));
    if (result == rex::platform::WaitResult::kSuccess) {
      *out_state = 1;
      event.event->Set();
    } else {
      *out_state = 0;
    }
  }
}

i32 EventSet(EventObject& event) {
  event.event->Set();
  return 1;
}

i32 EventPulse(EventObject& event) {
  event.event->Pulse();
  return 1;
}

i32 EventReset(EventObject& event) {
  event.event->Reset();
  return 1;
}

}  // namespace rex::system
