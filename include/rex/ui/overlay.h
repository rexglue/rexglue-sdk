/**
 * @file        ui/overlay.h
 * @brief       Overlay tables and the persistent overlay registry
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rex/result.h>
#include <rex/types.h>
#include <rex/ui/overlay/debug_overlay.h>
#include <rex/ui/window_listener.h>

namespace rex {
class Runtime;
struct Config;
class LogCaptureSink;
}  // namespace rex

namespace rex::cvar {
class ICvarRegistry;
}  // namespace rex::cvar

namespace rex::ui {

class ImGuiDialog;
class ImGuiDrawer;
class ImmediateDrawer;
class Window;

/** What an overlay may use. Valid for the overlay's whole life. */
class OverlayContext {
 public:
  /**
   * Creates a context. TitleApp creates the one every overlay receives.
   *
   * @param drawer ImGui drawer.
   * @param immediate_drawer Slot holding the immediate drawer, which may be empty.
   * @param cvars Cvar registry.
   * @param log_capture Sink that keeps recent log lines.
   * @param config The app's configuration.
   * @param runtime Slot holding the runtime once the title launches.
   * @param frame_stats Slot holding the frame time source.
   */
  OverlayContext(ImGuiDrawer& drawer, const std::unique_ptr<ImmediateDrawer>& immediate_drawer,
                 cvar::ICvarRegistry& cvars, LogCaptureSink& log_capture, const Config& config,
                 const std::unique_ptr<Runtime>& runtime,
                 const DebugOverlayDialog::FrameStatsProvider& frame_stats);

  /**
   * Returns the ImGui drawer.
   *
   * @return The drawer the overlay draws through.
   */
  ImGuiDrawer& drawer() const;

  /**
   * Returns the immediate drawer.
   *
   * @return The drawer for textures and primitives, or nullptr when the app has none.
   */
  ImmediateDrawer* immediate_drawer() const;

  /**
   * Returns the cvar registry.
   *
   * @return The registry every module's cvars live in.
   */
  cvar::ICvarRegistry& cvars() const;

  /**
   * Returns the sink that keeps recent log lines.
   *
   * @return The capture sink App::Init attached to logging.
   */
  LogCaptureSink& log_capture() const;

  /**
   * Returns the configuration.
   *
   * @return The configuration the app initialized with.
   */
  const Config& config() const;

  /**
   * Returns the runtime.
   *
   * @return The runtime, or nullptr before the title launches and after Release.
   */
  Runtime* runtime() const;

  /**
   * Returns the frame time source for the debug overlay.
   *
   * @return The function set through TitleUI::SetFrameStats, or an empty function when none
   *         was set.
   */
  const DebugOverlayDialog::FrameStatsProvider& frame_stats() const;

 private:
  ImGuiDrawer& drawer_;
  const std::unique_ptr<ImmediateDrawer>& immediate_drawer_;
  cvar::ICvarRegistry& cvars_;
  LogCaptureSink& log_capture_;
  const Config& config_;
  const std::unique_ptr<Runtime>& runtime_;
  const DebugOverlayDialog::FrameStatsProvider& frame_stats_;
};

/** 1 overlay table entry. REX_OVERLAY builds it. */
struct OverlayDesc {
  /** Unique overlay name, also the keybind name bind.<name>. */
  std::string_view name;
  /** Default key of the toggle bind. Empty registers no bind. */
  std::string_view default_key;
  /** Constructs the overlay. */
  std::unique_ptr<ImGuiDialog> (*create)(const OverlayContext& context);
};

/** Persistent overlays. Toggling shows and hides, it never destroys. */
class OverlayRegistry : public NonCopyable, private WindowListener {
 public:
  /**
   * Creates an empty registry that destroys its overlays when the window closes.
   *
   * @param window Window whose closing destroys the overlays. Outlives the registry.
   * @param drawer Drawer the overlays draw through.
   * @param context What every overlay receives at construction.
   */
  OverlayRegistry(Window& window, ImGuiDrawer& drawer, const OverlayContext& context);

  /** Unregisters every toggle bind and destroys every overlay. */
  ~OverlayRegistry() override;

  /** Hides every overlay. */
  void HideAll();

  /**
   * Tests whether any overlay is shown.
   *
   * @return True when at least 1 overlay is shown.
   */
  bool AnyVisible() const;

  /**
   * Constructs every overlay in a table, hidden, and registers each toggle bind.
   *
   * @param table Entries built with REX_OVERLAY.
   * @return Ok, or ErrorCategory::Config naming the first entry whose name is already used.
   */
  Result<void> Add(std::span<const OverlayDesc> table);

  /**
   * Shows or hides an overlay. An unknown name does nothing.
   *
   * @param name Overlay name.
   * @param visible True shows it, false hides it.
   */
  void Show(std::string_view name, bool visible);

  /**
   * Flips an overlay between shown and hidden. An unknown name does nothing.
   *
   * @param name Overlay name.
   */
  void Toggle(std::string_view name);

  /**
   * Finds an overlay.
   *
   * @param name Overlay name.
   * @return The overlay, or nullptr when name is unknown.
   */
  ImGuiDialog* Get(std::string_view name);

 private:
  struct Entry {
    std::string name;
    std::unique_ptr<ImGuiDialog> overlay;
    bool visible = false;
    bool bound = false;
  };

  Entry* Find(std::string_view name);
  void Clear();
  void OnClosing(UIEvent& e) override;

  Window& window_;
  ImGuiDrawer& drawer_;
  const OverlayContext& context_;
  std::vector<Entry> entries_;
};

namespace detail {
std::span<const OverlayDesc> StandardOverlays();
}  // namespace detail

/**
 * The debug, console, settings and achievements overlays, with the default keys F3, the
 * backtick key, F4 and F7.
 */
inline const std::span<const OverlayDesc> kStandardOverlays = detail::StandardOverlays();

}  // namespace rex::ui

/**
 * Builds 1 overlay table entry. Class must be constructible from
 * const rex::ui::OverlayContext&.
 *
 * @param Class Overlay class derived from rex::ui::ImGuiDialog.
 * @param name Unique overlay name as a string literal.
 * @param default_key Default key of the toggle bind as a string literal, or "" for none.
 *
 * @code
 * REX_OVERLAY(FooMapOverlay, "map", "M")
 * @endcode
 */
#define REX_OVERLAY(Class, name, default_key)                                                     \
  ::rex::ui::OverlayDesc {                                                                        \
    name, default_key,                                                                            \
        [](const ::rex::ui::OverlayContext& context) -> std::unique_ptr<::rex::ui::ImGuiDialog> { \
          return std::make_unique<Class>(context);                                                \
        }                                                                                         \
  }

/**
 * Declares a constant overlay table.
 *
 * @param table Name of the table variable.
 * @param ... REX_OVERLAY entries.
 *
 * @code
 * REX_OVERLAY_TABLE(kFooOverlays,
 *                   REX_OVERLAY(FooMapOverlay, "map", "M"),
 *                   REX_OVERLAY(FooStatsOverlay, "stats", "F9"));
 * @endcode
 */
#define REX_OVERLAY_TABLE(table, ...)                                        \
  inline constexpr ::rex::ui::OverlayDesc table##_entries[] = {__VA_ARGS__}; \
  inline constexpr std::span<const ::rex::ui::OverlayDesc> table {           \
    table##_entries                                                          \
  }
