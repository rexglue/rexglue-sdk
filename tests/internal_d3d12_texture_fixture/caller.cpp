#include <rex/graphics/draw_capture_mailbox.h>
namespace test_fixture {
void ObserveDraw(const rex::graphics::diagnostic::DrawCaptureFrameInputDrawView&);
void ObserveCopy(const rex::graphics::diagnostic::DrawCaptureFrameInputCopyView&);
void ObserveEnd(uint64_t frame, uint64_t submission, bool succeeded);
}  // namespace test_fixture
#define REX_INTERNAL_D3D12_TEXTURE_FIXTURE_CALLER 1
#define wmain UnusedInstalledControlMain
#include "../installed_prepared_control/control.cpp"
#undef wmain
#include "fixture.h"
#include <dxgiformat.h>
#include "capture_io.h"
#include "caller.inl"
