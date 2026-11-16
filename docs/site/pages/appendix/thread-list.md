# Thread List

## Threads

These are the threads of a running ReXGlue program, by the name a debugger or profiler shows. See [App Lifecycle](../app/app-lifecycle.md#ui-thread) for the UI thread and [Kernel Services](../runtime/kernel.md#threads) for guest threads.

| Thread | Purpose | Starts and stops |
|---|---|---|
| UI thread | Runs every `TitleApp` lifecycle step and callback, such as `OnInit`, the window and its event loop. | Runs for the whole process. It is the thread that calls `App::Init`, usually the main thread. |
| `Main XThread` | Runs the entry point of the game. | Starts in `TitleRuntime::Launch` and stops when the game unloads. |
| `XThread` and a hex ID, such as `XThread000A` | Runs a thread that the game creates. A thread that the game names shows its own name. | Starts in `ExCreateThread` and stops when the thread exits or the game unloads. |
| `Title Monitor` | Reports a normal exit when the main thread returns. | Starts in `TitleRuntime::Launch` and stops in `TitleRuntime::Unload`. |
| `Kernel Dispatch` | Runs deferred overlapped completions and queued DPCs. | Starts when `TitleRuntime::Load` loads the executable and stops when the game unloads. |
| `GPU Commands` | Runs the command processor, which reads the commands the game writes for the GPU. | Starts when `TitleRuntime::Launch` attaches the game to a Xenos graphics driver and stops when the game detaches. |
| `GPU VSync` | Raises the vertical blank interrupt at the refresh rate. | The same as `GPU Commands`. |
| Present thread, with no name | Makes the present calls of the graphics driver and draws the overlays. | Starts when the graphics system starts a driver and stops when the graphics system stops it. |
| `Audio Worker` | Calls the audio callbacks of the game. | Starts when `TitleRuntime::Launch` starts guest audio and stops when the game unloads. |
| `XMA Decoder` | Decodes XMA audio. | Starts and stops with `Audio Worker`. |
| Mix thread, with no name | Mixes on a timer while no audio device is open, and checks for a lost device. | Starts when the audio system starts and stops when the audio system stops. |
| `rex::platform::TimerQueue` | Fires guest timers. The process has one. | Starts with the first timer and stops in `rex::Stop`, which `App::Shutdown` calls. |
| `DXGI UI Tick` | Waits for each vertical blank of the host display and signals the presenter, so that the UI redraws once for each refresh. Windows only. | Starts when the presenter is set up and stops when it shuts down. |
| The spdlog flush thread, with no name | Flushes the log every `log.flush_interval` seconds. | Starts when the log file opens and stops when the logger session ends. |
| `Shutdown Signals` | Waits for SIGINT and SIGTERM. Linux and macOS only. | Starts in `App::Init` and stops in `App::Shutdown`. |

The graphics driver modules start threads that build pipelines and translate shaders: `D3D12 Pipelines`, `Vulkan Pipelines`, `Shader Translation`, `D3D12 Storage writer` and `Vulkan Storage writer`.

On Windows, the system calls the console control handler for Ctrl+C and for closing the console window on a thread of its own.
