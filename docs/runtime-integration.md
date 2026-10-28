# Runtime integration

Use these contracts when embedding the SDK or replacing its presentation layer.
See [Frame-input diagnostics](frame-input.md) for prepared GPU-input views and
[Mod system](mod-system.md) for package contracts.

- [ReXApp lifecycle and overlays](#rexapp-lifecycle-and-overlays)
- [Overlay settings, skin and FPS
  counter](#overlay-settings-skin-and-fps-counter)
- [Host session data and notifications](#host-session-data-and-notifications)
- [Detached video delegate](#detached-video-delegate)
- [Physical write notices](#physical-write-notices)
- [Lua package runtime](#lua-package-runtime)
- [Guest output frame timing](#guest-output-frame-timing)

## ReXApp lifecycle and overlays

[`ReXApp`](../include/rex/rex_app.h) supports title-owned presentation with
`config.graphics == nullptr` and an empty `config.gpu_plugin`. Override
`OnCreateImmediateDrawer()` and drive `ImGuiDrawer::Draw` with an
`AppUIDrawContext`. The title owns its device, queues, swapchain and present
timing. The drawer's `CreateTexture` must return null until its device is ready;
font upload is lazy, and detached drawers do not receive `OnEnterPresenter`.

`ShutdownOverlays()` releases the SDK overlay stack before a title releases its
host device. Call it on the UI thread after stopping input polling, draining
queued UI/GPU work and releasing dialogs owned by the title or mods. It
unregisters overlay binds, clears the runtime input active callback and borrowed
ImGui drawer, then releases the achievement listener, SDK dialogs, ImGui drawer
and immediate drawer. `OnDestroy()` calls it too; repeated calls do nothing.

The overlay binds default to F3 debug, Backtick console, F4 settings, F1 mods,
F7 achievements and F12 FPS counter. A title chooses its own defaults by calling
`rex::ui::SetBindDefault` with the bind name before overlay setup, for example
in `OnPreSetup`. A key saved in config still wins.

`SettingsLayout::choice_labels` turns an integer cvar row into a combo of exactly
the mapped values, keyed by decimal strings and listed in ascending order.
`show_descriptions = false` limits default row
tooltips to rows that do not apply immediately and shows only their lifecycle
text. When the layout has no `monitor` entry, the overlay fills it from
`rex::ui::MonitorChoiceLabels()` (`rex/ui/monitors.h`), which names the
connected displays in the order the window resolves the `monitor` cvar.

Player options: `audio_volume` (0 to 100 percent, hot reload) multiplies with
the master gain from `rex::audio::SetOutputGain`, so a title or mod gain keeps
applying. `audio_play_in_background` (default true) silences output like
`audio_mute` while the main window is unfocused or minimized; `ReXApp` publishes
that state through `rex::audio::SetOutputFocused(bool)` and
`rex::audio::IsOutputFocused()` (`rex/audio/output_state.h`), and a title with
its own window may call them. `window_size` (`UI/Window`, requires restart) sets
the startup window to a listed `WxH` or `maximized`; any other size comes from
`window_width`/`window_height` (logical pixels, up to 8192), which outrank it.
`window_size` outranks `resolution`, and the host window size never changes the
guest video mode: only `video_mode_width`/`video_mode_height` and the
`resolution` preset do.
`resolution_scale` 0 (`Display`, requires restart) is Auto: it selects the
smallest whole-number draw scale that covers the height of the guest-aspect
image fitted inside the presented surface, then clamps it to 1 through 7.
A non-default `draw_resolution_scale_x`/`draw_resolution_scale_y` overrides
its axis.
`ReXApp` reports the surface (the selected display's pixel size in fullscreen,
otherwise the window's pixel size after creation) with
`IGraphicsSystem::SetDisplaySurfaceSize` before the Runtime creates the GPU
caches, so the scale is chosen once at startup. The GPU plugin uses ABI 5.
After device limits apply, the plugin logs
`Display: guest=WxH scale_requested=<auto|XxY> scale_effective=XxY surface=WxH`,
and on the first presented frame
`Display: swap_source=WxH output=x,y,w,h`.
`IGraphicsSystem::GetDisplaySurfaceSize` returns the last reported surface as one
coherent pair, or false with both outputs zero while it is unknown or minimized;
`ReXApp` refreshes it on every window resize.

A title can change the presented aspect per swap without changing the guest
buffer. `IGraphicsSystem::SetPresentationAspect(x, y)` is called on the guest
thread immediately before the guest swap: two positive values request that
display aspect for that swap, `(0, 0)` clears the request, and exactly one zero
returns false. The request is one-shot. `VdSwap` moves it
(`TakePresentationAspect`) into a trailer of the `PM4_XE_SWAP` packet (signature
`ASPX`, aspect x and y, 64-bit sequence of accepted calls counted from 1), so the
GPU thread reads the aspect of the swap it decodes, not the latest one. A swap
without a request omits the trailer. New content without an override uses the
`VdQueryVideoMode` aspect. Both backends' `IssueSwap` replace only the display
aspect handed to `Presenter::RefreshGuestOutput`; guest-output and source
dimensions are unchanged. The aspect stays with its image: a swap that repeats
the previous source texture contents (same texture instance and content
generation, not merely the same frontbuffer address) keeps that image's aspect,
and a refresh the presenter rejects leaves the shown image's record unchanged.

`SetSwapPresentationObserver` registers a callback invoked on the GPU thread for
every decoded swap, including skipped and failed ones, with the decoded request,
the aspect handed to the presenter, the content identity and the refresh result;
clearing it waits for a running callback. `SetPresentationAspect` returns true
when it accepts a valid request and the command processor supports it (D3D12
and Vulkan).

The request, trailer and observer are part of GPU plugin ABI 5. Build the runtime,
the Xenos plugin, the installed headers and the title from matching sources;
the ABI number alone does not establish compatibility. The presenter honors
the aspect only through its normal fitting, so original-aspect movies
need `present_letterbox` on and `present_safe_area_x`/`present_safe_area_y` at
100.

`present_bar_fill` (`Display`, player audience, hot reload) chooses what fills
the pillarbox or letterbox bars left by the fitted image. `black` (default)
clears them. `blur` draws a dimmed (0.4x), blurred copy of the guest output over
the whole surface, scaled to cover it without distortion (centered crop), then
draws the sharp image over it.

The blur halves the guest output through bilinear passes until neither side
exceeds 96 pixels (four passes for 1280x720, about 0.3 megapixels written), then
scales the last level with bilinear filtering. Upscaling and sharpening effects
do not affect the fill because it is computed from the guest output.

The fill applies only when bars exist, so it does nothing with
`present_letterbox` off or an exactly fitting window. D3D12 and Vulkan support
the fill, and UI drawn after the guest output covers it. The setting belongs
to the presenter and does not change the GPU plugin ABI.

`input_gamepad_in_background` (default false, hot reload) sets SDL's background
joystick hint so the SDL input backend keeps reading gamepads while the window
is unfocused.

Diagnostics for players: `rex::GetLogFilePath()` (`rex/logging.h`) returns the
absolute path of the active log file, or empty without a file sink. The first
`InitLogging` installs a best-effort crash report: `std::terminate` logs a
critical line with the active exception's message, and on Windows an unhandled
exception writes a small `<log stem>.dmp` minidump beside the log unless a
debugger is attached, then logs its code, access address, faulting
`module+offset` and thread. MSVC terminate handlers are per thread, so a
terminate on another Windows thread is logged only as an abort with its thread
ID. Crash logging waits a bounded time, so a lock held by the failing thread or
exhausted memory can lose the line but never hangs the exit.
`rex::LaunchFileExplorer` and `rex::LaunchWebBrowser` (`rex/system.h`) open the
folder or URL in the desktop shell. A command cvar named exactly by a
`SettingsLayout` tab source shows as a settings button that invokes it with no
arguments, so a title can offer "Open Log Folder"; category and prefix sources
still skip commands, audience rules still apply, and commands are never saved.

For asynchronous presentation shutdown, return false from
`OnWindowCloseRequested()`, drain presentation work, then call
`window()->RequestClose()` on the UI thread. A detached title closed without a
Runtime quits the UI loop normally and reaches `OnShutdown()`,
`ShutdownOverlays()` and `OnDestroy()`. Any close with a Runtime, or a
presenter-backed title closed before its Runtime exists, shuts down mods,
terminates the title and hard-exits. `OnBeforeHardExit()` runs on the UI thread
after title termination, immediately before the hard exit. Its default is a
no-op.

Guest threads may still poll input, create XAM dialogs or push achievement
toasts during `OnBeforeHardExit()`, and stragglers that termination left running
can continue to do so. The hook may only drain and release the title's own
presentation resources. It must not call `ShutdownOverlays()`, which would race
the input active callback and dialog lifetimes. Call `ShutdownOverlays()` during
an orderly close without a Runtime or in `OnDestroy()`. Setting up overlays
again re-arms it, so a call before setup does not suppress later teardown.

`IsOverlayInputCaptured()` reports whether attached ImGui dialogs want mouse or
keyboard capture, including dialogs supplied by a title. Its inverse is the
runtime input system's active callback. The SDL gamepad driver and the
keyboard/mouse driver both honor that gate. It does not cover every guest input
path and does not guarantee controller isolation.

Console keyboard focus and guest XAM modal dialogs gate guest input: the gate
follows mouse or keyboard capture, not only mouse capture.

The `window_hidden` cvar defaults to false and requires a restart. When true,
SDL keeps the initial window hidden by skipping `SDL_ShowWindow`; it still
creates a window and requires an available SDL video backend. Harnesses should
also pass `fullscreen=false`; a fullscreen request applied during window open
may make SDL show the window despite the flag.

## Overlay settings, skin and FPS counter

The settings overlay (F4) lists `Audience::kPlayer` rows by default. Its "Show
advanced settings" toggle, stored in the hidden `settings_show_advanced` cvar,
adds advanced rows and appears only when some row is advanced. Developer rows
appear only when the hidden `settings_show_developer` cvar is set in the config
or on the command line. Hidden rows and non-command rows with a single allowed
value never appear. A command appears as a button only when a tab source names
it exactly; category and prefix sources do not include commands. Audience rules
still apply. Chain `.audience()` and `.persistence()` on a cvar definition to
set its policy. A static definition applies both before config loads; a flag
registered after initialization passes persistence to the `FlagRegistrar`
constructor instead.

`ReXApp::OnConfigureSettings` runs each time F4 opens and fills a
`ui::SettingsLayout` (`rex/ui/overlay/settings_layout.h`). Its tabs list
sections whose sources select rows by exact name, by name prefix ending in `*`,
or by `@Category`, which includes subcategories; each row goes to the first
source that matches it. Visible rows that no source places land in a tab named
after their top-level category, so a new cvar never vanishes; with
`developer_tab` set, unplaced developer rows instead share that last tab, one
section per category. Audience rules use the same selectors, override the row's
own audience for display only, and later rules win; they cannot reveal a row its
definition marks hidden. `labels` replaces a row's label by cvar name, and
`format_label`, when set, rewrites the labels derived from descriptions.
`choice_labels` names a string row's allowed values by cvar name and value;
other values go through `format_label`. An empty layout keeps the category tree
with its "All" entry. Key bindings show as a button that starts a capture, with
Reset only when the binding differs from its default.

`SettingsLayout::float_decimals` maps a Double cvar name to fixed decimal places
(0 through 17); an absent or invalid entry keeps four decimals. The editor seeds
its field text from that rounded display. With a precision entry, it submits the
parsed double without further display rounding; cvar constraints still validate
edits. The Double cvar getter and config persistence use `std::to_string`, which
keeps six decimal places.

`SettingsLayout::inline_toggles` lists `SettingsInlineToggle` values with
`toggle` (bool cvar name), `value` (string or integer cvar name), and
`unlock_when_checked` (default true). Each pair shows one label, a checkbox,
then the value field; the value has no separate row. The bool controls the
label, placement and audience, and search covers both cvars' names, labels and
descriptions. The value field is disabled unless the checkbox matches
`unlock_when_checked`, and each field keeps its own lifecycle lock and
save/revert policy. Missing cvars, incompatible types and cvars whose
definitions mark them hidden are ignored; the first valid pair owns each cvar.

`SettingsLayout::tooltips` maps cvar names to custom hover text. An entry
replaces the description and cvar name regardless of `show_descriptions`,
followed by the colored lifecycle note only when the field does not apply
immediately. It appears only on the row label, including disabled rows; the
value control then shows no tooltip. For an inline pair, the toggle's entry is
used; the value's entry is used only when the toggle has none.

`SettingsLayout` is part of its containing dialog's public C++ layout;
rebuild the SDK and consumers together when changing it. Plugin ABI versions are
defined in the [GPU](../include/rex/system/gpu_plugin.h) and
[mod](../include/rex/system/mod_plugin.h) headers.

`input_vibration` (Input, player audience, default true, hot reload) controls
motor output. When false, `InputSystem::SetState` sends zero speeds to every
assigned device, stopping active rumble on that call; driver results still
determine its native return value. When true, requests pass through unchanged.

Row labels use the description's first sentence when it has at most 48
characters. Otherwise they use the cvar name with underscores replaced by spaces
and its first character capitalized. The default tooltip shows the full
description, the cvar name and when a change applies. Search ignores case across
name, label and description. Save calls `cvar::TrySaveConfig`, which rewrites
the file even when every value is default but leaves out values supplied on the
command line or by environment variable, keeping their saved lines; a failed
write keeps the unsaved count and shows an error. Revert restores the values
present at the last save or when the overlay opened. Both cover rows saved in
configuration, except hidden rows and commands. Escape or the close button
closes the settings, mods and achievements overlays; queued close and FPS
counter work is dropped once `ShutdownOverlays()` runs.

`ui::Style` offers optional scaling and drawing customization:

- `scale.reference_height` scales fonts and ImGui sizes with the output height,
  clamped to `min_scale` and `max_scale`. Dialogs multiply their own fixed sizes
  by `ImGuiDrawer::overlay_scale()`.
- `chrome.draw_window` and `chrome.draw_panel` replace window and panel
  backgrounds through `BeginOverlayWindow` and `BeginOverlayPanel`
  (`rex/ui/overlay/window_chrome.h`). Settings, mods, achievements and the FPS
  counter use them. `title_height_em` sizes the title band and `title_indent_em`
  moves its text clear of wide frame corners.
- `chrome.draw_button` and `chrome.draw_selection` draw under button labels and
  hovered, pressed or selected list rows through `OverlayButton` and
  `OverlaySelectable`, which the built-in overlays use in place of
  `ImGui::Button` and `ImGui::Selectable`.
- `fps_counter` sets the counter's text color, background alpha and margin.

The FPS counter follows the player cvar `show_fps_counter`, and
`bind_fps_counter` (F12) toggles it. The counter and F3 read the provider given
to `SetGuestFrameStats`. Without one they read the presenter's
`GetGuestOutputRefreshStats()`, which counts refreshes that carried a guest
image. Detached titles have no presenter and show `--` until they set a
provider.

The achievements overlay lists achievements with `AchievementInfo::custom` set
(or `custom = true` in `achievements.toml`) on a Custom tab beside Original.
The tabs appear only when a custom achievement exists. Custom sorts by
gamerscore from lowest to highest, then label. It has its own count and
gamerscore totals. The marker is separate from `flags`, the XDBF value the guest
reads.

These features use the public C++ layouts of `ui::Style`, `ImGuiDrawer`,
`Presenter`, `ReXApp` and the settings and achievements dialogs. Build the SDK
and its consumers with matching headers.
Fullscreen, vsync, resolution scale, and post effects belong to the Display
category; Discord activity belongs to Online. Config files key by cvar name.

## Host session data and notifications

[`rex/session_data.h`](../include/rex/session_data.h) lets a host share opaque
title bytes with players who join its System Link or Live session.
`SetHostDataProvider` registers a cheap callback that may run on any calling
thread. Host session creation captures lobby bytes. `Current()` reports
`kHost` with `game_ready == false`. A non-host create requests lobby data from
the host address in its `XSESSION_INFO`, enters `kPending`, then reports
`kReceived` on a valid reply. A missing route or System Link disabled on a
non-host create gives `kUnavailable` immediately.

Call `PrepareForGame()` before title gameplay settings freeze. On the host it
captures the current provider once for that game, replacing the lobby snapshot.
On a joiner it starts a fresh authenticated game-only request, independently of
any received lobby bytes. `XSessionStart` also invokes preparation. Repeated
preparation keeps the same capture or request until `XSessionEnd`, which clears
readiness and cancels in-flight game preparation. A later game captures the
provider again. Replacing the provider does not change an already prepared
game's bytes.

A ready host reports `kHost`, and a joiner with an accepted game reply reports
`kReceived`, both with `game_ready == true`. Before applying game policy, the
title must call `WaitForHostData` as needed and require readiness as well as an
appropriate state. Lobby data and bytes retained after game end do not establish
readiness. The SDK transports bytes. The title owns decoding, compatibility,
admission decisions, and applying settings or mods to gameplay.

The provider runs outside the session data lock and may read Current or replace
itself. Output above `kMaxBytes` (1,024) is truncated with a warning. A null or
throwing provider freezes empty bytes. Exceptions are logged and do not escape
into guest dispatch. A result superseded by session deletion, creation, or game
end is discarded. `XSessionDelete` and the last `XNetCleanup` return to `kNone`.

Each lobby or game request retries after 250 ms, doubling to 2 s, and becomes
`kUnavailable` after 15 seconds without an accepted reply. `WaitForHostData`
blocks only while `kPending`, up to its timeout. If the waited generation ends
or is replaced, it returns `kNone` without bytes. Call Current for the newer
state. Session bytes remain local SDK state and are not guest network payloads.

### Wire framing and freshness

The codec and state machine in
[`host_session_data.h`](../src/kernel/xam/host_session_data.h) and
[`host_session_data.cpp`](../src/kernel/xam/host_session_data.cpp) own the wire
contract. All messages use ASCII `RXSD`, version 1, and this 32-byte prefix:

| Offset | Field |
|---|---|
| 0 | Magic `RXSD`, 4 bytes |
| 4 | Version, u8 |
| 5 | Type, u8 |
| 6 | Title data length, u16 little-endian |
| 8 | Session ID, 8 bytes |
| 16 | Request nonce, 16 bytes, echoed by the reply |

Lobby types 1 (request) and 2 (reply) place title data at offset 32. Game-only
types 3 (request) and 4 (reply) extend the header to 64 bytes:

| Offset | Game-only field |
|---|---|
| 32 | Generation, u64 little-endian |
| 40 | Requester ID, 16 bytes |
| 56 | Game attempt, u64 little-endian |
| 64 | Title data, up to 1,024 bytes |

Requests carry no title data. The final 32 bytes of every message are
`HMAC-SHA256(XNKEY, "RXSD-HOSTDATA-v1" || preceding bytes)`, including all game
framing when present. Lobby requests are 64 bytes and game requests are 96.
Replies reach 1,088 and 1,120 bytes respectively. Older peers that only accept
types 1/2 cannot satisfy a game-only request. Generation and attempt must be
nonzero, and the requester ID must contain a nonzero byte.

A game request carries the joiner's minimum acceptable generation. A game reply
carries the actual prepared host generation. The host serves game data only
when ready and at or above that floor. A joiner accepts only the current
session ID, a nonce it issued for this request, the current requester ID and
attempt, and a generation meeting its requested floor. Game end retires the
requested floor even if the reply never arrived and clears old reply nonces.

The host retains up to 64 requester records for the session lifetime, with no
eviction to admit new requesters. Each binds requester ID and transport route
to the last served attempt and host generation. A new requester must start at
attempt 1. A retry of the same attempt can receive only its bound generation.
The next attempt must be exactly one greater and can bind only when the host's
prepared generation is also exactly one greater. Unknown later attempts,
skipped attempts or generations, route changes, and new IDs when the cache is
full are refused. Ambiguous histories, including an earlier attempt that the
host never saw, require session recreation rather than guessing a generation.

Malformed, oversized, unauthenticated, unexpected, or stale messages are
dropped with a debug log. The host answers each nonce at most three times,
remembers 64 reply nonces, and rate-limits replies to 20 per second with a burst
of 20. That nonce cache may evict its oldest entry. The requester/generation
cache does not. Joiners rotate request nonces after three sends. These limits
bound reply traffic. They do not make title bytes trustworthy beyond possession
of the shared session key. The HMAC authenticates holders of that key and does
not encrypt the data or validate its title-specific meaning.

### Transports

In relay mode, including Live sessions, these messages use relay DATA from
guest port 0 to guest port 0 at the host's virtual address, like round-trip
probes. Guest sockets never own port 0. The client routes `RXSD` payloads to the
SDK instead of probe logic, and the side channel always uses the relay rather
than a direct path. The relay protocol is unchanged.

In direct-LAN mode each instance binds a native UDP side socket to its selected
`system_link_local_address` on port 30110 when it creates a session. Requests go
to the host's address on that port, and replies return to the sending route.
Separate instances on one machine require distinct local addresses. A firewall
must allow UDP 30110 between LAN players. These transports authenticate the
session data envelope with the session key. The title must still validate the
received settings. An exit handler stops the retry timer and LAN worker before
the relay client is destroyed.

### Notifications

[`rex/ui/notifications.h`](../include/rex/ui/notifications.h) exposes
`PostNotification(title, message, duration)`, callable from any thread. Toasts
show one at a time above the achievement toast, with the `ToastStyle` colors,
the `chrome.draw_panel` skin and overlay scale. Each is logged when posted, its
duration (at least one second) starts when it is first drawn, and at most 16
wait; later posts are dropped until one shows. `ReXApp` creates the drawing
`NotificationToastDialog` (`rex/ui/overlay/notification_toast.h`) with its
overlays and releases it in `ShutdownOverlays()`; a host without overlays only
logs.

`ToastStyle::bottom_inset` keeps a fraction of the display height clear at the
bottom, clamped to 0-0.5, for a title HUD in that corner; the achievement and
notification toasts both sit above it. The default 0 reserves no extra bottom
inset.

## Detached video delegate

A title running without an `IGraphicsSystem` can supply its own guest video
services through
[`IDetachedVideoDelegate`](../include/rex/system/interfaces/video_delegate.h).
Register it with `Runtime::SetDetachedVideoDelegate` after `Runtime::Setup` and
before the module launches. The setter returns `X_STATUS_NOT_SUPPORTED` when a
graphics system is set or in tool mode, and `X_STATUS_OBJECT_NAME_COLLISION`
when replacing one delegate with another. Passing `nullptr` always clears it.
`Runtime::Shutdown` clears it before releasing the kernel state.

The Runtime holds a `std::shared_ptr` behind a mutex. Each export copies it for
the duration of its call, so a guest thread still inside a call during title
termination or hard exit keeps the delegate alive. A delegate cleared from
another thread can therefore receive a call that started before the clear.
Methods run synchronously on the calling guest thread, may run concurrently,
must not block on host presentation and must not call the setter.

`VdSetGraphicsInterruptCallback`, `VdInitializeRingBuffer` and
`VdEnableRingBufferRPtrWriteBack` go to the delegate first and do not reach a
graphics system. `VdSwap` decodes the frontbuffer fetch and writes its 64-dword
ring packet exactly as without a delegate, then calls `OnSwap` with the physical
frontbuffer address and the rebased fetch dwords. A frontbuffer without a
physical mapping writes nothing and skips `OnSwap`. All other Vd* exports,
including `VdInitializeEngines` and
`VdSetSystemCommandBufferGpuIdentifierAddress`, keep their SDK behavior. Without
a delegate the registration exports forward to the graphics system, or log one
warning per export when none is loaded.

The delegate is part of Runtime's public C++ layout. Build the SDK and consumers
with matching headers.

## Physical write notices

Physical memory invalidation callbacks hear of writes to a watched page from
these paths:

- Guest stores through any of the three guest views of physical memory
  (0xA0000000, 0xC0000000 and 0xE0000000): `EnablePhysicalMemoryAccessCallbacks`
  protects the page in every view, not only the one that allocated it, and the
  fault calls the callbacks before the store lands. A page a view does not let
  the guest write but the host does is made read-only for the watch and writable
  again when it is unwatched.
- Host writes through the physical view: the writer calls
  `Memory::NotifyPhysicalWrite(physical, length)` after writing, which calls
  every callback when any of the range's system pages is watched and leaves
  protection and watches as they are. The XMA decoder reports its output this
  way before it stores the context the guest reads, and file reads before they
  complete.
- Writes the host OS makes, such as socket receives, go through the physical
  view from `Memory::TranslateForHostWrite` and are reported the same way, so a
  watched receive buffer does not fail the OS's buffer check. A receive reports
  the bytes it copied: never more than the buffer, and the whole buffer when it
  fails with a truncated datagram.
- `Memory::Restore` reports all of physical memory after rewriting it.
- Allocation, `Protect` and the stale-protection recovery path replace host
  protection. Each calls the callbacks for every system page whose watch it
  drops, over the whole span it changes (whole guest pages for `Protect`, the
  aligned size for `Alloc`), and drops the flags under the same hold of the
  global critical region, so a watch armed meanwhile is never left trusted on a
  writable page.

`Memory::IsPhysicalRangeWriteWatched(physical, length)` reports whether every
system page of a range is watched in every view. A consumer that arms, checks it
and then finds the bytes unchanged can skip later compares until a callback
names the range. A host writer reports after its bytes land, so a read that
races the write may see the old bytes, as an unwatched read would; any read
ordered after the writer's completion sees the new ones. Callbacks run under the
global critical region and sometimes a heap lock, so they must take only locks
that are never held while calling into `Memory`. Only watched ranges change
protection, so stores elsewhere take no new faults.

## Lua package runtime

Build the SDK and consumers with matching 0.13.0 headers and runtime for the
public script host, prepared loadout replacement, active Lua digest, and
post-commit callback. `kModPluginAbiVersion` is unchanged. See
[Mod system](mod-system.md#launch-publication-and-status) for publication and
failure behavior, and
[Active digest and title notification](mod-system.md#active-digest-and-title-notification)
for callback lifetime and snapshot integration. Native DLLs and asset caches
retain their separate restart requirements.

The pinned Lua sources compile as C++ with Lua's native exception bridge.
SDK-owned protected thunks cover environment construction, compilation,
execution and raw decoding. Lua exceptions unwind C++ owned values; native
allocation failures are translated inside the decoder. The VM closes before any
title callback. A separate test build injects allocation denial throughout
compilation, execution and decoding and checks state teardown; the injection
seam is absent from the runtime library and installed API.

## Guest output frame timing

The D3D12 presenter exposes `GetGuestOutputFrameStats()` for CPU or GPU output
overrides. It counts distinct output tokens whose swapchain `Present` returns
`S_OK`. Repeated presentation, older mailbox entries, failed and occluded calls,
and producer or consumer-copy completion do not add frames. Successful original
output resets the measured interval.

FPS and mean frame time update over completed windows of at least one second.
After one second without a new presented token the rate becomes zero. Startup
has no timing sample until the first complete window. The counter uses bounded
scalar state and a monotonic clock, and its snapshots are thread-safe. It does
not establish physical scanout or GPU retirement. F3 and the FPS counter display
it through `SetGuestFrameStats`.
