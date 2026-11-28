*[APC]: Asynchronous procedure call: a function queued to run on a guest thread.
*[APU]: The Xbox 360 audio hardware and its register block at 0x7FEA0000.
*[CPU]: The central processor, here the PowerPC CPU of the Xbox 360.
*[DLL]: Dynamic link library. A DLL that a game loads is recompiled as a DLL module.
*[DPC]: Deferred procedure call: work the kernel queues to run on the Kernel Dispatch thread.
*[EDRAM]: The embedded memory of the Xenos GPU that render targets are drawn into.
*[GDFX]: Game Disc File System: the filesystem of an Xbox 360 disc image.
*[GPU]: The graphics processor, here the Xenos GPU of the Xbox 360.
*[IRQL]: Interrupt request level, the kernel priority level that a guest thread runs at, kept in its PCR. Deferred procedure calls run only while the level is below the dispatch level.
*[LFE]: Low frequency effects: the subwoofer channel of a surround stream.
*[MMIO]: Memory mapped I/O: device registers that a game reads and writes at memory addresses. The GPU block is at 0x7FC80000 and the APU block at 0x7FEA0000.
*[MSVC]: Microsoft Visual C++, the compiler that built the game code that ReXGlue recompiles.
*[PCR]: Processor control region, the kernel data structure of a guest thread, one for each guest thread. Guest code reaches it through register r13.
*[PM4]: The packet format of the GPU command stream.
*[PPC]: PowerPC, the CPU architecture of the Xbox 360.
*[SDL3]: Simple DirectMedia Layer 3: the window, input and audio library behind the -sdl3 driver modules.
*[SEH]: Structured exception handling: __try, __except and __finally.
*[STFS]: Secure Transacted File System: the package format of downloaded content and saves.
*[SVOD]: Secure Virtual Optical Drive: the package format of a disc installed to storage.
*[TLS]: Thread local storage.
*[TOML]: The text format of the manifest and the config file.
*[XAM]: Xbox Application Manager: the system library of the services that games use, such as input, UI, content, profiles and achievements. rexxam implements its exports.
*[XBDM]: The Xbox 360 debug monitor module, xbdm.xex.
*[XDBF]: The data section of a title executable with achievements, strings and icons.
*[XEX]: The Xbox 360 executable format.
*[XMA]: The Xbox 360 compressed audio format, a variant of Windows Media Audio Professional that the console decodes in hardware.
*[XUID]: The 64 bit ID of a user profile.
*[FFmpeg]: The media library that decodes XMA audio.
*[ImGui]: The immediate mode GUI library behind the overlays.
*[spdlog]: The logging library behind the SDK log.
*[XInput]: The Windows gamepad API behind rexhid-xinput.
*[Xenos]: The GPU of the Xbox 360. A Xenos graphics driver emulates it on Direct3D 12 or Vulkan, and the gpu.xenos.* settings apply to both.
*[XThread]: The kernel thread object of a guest thread.
*[driver module]: A shared library that provides drivers, such as rexapu-sdl3, rexhid-xinput or rexgpu-xenos-vulkan. A program loads it by name and never links it.
*[DLL module]: A XEX of the game other than the entrypoint, such as a DLL that the game loads while it runs. Codegen recompiles it as its own binary, named by its DLL file name without the extension. Not an SDK module.
*[function table]: The table in guest memory that maps a guest address to its recompiled host function.
*[game root]: The folder that holds the entrypoint and the DLLs of a game, set by [project].game_root or title.game_root.
*[graphics family]: The type of graphics driver a program requires: kXenos or kNone.
*[idle pad]: A virtual gamepad on user 0 that never presses anything, so a game that waits for a controller starts without one.
*[jump table]: The branch targets of a bctr instruction, which codegen recompiles as a switch.
*[kernel export]: A host function that takes the place of a function that a game imports from xboxkrnl, XAM or XBDM.
*[kernel services]: The objects, threads, files and memory that the runtime provides to a game through the kernel exports.
*[present thread]: The thread that shows finished frames in the window.
*[register sharing]: A function that continues the work of its caller and reads the register values of the caller instead of its own, such as a handler block of an SEH scope that the compiler emits as a separate function.
*[shared layer]: The manifest keys outside any binary table, which apply to every binary.
*[driver context]: What a driver receives from the SDK: an IDriverContext when it starts, and the game interfaces when a game attaches.
*[title workspace]: The memory that XAM allocates from on behalf of the game, through ITitleWorkspace.
*[tool mode]: A runtime started without recompiled game code, which tools such as rexglue use to read a XEX. It loads the kernel and mounts the game files, but installs no recompiled code.
*[write watch]: A one shot watch on a physical page that calls its listeners on the next write, including host writes through GuestMemory.
