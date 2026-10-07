#pragma once

#include <rex/ui/overlay.h>
#include <rex/ui/user_interface.h>

namespace rex {
class Runtime;
namespace log {
class CaptureSink;
}  // namespace log
}  // namespace rex

namespace rex::ui {

class ConsoleHost;
class ConsoleModel;

struct ModuleOverlayContext {
  OverlayContext base;
  ConsoleModel& console;
  ConsoleHost& console_host;
  log::CaptureSink& log_capture;
  Runtime*& runtime;
  FrameStatsProvider& frame_stats;
};

}  // namespace rex::ui
