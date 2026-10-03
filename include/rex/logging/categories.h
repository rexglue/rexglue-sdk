/**
 * @file        logging/categories.h
 * @brief       The SDK log categories
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/logging/macros.h>

REXLOG_CATEGORY(core, "Core library, module loading, cvars and the app lifecycle")
REXLOG_CATEGORY(filesystem, "Mount table, links and file providers")
REXLOG_CATEGORY(kernel, "Kernel exports of xboxkrnl and xam")
REXLOG_CATEGORY(system, "Kernel state, guest threads, guest memory and modules")
REXLOG_CATEGORY(cpu, "Function dispatch and the guest CPU state")
REXLOG_CATEGORY(title, "Debug output the title writes through DbgPrint and OutputDebugString")
REXLOG_CATEGORY(apu, "APU emulation, XMA decoding and the audio output modules")
REXLOG_CATEGORY(hid, "Pads, keyboard and pointer, and the hid modules")
REXLOG_CATEGORY(ui, "Windows, presenters, overlays and ImGui")
REXLOG_CATEGORY(console, "Console command output")
REXLOG_CATEGORY(gpu, "Xenos emulation, shader translation and the host graphics paths")
REXLOG_CATEGORY(codegen, "Recompiler analysis and code generation")
REXLOG_CATEGORY(rexglue, "The rexglue command line tool")
