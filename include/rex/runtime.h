/**
 * @file        runtime.h
 * @brief       Runtime subsystem entry point
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 * @remarks     Based on Xenia emulator.h/cc
 */

#pragma once

#include <filesystem>
#include <memory>

#include <rex/runtime/export_resolver.h>
#include <rex/kernel/xobject.h>  // object_ref
#include <rex/memory.h>
#include <rex/filesystem/vfs.h>
#include <rex/kernel.h>

// Forward declaration for function mapping (defined in rex/runtime/guest/context.h)
struct PPCFuncMapping;

namespace rex {

// Forward declarations
namespace runtime {
class Processor;
class ExportResolver;
}  // namespace runtime
namespace graphics {
class GraphicsSystem;
}  // namespace graphics
namespace audio {
class AudioSystem;
}  // namespace audio
namespace kernel {
class KernelState;
class XThread;
}  // namespace kernel
namespace ui {
class WindowedAppContext;
class Window;
class ImGuiDrawer;
}  // namespace ui

/**
 * Runtime class - the main entry point for recompiled applications.
 *
 * Owns all subsystems:
 * - Memory: Virtual address space for guest code
 * - VFS: Virtual file system
 * - KernelState: Kernel objects, threading, etc.
 */
class Runtime {
 public:
  explicit Runtime(const std::filesystem::path& game_data_root,
                   const std::filesystem::path& user_data_root = {},
                   const std::filesystem::path& update_data_root = {});
  ~Runtime();

  // Non-copyable
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;

  // Global instance accessor - set after Setup() is called
  static Runtime* instance() { return instance_; }

  // Subsystem accessors
  memory::Memory* memory() const { return memory_.get(); }
  rex::filesystem::VirtualFileSystem* file_system() const { return file_system_.get(); }
  kernel::KernelState* kernel_state() const { return kernel_state_.get(); }
  graphics::GraphicsSystem* graphics_system() const { return graphics_system_.get(); }
  audio::AudioSystem* audio_system() const { return audio_system_.get(); }

  // Processor for IRQL and interrupt synchronization
  runtime::Processor* processor() const { return processor_.get(); }
  // Export resolver - used for variable import resolution in guest memory
  runtime::ExportResolver* export_resolver() const { return export_resolver_.get(); }

  // Path accessors
  const std::filesystem::path& game_data_root() const { return game_data_root_; }
  const std::filesystem::path& user_data_root() const { return user_data_root_; }
  const std::filesystem::path& update_data_root() const { return update_data_root_; }

  // Set the app context for presentation (call before Setup)
  void set_app_context(ui::WindowedAppContext* context) { app_context_ = context; }
  ui::WindowedAppContext* app_context() const { return app_context_; }

  // UI accessors for dialog system
  void set_display_window(ui::Window* window) { display_window_ = window; }
  ui::Window* display_window() const { return display_window_; }
  void set_imgui_drawer(ui::ImGuiDrawer* drawer) { imgui_drawer_ = drawer; }
  ui::ImGuiDrawer* imgui_drawer() const { return imgui_drawer_; }

  // Setup the runtime environment
  // tool_mode: If true, skips all but core initialization (for analysis tools like codegen)
  X_STATUS Setup(bool tool_mode = false);

  // rexglue - initializes function dispatch table
  // func_mappings: null-terminated array of {guest_addr, host_func} pairs
  X_STATUS Setup(uint32_t code_base, uint32_t code_size,
                 uint32_t image_base, uint32_t image_size,
                 const PPCFuncMapping* func_mappings);

  // Check if running in tool mode (no GPU)
  bool is_tool_mode() const { return tool_mode_; }

  void Shutdown();

  // Load XEX image into guest memory
  X_STATUS LoadXexImage(const std::string_view module_path);

  // Launch XEX module and return main thread
  // Call after LoadXexImage to start execution
  kernel::object_ref<kernel::XThread> LaunchModule();

  // Access the memory base pointer for recompiled code
  uint8_t* virtual_membase() const;

 private:
  // Set up VFS: mounts game_data_root as game:/d:, update_data_root as update:
  bool SetupVfs();

  std::filesystem::path game_data_root_;
  std::filesystem::path user_data_root_;
  std::filesystem::path update_data_root_;

  ui::WindowedAppContext* app_context_ = nullptr;
  ui::Window* display_window_ = nullptr;
  ui::ImGuiDrawer* imgui_drawer_ = nullptr;
  bool tool_mode_ = false;

  std::unique_ptr<memory::Memory> memory_;
  std::unique_ptr<runtime::Processor> processor_;
  std::unique_ptr<rex::filesystem::VirtualFileSystem> file_system_;
  std::unique_ptr<kernel::KernelState> kernel_state_;
  std::unique_ptr<graphics::GraphicsSystem> graphics_system_;
  std::unique_ptr<audio::AudioSystem> audio_system_;
  std::unique_ptr<runtime::ExportResolver> export_resolver_;

  static Runtime* instance_;
};

}  // namespace rex
