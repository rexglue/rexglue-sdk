/**
 * @file        ui/user_interface.h
 * @brief       The ui service interface and the window it opens
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rex/gpu/overlay.h>
#include <rex/gpu/presenter.h>
#include <rex/macros.h>
#include <rex/result.h>
#include <rex/types.h>
#include <rex/ui.h>
#include <rex/ui/overlay.h>
#include <rex/ui/style.h>
#include <rex/ui/ui_event.h>

struct ImFontAtlas;
struct ImGuiIO;
struct ImGuiStyle;

namespace rex {
class Runtime;
}  // namespace rex

namespace rex::ui {

class ImGuiDialog;

/** Cursor visibility over a window. */
enum class CursorVisibility : u8 {
  Visible,    /**< Always shown. */
  AutoHidden, /**< Shown while the mouse moves, hidden after the auto hide delay. */
  Hidden,     /**< Always hidden. */
};

/** Severity of a message box. */
enum class MessageKind : u8 {
  Info,    /**< Information. */
  Warning, /**< A recoverable problem. */
  Error,   /**< A failure. */
};

/** A modal choice shown in a native box. */
struct ChoiceDesc {
  /** Severity, which picks the box icon. */
  MessageKind kind = MessageKind::Error;
  /** Box title. */
  std::string title;
  /** Box text. */
  std::string body;
  /** Button labels. The first is the default and the last is the escape button. */
  std::vector<std::string> buttons;
};

/** A prompt shown inside the title window. */
struct PromptDesc {
  /** Prompt title. */
  std::string title;
  /** Prompt text. */
  std::string body;
  /** Button labels. Empty means a single OK button. */
  std::vector<std::string> buttons;
  /** True shows a text field. */
  bool text_input = false;
  /** Initial text of the field. */
  std::string default_text;
  /** Longest text the field accepts in characters. 0 means no limit. */
  u32 max_length = 0;
};

/** How a prompt ended. */
struct PromptResult {
  /** True when no button completed the prompt. */
  bool cancelled = true;
  /** Index of the pressed button. */
  u32 button = 0;
  /** Text of the field when the prompt had one. */
  std::string text;
};

/** One file type filter of a file dialog. */
struct FileFilter {
  /** Name shown to the user. */
  std::string name;
  /** Extensions separated by semicolons, for example "iso;xex". */
  std::string pattern;
};

/** A native file dialog request. */
struct FileDialogDesc {
  /** What the dialog selects. */
  enum class Mode : u8 {
    OpenFile,   /**< 1 existing file. */
    OpenFiles,  /**< Any number of existing files. */
    OpenFolder, /**< 1 folder. */
    SaveFile,   /**< 1 file that may not exist. */
  };
  /** What the dialog selects. */
  Mode mode = Mode::OpenFile;
  /** Dialog title. */
  std::string title;
  /** File type filters. Empty accepts every file. */
  std::vector<FileFilter> filters;
  /** Folder or file the dialog starts at. */
  std::filesystem::path default_location;
};

/** One connected display. */
struct DisplayInfo {
  /** Index to pass to IWindow::SetMonitor. */
  i32 index = 0;
  /** Name the system gives the display. */
  std::string name;
  /** Left edge in desktop coordinates. */
  i32 x = 0;
  /** Top edge in desktop coordinates. */
  i32 y = 0;
  /** Current mode width in pixels. */
  u32 width = 0;
  /** Current mode height in pixels. */
  u32 height = 0;
  /** Current mode refresh rate in hertz. */
  f32 refresh_rate = 0;
  /** Ratio of pixels to the size the system lays out at. */
  f32 content_scale = 1;
};

/** One preferred locale of the user. */
struct HostLocale {
  /** Language code such as en or ja. */
  std::string language;
  /** Country code such as US, empty when the system gives none. */
  std::string country;
};

/** The platform handles of a window. */
struct NativeWindow {
  /** Which platform the handles belong to. */
  enum class Kind : u8 {
    None,    /**< No native window. */
    Win32,   /**< window is an HWND. */
    Cocoa,   /**< window is an NSWindow pointer and layer a CAMetalLayer pointer. */
    Wayland, /**< window is a wl_surface pointer and display a wl_display pointer. */
    X11,     /**< window is an X11 Window id stored as an integer and display a Display pointer. */
  };
  /** Which platform the handles belong to. */
  Kind kind = Kind::None;
  /** The window handle. */
  void* window = nullptr;
  /** The display connection. Null on Win32 and Cocoa. */
  void* display = nullptr;
  /** The CAMetalLayer on Cocoa. Created on first request and owned by the window. */
  void* layer = nullptr;
  /** The SDL_Window of an SDL backed window on every platform, otherwise null. */
  void* sdl = nullptr;
};

/** Frame timing the debug overlay shows. */
struct FrameStats {
  /** Duration of the last frame in milliseconds. */
  f64 frame_time_ms = 0;
  /** Frames per second. */
  f64 fps = 0;
  /** Frames presented so far. */
  u64 frame_count = 0;
};

/** The display mode the title queries through VdQueryVideoMode and XGetVideoMode. */
struct VideoMode {
  /** Width in pixels. */
  u32 width = 1280;
  /** Height in pixels. */
  u32 height = 720;
  /** Refresh rate in hertz. */
  f32 refresh_rate = 60.0f;
};

/** Receives every window and input event. Returns true when it consumed the event. */
using EventCallback = std::move_only_function<bool(const UIEvent&)>;

/** Supplies the frame timing the debug overlay shows. */
using FrameStatsProvider = std::move_only_function<FrameStats()>;

/** A window the ui provider opened. Every method runs on the UI thread. */
class IWindow {
 public:
  virtual ~IWindow() = default;

  /**
   * Sets the window title. Runs on the UI thread.
   *
   * @param title The new title.
   */
  virtual void SetTitle(std::string_view title) = 0;

  /**
   * Sets the client area shape the window holds while the user resizes it. Runs on the UI thread.
   *
   * @param ratio The shape, AspectRatio::kStretch for none.
   */
  virtual void SetAspectRatio(AspectRatio ratio) = 0;

  /**
   * Sets the smallest client area the window accepts. Runs on the UI thread.
   *
   * @param size Minimum size. A 0 field means no limit on that axis.
   */
  virtual void SetMinimumSize(WindowSize size) = 0;

  /**
   * Enters or leaves fullscreen. Runs on the UI thread.
   *
   * @param fullscreen True for fullscreen.
   */
  virtual void SetFullscreen(bool fullscreen) = 0;

  /**
   * Reports whether the window is fullscreen. Runs on the UI thread.
   *
   * @return True when the window is fullscreen.
   */
  virtual bool Fullscreen() const = 0;

  /**
   * Moves the window to a display. Runs on the UI thread.
   *
   * @param monitor Display index from DisplayInfo.
   */
  virtual void SetMonitor(i32 monitor) = 0;

  /** Brings the window to the front and gives it focus. Runs on the UI thread. */
  virtual void Raise() = 0;

  /**
   * Sets the cursor visibility over the window. Runs on the UI thread.
   *
   * @param visibility The visibility.
   */
  virtual void SetCursorVisibility(CursorVisibility visibility) = 0;

  /**
   * Turns text input for the OS input method on or off. Runs on the UI thread.
   *
   * @param active True turns it on.
   */
  virtual void SetTextInputActive(bool active) = 0;

  /**
   * Returns the width of the drawable area in physical pixels. Runs on the UI thread.
   *
   * @return The width, or 0 while the window has no actual state.
   */
  virtual u32 Width() const = 0;

  /**
   * Returns the height of the drawable area in physical pixels. Runs on the UI thread.
   *
   * @return The height, or 0 while the window has no actual state.
   */
  virtual u32 Height() const = 0;

  /**
   * Reports whether the window has input focus. Runs on the UI thread.
   *
   * @return True when the window has focus.
   */
  virtual bool Focused() const = 0;

  /**
   * Returns the platform handles. Runs on the UI thread. On Cocoa the first call creates the
   * metal layer, which the window destroys when it closes.
   *
   * @return The handles, with Kind::None when there is no native window.
   */
  virtual NativeWindow Native() = 0;

  /**
   * Returns the side a presenter attaches to. Runs on the UI thread.
   *
   * @return The presentation host of this window.
   */
  virtual gpu::IPresentationHost& Presentation() = 0;
};

/** The ui service, served as rex.ui by a ui module. */
class IUserInterface {
 public:
  REX_INTERFACE(IUserInterface, "rex.ui", 1);

  virtual ~IUserInterface() = default;

  /**
   * Opens the title window. Runs on the UI thread.
   *
   * @param config Window placement, title, minimum size and aspect ratio.
   * @param callback Receives every window and input event after the provider handled it.
   * @return The window, owned by the provider, or the error that stopped it opening.
   */
  [[nodiscard]] virtual Result<IWindow*> OpenWindow(const UIConfig& config,
                                                    EventCallback callback) = 0;

  /** Closes the window and drops its overlays. Runs on the UI thread. */
  virtual void CloseWindow() = 0;

  /**
   * Returns the open window. Runs on the UI thread.
   *
   * @return The window, or nullptr when none is open.
   */
  virtual IWindow* GetWindow() = 0;

  /** Runs frames until Quit or until the window closes. Runs on the UI thread. */
  virtual void Run() = 0;

  /**
   * Runs posted work and handles pending events for 1 frame. Runs on the UI thread.
   *
   * @return True while the loop should continue, false after Quit or CloseWindow.
   */
  virtual bool PumpFrame() = 0;

  /** Ends the frame loop. Callable from any thread. */
  virtual void Quit() = 0;

  /**
   * Reports whether the caller is on the UI thread. Callable from any thread.
   *
   * @return True when the calling thread is the UI thread.
   */
  virtual bool IsUIThread() const = 0;

  /**
   * Queues work for the UI thread. Callable from any thread.
   *
   * @param work The work to run on the next frame.
   * @return True when the work was queued.
   */
  virtual bool Post(std::move_only_function<void()> work) = 0;

  /**
   * Runs work on the UI thread and waits for it. Runs inline on the UI thread, otherwise posts
   * and blocks the caller. Callable from any thread.
   *
   * @param work The work to run.
   */
  virtual void RunSynchronous(std::move_only_function<void()> work) = 0;

  /**
   * Returns the ImGui font atlas. Runs on the UI thread.
   *
   * @return The atlas, or nullptr on a provider that runs no ImGui code.
   */
  virtual ImFontAtlas* Fonts() = 0;

  /**
   * Returns the ImGui style. Runs on the UI thread.
   *
   * @return The style, or nullptr on a provider that runs no ImGui code.
   */
  virtual ImGuiStyle* GetImGuiStyle() = 0;

  /**
   * Returns the ImGui IO of the window, with its context made current. Runs on the UI thread.
   *
   * @return The IO, or nullptr when no window is open or the provider runs no ImGui code.
   */
  virtual ImGuiIO* GetImGuiIO() = 0;

  /**
   * Returns the style the overlays draw with. Runs on the UI thread.
   *
   * @return The overlay style.
   */
  virtual Style& GetStyle() = 0;

  /**
   * Constructs every overlay of a table, hidden, and registers each toggle bind. Runs on the UI
   * thread.
   *
   * @param table Entries built with REX_OVERLAY.
   * @return Ok, or ErrorCategory::Config naming the first entry whose name is already used.
   */
  [[nodiscard]] virtual Result<void> AddOverlays(std::span<const OverlayDesc> table) = 0;

  /**
   * Shows or hides an overlay. An unknown name does nothing. Runs on the UI thread.
   *
   * @param name Overlay name.
   * @param visible True shows it, false hides it.
   */
  virtual void ShowOverlay(std::string_view name, bool visible) = 0;

  /**
   * Flips an overlay between shown and hidden. An unknown name does nothing. Runs on the UI
   * thread.
   *
   * @param name Overlay name.
   */
  virtual void ToggleOverlay(std::string_view name) = 0;

  /**
   * Finds an overlay. Runs on the UI thread.
   *
   * @param name Overlay name.
   * @return The overlay, or nullptr when the name is unknown.
   */
  virtual ImGuiDialog* Overlay(std::string_view name) = 0;

  /**
   * Reports whether any overlay is shown. Runs on the UI thread.
   *
   * @return True when at least 1 overlay is shown.
   */
  virtual bool AnyOverlayVisible() const = 0;

  /**
   * Draws a dialog the caller owns. The caller keeps it alive until RemoveDialog. Runs on the UI
   * thread.
   *
   * @param dialog The dialog to draw.
   */
  virtual void AddDialog(ImGuiDialog& dialog) = 0;

  /**
   * Stops drawing a dialog. Runs on the UI thread.
   *
   * @param dialog The dialog to stop drawing.
   */
  virtual void RemoveDialog(ImGuiDialog& dialog) = 0;

  /**
   * Reports whether any dialog or overlay is drawn. Callable from any thread.
   *
   * @return True when at least 1 dialog is drawn.
   */
  virtual bool HasDialogs() const = 0;

  /**
   * Draws every overlay and dialog into an overlay frame. Runs on the UI thread.
   *
   * @param context The frame to draw into.
   */
  virtual void DrawOverlays(gpu::OverlayDrawContext& context) = 0;

  /**
   * Attaches the presenter that paints the window. Runs on the UI thread.
   *
   * @param presenter The presenter, or null to detach.
   */
  virtual void SetPresenter(gpu::IPresenter* presenter) = 0;

  /**
   * Hands over the drawer for textures and primitives. Runs on the UI thread.
   *
   * @param drawer The drawer, or null to drop it.
   */
  virtual void SetOverlayDrawer(std::unique_ptr<gpu::IOverlayDrawer> drawer) = 0;

  /**
   * Returns the overlay drawer. Runs on the UI thread.
   *
   * @return The drawer, or nullptr when none was set.
   */
  virtual gpu::IOverlayDrawer* OverlayDrawer() = 0;

  /**
   * Sets the frame time source of the debug overlay. Runs on the UI thread.
   *
   * @param provider The source, or an empty function for none.
   */
  virtual void SetFrameStats(FrameStatsProvider provider) = 0;

  /**
   * Sets the runtime the achievement overlays read. Runs on the UI thread.
   *
   * @param runtime The runtime, or null once the title stopped.
   */
  virtual void SetRuntime(Runtime* runtime) = 0;

  /**
   * Returns the video mode the title queries. Callable from any thread.
   *
   * @return The mode from the video mode settings.
   */
  virtual VideoMode GetVideoMode() const = 0;

  /**
   * Logs a message and shows it in a native box. Blocks the caller. Callable from any thread.
   *
   * @param kind Severity.
   * @param title Box title.
   * @param body Box text.
   */
  virtual void ShowMessage(MessageKind kind, std::string_view title, std::string_view body) = 0;

  /**
   * Logs a message and shows a native box with buttons. Blocks the caller. Callable from any
   * thread.
   *
   * @param desc The box.
   * @return The index of the pressed button, or the escape index when the box was closed.
   */
  [[nodiscard]] virtual Result<u32> ShowChoice(const ChoiceDesc& desc) = 0;

  /**
   * Queues a prompt inside the title window. Never blocks. Callable from any thread.
   *
   * @param desc The prompt.
   * @param done Called on the UI thread when the prompt ends, cancelled at once when no window
   *             is open.
   */
  virtual void Prompt(PromptDesc desc, std::move_only_function<void(PromptResult)> done) = 0;

  /** Ends every pending prompt as cancelled. Runs on the UI thread. */
  virtual void CancelPrompts() = 0;

  /**
   * Shows a native file dialog. Blocks the calling thread. Callable from any thread.
   *
   * @param desc The dialog.
   * @return The selected paths, empty when the user cancelled.
   */
  [[nodiscard]] virtual Result<std::vector<std::filesystem::path>> ShowFileDialog(
      const FileDialogDesc& desc) = 0;

  /**
   * Lists the connected displays. Callable from any thread.
   *
   * @return The displays, empty when the provider has none.
   */
  virtual std::vector<DisplayInfo> Displays() = 0;

  /**
   * Lists the preferred locales of the user, most preferred first. Callable from any thread.
   *
   * @return The locales, empty when the provider has none.
   */
  virtual std::vector<HostLocale> PreferredLocales() = 0;
};

}  // namespace rex::ui
