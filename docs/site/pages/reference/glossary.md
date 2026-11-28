# Glossary

This page explains the acronyms and the SDK terms that the other pages use. The site shows these explanations as tooltips where a term appears.

## Acronyms

### APC

Asynchronous procedure call: a function queued to run on a guest thread.

### APU

The Xbox 360 audio hardware and its register block at `0x7FEA0000`.

### D3D12

Direct3D 12.

### DLL

Dynamic link library. A DLL that a game loads is recompiled as a DLL module.

### DPC

Deferred procedure call: work the kernel queues to run on the `Kernel Dispatch` thread.

### EDRAM

The embedded memory of the Xenos GPU that render targets are drawn into.

### GDFX

Game Disc File System: the filesystem of an Xbox 360 disc image.

### HID

Human interface device. The `hid.*` settings cover input devices.

### IRQL

Interrupt request level, the kernel priority level that a guest thread runs at, kept in its [PCR](#pcr). Deferred procedure calls run only while the level is below the dispatch level.

### MMIO

Memory mapped I/O: device registers that a game reads and writes at memory addresses. The GPU block is at `0x7FC80000` and the APU block at `0x7FEA0000`.

### MnK

Mouse and keyboard. The `hid.mnk_*` settings turn them into a gamepad.

### MSVC

Microsoft Visual C++, the compiler that built the game code that ReXGlue recompiles.

### PCR

Processor control region, the kernel data structure of a guest thread, one for each guest thread. Guest code reaches it through register r13.

### PM4

The packet format of the GPU command stream.

### PPC

PowerPC, the CPU architecture of the Xbox 360.

### SDL3

Simple DirectMedia Layer 3: the window, input and audio library behind the `-sdl3` driver modules.

### SEH

Structured exception handling: `__try`, `__except` and `__finally`.

### STFS

Secure Transacted File System: the package format of downloaded content and saves.

### SVOD

Secure Virtual Optical Drive: the package format of a disc installed to storage.

### TLS

Thread local storage.

### TOML

The text format of the manifest and the config file.

### VMX128

The vector instruction set of the Xbox 360 CPU, with 128 vector registers.

### XAM

Xbox Application Manager: the system library of the services that games use, such as input, UI, content, profiles and achievements. `rexxam` implements its exports.

### XBDM

The Xbox 360 debug monitor module, `xbdm.xex`.

### XDBF

The data section of a title executable with achievements, strings and icons.

### XEX

The Xbox 360 executable format.

### XInput

The Windows gamepad API behind `rexhid-xinput`.

### XMA

The Xbox 360 compressed audio format, a variant of Windows Media Audio Professional that the console decodes in hardware.

### XUID

The 64 bit ID of a user profile.

## SDK Terms

### Codegen

The step, run by `rexglue codegen`, that analyzes the PowerPC code of the XEX files in the manifest and writes the C++ that replaces them. See [Codegen pipeline](../codegen/codegen-pipeline.md).

### Console Overlay

The in game overlay that shows the log and takes commands. Its hotkey is the backtick key, set by `ui.bind.console`. See [Overlays and dialogs](../devices/ui.md#overlays-and-dialogs).

### Detour

A function hook that wraps the original function. Its body must call the original on every path, and the original stays callable as `__imp__name`. See [Hooks](../codegen/hooks.md).

### DLL Module

A XEX of the game other than the entrypoint, such as a DLL that the game loads while it runs. Codegen recompiles it as its own binary, named by its DLL file name without the extension. Not an [SDK module](#sdk-module).

### Driver

The host implementation a system runs, such as SDL3 audio or Vulkan graphics.

### Driver Context

What a driver receives from the SDK: an `IDriverContext` when it starts, and the game interfaces when a game attaches. See [Driver Context](../devices/driver-context.md).

### Driver Module

A shared library that provides drivers, such as `rexapu-sdl3`, `rexhid-xinput` or `rexgpu-xenos-vulkan`. A program loads it by name and never links it.

### Entry Point

The address of the function where a binary starts running. Not the entrypoint, which is the main XEX of a project.

### Entrypoint

The main XEX of a project. Every other binary is a DLL module. Not an entry point, which is the address where a binary starts running.

### Function Table

The table in guest memory that maps a guest address to its recompiled host function. See [Function table](../codegen/generated-code.md#function-table).

### Game Root

The folder that holds the entrypoint and the DLLs of a game, set by `[project].game_root` or `title.game_root`.

### Graphics Family

The type of graphics driver a program requires: `kXenos` or `kNone`.

### Guest

The Xbox 360 side: the recompiled code, its memory, threads and paths.

### Headless

The state of the UI system when it has no display: `ui.driver` is `none`, the UI driver reports no display, or the window failed to open. Message boxes only log, and prompts are canceled. See [Headless](../devices/ui.md#headless). The setting `kernel.headless` is separate. It makes the system dialogs of the game return immediately.

### Hook

Host code that replaces or runs inside a recompiled function. A function hook replaces a whole function, a detour calls the original as well, and a mid function hook runs at one instruction. See [Hooks](../codegen/hooks.md).

### Host

The PC side: the operating system, the devices and the native program.

### Idle Pad

A virtual gamepad on user 0 that never presses anything, so a game that waits for a controller starts without one. See [Input](../devices/input.md).

### Interface

A class declared with `REX_DECLARE_INTERFACE` that a module provides and code finds by name and version. See [Interfaces](../app/modules.md#interfaces).

### Jump Table

The branch targets of a `bctr` instruction, which codegen recompiles as a `switch`.

### Kernel Export

A host function that takes the place of a function that a game imports from `xboxkrnl`, XAM or XBDM.

### Kernel Module

One of `xboxkrnl.exe`, `xam.xex` and `xbdm.xex`, which the runtime creates. A kernel export belongs to one of them.

### Layer

In settings, the source that wrote a value: default, title, file, environment, command line, runtime or lock. A value from the same or a higher layer replaces it. See [Layers](../app/settings.md#layers). In the manifest, the shared layer holds the keys that apply to every binary.

### Link

A short name such as `game:` that stands for a device path. See [Links and Mounts](../filesystem/filesystem.md#links-and-mounts).

### Log Category

A `LogCategory`: a named log channel with its own level. See [Logging](../app/logging.md).

### Manifest

The TOML file that lists the binaries `rexglue codegen` recompiles and their settings. See [Project manifest](manifest.md).

### Module

Short for [SDK module](#sdk-module). A XEX of the game is a [DLL module](#dll-module).

### Mount

A device path mapped to a provider: a host folder, a container or a null device. See [Links and Mounts](../filesystem/filesystem.md#links-and-mounts).

### Pin

A reference from `IObjectTable::Get` that keeps a kernel object valid for the length of a call. See [Kernel services](../runtime/kernel.md).

### Present Thread

The thread that shows finished frames in the window. See [Graphics](../devices/graphics.md).

### Provider

The class that serves the files of a mount: `HostProvider` for a host folder and `ContainerProvider` for an STFS, SVOD or disc image. See [Providers](../filesystem/filesystem.md#providers).

### Recipient

The read and write functions that receive the game's accesses to a GPU or APU register block. See [Attaching a recipient](../runtime/memory.md#attaching-a-recipient).

### Register Sharing

A function that continues the work of its caller and reads the register values of the caller instead of its own, such as a handler block of an SEH scope that the compiler emits as a separate function.

### rexglue

The command line tool that creates projects and recompiles the XEX of a game into C++. See [Command line](cli.md).

### SDK Module

An object that implements `rex::IModule`, which `rex::Init` connects and initializes. Most SDK modules live in a shared library that the app loads by name. See [Modules and Interfaces](../app/modules.md). Not a [DLL module](#dll-module) or a [kernel module](#kernel-module), which are parts of the game.

### Settings Class

A class derived from `SettingOwner` that declares settings.

### Settings Overlay

The in game overlay that lists every setting and changes it. Its hotkey is F4, set by `ui.bind.settings`. See [Overlays and dialogs](../devices/ui.md#overlays-and-dialogs).

### Shared Layer

The manifest keys outside any binary table, which apply to every binary. See [Includes](manifest.md#includes).

### Shim

An SDK function that replaces a library function inside the game, such as `memcpy`. A shim is a function hook that the SDK provides. See [Shims](../runtime/shims.md).

### System

One of `IInputSystem`, `IAudioSystem`, `IUISystem` or `IGraphicsSystem`, which connect the game to the host through drivers.

### Title

The Xbox 360 game program that is recompiled and run. A title is the game, in Xbox 360 terms, and SDK names use `Title`.

### Title Workspace

The memory that XAM allocates from on behalf of the game, through `ITitleWorkspace`. See [Title workspace](../runtime/kernel.md#title-workspace).

### Tool Mode

A runtime started without recompiled game code, which tools such as `rexglue` use to read a XEX. It loads the kernel and mounts the game files, but installs no recompiled code.

### UI Thread

The thread that called `App::Init`.

### User

One of the four controller slots, 0 to 3, that a game reads input from.

### Write Watch

A one shot watch on a physical page that calls its listeners on the next write, including host writes through `GuestMemory`.

### Xenos

The GPU of the Xbox 360. A Xenos graphics driver emulates it on Direct3D 12 or Vulkan, and the `gpu.xenos.*` settings apply to both.
