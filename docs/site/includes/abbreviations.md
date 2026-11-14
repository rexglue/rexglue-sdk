*[APC]: Asynchronous procedure call: a function queued to run on a guest thread.
*[APU]: The Xbox 360 audio hardware and its register block at 0x7FEA0000.
*[CPU]: The central processor, here the PowerPC CPU of the Xbox 360.
*[DLL]: Dynamic link library. A title DLL is recompiled as a DLL module.
*[DPC]: Deferred procedure call: work the kernel queues to run on the Kernel Dispatch thread.
*[EDRAM]: The embedded memory of the Xenos GPU that render targets are drawn into.
*[GDFX]: Game Disc File System: the filesystem of an Xbox 360 disc image.
*[GPU]: The graphics processor, here the Xenos GPU of the Xbox 360.
*[IRQL]: Interrupt request level of the kernel.
*[LFE]: Low frequency effects: the subwoofer channel of a surround stream.
*[MMIO]: Memory mapped I/O: device registers that a title reads and writes at memory addresses.
*[MSVC]: Microsoft Visual C++, the compiler that built the title.
*[PCR]: Processor control region: the kernel block for each guest thread.
*[PM4]: The packet format of the GPU command stream.
*[PPC]: PowerPC, the CPU architecture of the Xbox 360.
*[SDL3]: Simple DirectMedia Layer 3: the window, input and audio library behind the sdl3 driver modules.
*[SEH]: Structured exception handling: __try, __except and __finally.
*[STFS]: Secure Transacted File System: the package format of downloaded content and saves.
*[SVOD]: Secure Virtual Optical Drive: the package format of a disc installed to storage.
*[TLS]: Thread local storage.
*[TOML]: The text format of the manifest and the config file.
*[XAM]: Xbox Application Manager: the system library of title services such as input, UI, content, profiles and achievements.
*[XBDM]: The Xbox 360 debug monitor module, xbdm.xex.
*[XDBF]: The data section of a title executable with achievements, strings and icons.
*[XEX]: The Xbox 360 executable format.
*[XMA]: The Xbox 360 compressed audio format, a variant of Windows Media Audio Professional that the console decodes in hardware.
*[XUID]: The user ID of a profile.
*[FFmpeg]: The media library that decodes XMA audio.
*[ImGui]: The immediate mode GUI library behind the overlays.
*[spdlog]: The logging library behind the SDK log.
*[XInput]: The Windows gamepad API behind the rexhid-xinput driver module.
*[Xenos]: The GPU of the Xbox 360.
*[XThread]: The kernel thread object of a guest thread.
*[driver module]: A shared library that provides drivers, named rex<system>-<name>, or rexgpu-<family>-<name> for graphics. A program loads it by name and never links it.
*[DLL module]: A binary other than the entrypoint, recompiled from a title DLL.
*[function table]: The table that maps a guest address to its recompiled host function.
*[game root]: The folder that holds the entrypoint XEX and the DLLs of the title.
*[graphics family]: The kind of graphics driver a program requires: Xenos, or none.
*[idle pad]: A virtual gamepad on user 0 that never presses anything.
*[jump table]: A table of branch targets that a bctr instruction indexes, recompiled as a switch.
*[kernel export]: A host function that a title import of xboxkrnl, XAM or XBDM reaches.
*[kernel services]: The objects, threads, files and memory that the runtime provides to a title through the kernel exports.
*[present thread]: The thread on which the graphics system makes every driver call and presents frames.
*[register sharing]: A function, such as an SEH funclet, that uses the registers of its caller instead of its own.
*[shared layer]: The manifest keys outside any binary table, which apply to every binary.
*[driver context]: The interfaces a graphics or UI driver receives: physical memory, write watches, interrupts and achievements.
*[title workspace]: The memory that XAM allocates from on behalf of the title, reached through ITitleWorkspace.
*[tool mode]: A runtime that loads an image with no code, so it runs kernel services without a title.
*[write watch]: A one shot watch on a physical page that calls its listeners on the next write, host writes through GuestMemory included.
