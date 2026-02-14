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

#include <rex/dbg.h>
#include <rex/filesystem.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/trace_dump.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/string.h>
#include <rex/system/kernel_state.h>
#include <rex/thread.h>
#include <rex/ui/file_picker.h>
#include <rex/ui/presenter.h>
#include <rex/ui/window.h>

#include <stb_image_write.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#undef _CRT_SECURE_NO_WARNINGS
#undef _CRT_NONSTDC_NO_DEPRECATE
#include <stb_image_write.h"

DEFINE_path(target_trace_file, "", "Specifies the trace file to load.", "GPU");
DEFINE_path(trace_dump_path, "", "Output path for dumped files.", "GPU");

namespace rex::graphics {

using namespace rex::graphics::xenos;

TraceDump::TraceDump() = default;

TraceDump::~TraceDump() = default;

int TraceDump::Main(const std::vector<std::string>& args) {
  // Grab path from the flag or unnamed argument.
  std::filesystem::path path;
  std::filesystem::path output_path;
  if (!REXCVAR_GET(target_trace_file).empty()) {
    // Passed as a named argument.
    // TODO(benvanik): find something better than gflags that supports
    // unicode.
    path = REXCVAR_GET(target_trace_file);
  } else if (args.size() >= 2) {
    // Passed as an unnamed argument.
    path = rex::to_path(args[1]);

    if (args.size() >= 3) {
      output_path = rex::to_path(args[2]);
    }
  }

  if (path.empty()) {
    REXGPU_ERROR("No trace file specified");
    return 5;
  }

  // Normalize the path and make absolute.
  auto abs_path = std::filesystem::absolute(path);
  REXGPU_INFO("Loading trace file {}...", rex::path_to_utf8(abs_path));

  if (!Setup()) {
    REXGPU_ERROR("Unable to setup trace dump tool");
    return 4;
  }
  if (!Load(std::move(abs_path))) {
    REXGPU_ERROR("Unable to load trace file; not found?");
    return 5;
  }

  // Root file name for outputs.
  if (output_path.empty()) {
    base_output_path_ = REXCVAR_GET(trace_dump_path);
    auto output_name = path.filename().replace_extension();

    base_output_path_ = base_output_path_ / output_name;
  } else {
    base_output_path_ = output_path;
  }

  // Ensure output path exists.
  rex::filesystem::CreateParentFolder(base_output_path_);

  return Run();
}

bool TraceDump::Setup() {
  // Create the emulator but don't initialize so we can setup the window.
  emulator_ = std::make_unique<Emulator>("", "", "", "");
  X_STATUS result = emulator_->Setup(
      nullptr, nullptr, false, nullptr, [this]() { return CreateGraphicsSystem(); }, nullptr);
  if (XFAILED(result)) {
    REXGPU_ERROR("Failed to setup emulator: {:08X}", result);
    return false;
  }
  graphics_system_ = emulator_->graphics_system();
  player_ = std::make_unique<TracePlayer>(graphics_system_);
  return true;
}

bool TraceDump::Load(const std::filesystem::path& trace_file_path) {
  trace_file_path_ = trace_file_path;

  if (!player_->Open(rex::path_to_utf8(trace_file_path_))) {
    REXGPU_ERROR("Could not load trace file");
    return false;
  }

  return true;
}

int TraceDump::Run() {
  BeginHostCapture();
  player_->SeekFrame(0);
  player_->SeekCommand(static_cast<int>(player_->current_frame()->commands.size() - 1));
  player_->WaitOnPlayback();
  EndHostCapture();

  // Capture.
  int result = 0;
  ui::Presenter* presenter = graphics_system_->presenter();
  ui::RawImage raw_image;
  if (presenter && presenter->CaptureGuestOutput(raw_image)) {
    // Save framebuffer png.
    auto png_path = base_output_path_.replace_extension(".png");
    auto handle = filesystem::OpenFile(png_path, "wb");
    auto callback = [](void* context, void* data, int size) {
      fwrite(data, 1, size, (FILE*)context);
    };
    stbi_write_png_to_func(callback, handle, static_cast<int>(raw_image.width),
                           static_cast<int>(raw_image.height), 4, raw_image.data.data(),
                           static_cast<int>(raw_image.stride));
    fclose(handle);
  } else {
    result = 1;
  }

  player_.reset();
  emulator_.reset();
  return result;
}

}  // namespace rex::graphics
