# Binaries and Modules

## Binaries

The program links the first six libraries through the `rex::sdk` CMake target. `rex::Init` loads the others by name at runtime, and loads `rexxam` and `rexkernel` as modules too. See [Introduction](../index.md#runtime) for how they fit together.

| Binary | Purpose |
|---|---|
| `rexbase` | Provides settings, logging, `rex::Init`, ImGui and the platform layer. |
| `rexppc` | Provides what recompiled code needs at runtime: the routing of the MMIO blocks, the registry of recompiled code, exception handling, and the guest stack and detour frames. |
| `rexxenos` | Describes the Xenos registers, formats and texture layouts. It is a static library. |
| `rexkernel` | Provides `TitleRuntime`, the kernel exports, guest memory, and guest audio with XMA decoding. |
| `rexxam` | Implements the XAM exports. |
| `rexapp` | Provides `App`, `TitleApp`, the command line and the entry macros. |
| `rexfilesystem` | Provides the filesystem. `rex::Init` loads it by name. |
| `rexhid`, `rexapu`, `rexui`, `rexgpu` | Provide the input, audio, UI and graphics systems. `rex::Init` loads each by name. |
| Driver modules | Provide the drivers. `rex::Init` loads the ones that the settings name. |
| `rexglue` | Creates projects and recompiles the XEX. It is a separate program and never loads into the program it builds. |

## Driver Modules

See [Modules and Interfaces](../app/modules.md#selecting-drivers) for how a system selects a driver.

| Module | Driver | Purpose |
|---|---|---|
| `rexui-sdl3` | `sdl3` | Creates the window and the UI through SDL3. |
| `rexhid-sdl3` | `sdl3` | Reads input devices through SDL3, on every platform. |
| `rexhid-xinput` | `xinput` | Reads controllers through XInput. Windows only. |
| `rexapu-sdl3` | `sdl3` | Plays audio through SDL3. |
| `rexgpu-xenos-d3d12` | `d3d12`, family `xenos` | Renders graphics through Direct3D 12. Windows only. |
| `rexgpu-xenos-vulkan` | `vulkan`, family `xenos` | Renders graphics through Vulkan. |
| `rexgpu-xenos-null` | `null`, family `xenos` | Processes the GPU commands of the title and draws nothing, so a title runs without a host GPU. |

## Graphics Driver Values

See [Graphics](../devices/graphics.md#drivers) for the `gpu.driver` setting.

| `gpu.driver` | Driver |
|---|---|
| `any` | Selects `d3d12` on Windows and `vulkan` elsewhere. This is the default. |
| `d3d12` | Selects Direct3D 12, on Windows only. |
| `vulkan` | Selects Vulkan. |
| `null` | Selects the null Xenos driver. |
| `none` | Starts no driver. A `TitleApp` then fails in `Init`. |

## System Libraries

The seven libraries that an `App` loads, in load order. A module can only connect when the SDK or an earlier module serves every interface it requires. See [Modules and Interfaces](../app/modules.md#system-libraries) for why the order matters.

| Library | Serves | Requires |
|---|---|---|
| `rexfilesystem` | `IFileSystemFactory` | `ILogger` |
| `rexhid` | `IInputSystem` | `IDriverContext`, `IAppInfo`, `ISettingsRegistry`, `ILogger` |
| `rexapu` | `IAudioSystem` | `IDriverContext`, `IAppInfo`, `ISettingsRegistry`, `ILogger` |
| `rexui` | `IUISystem`, `IUIControl` | `IDriverContext`, `IAppInfo`, `ISettingsRegistry`, `ILogger` |
| `rexgpu` | `IGraphicsSystem`, `IGraphicsControl` | `IDriverContext`, `IAppInfo`, `ISettingsRegistry`, `ILogger` |
| `rexxam` | `IXAMTitleLifecycle`, `IXAMTitleServices`, `IXAMLanguage`, `IXAMExports` | `ISettingsRegistry`, `ILogger` |
| `rexkernel` | `IKernelControl` | `ISettingsRegistry`, `ILogger`, `IFileSystemFactory`, `IUISystem` |

`IUIControl`, `IGraphicsControl`, `IXAMExports` and `IKernelControl` are private interfaces that only the app and the kernel can find. The app serves `IAppInfo` from a code module of its own.
