/**
 * @file        platform/console.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstdio>

#include <rex/macros.h>

namespace rex::platform {

/**
 * Reports whether a stream is attached to a terminal.
 *
 * @param stream Stream to test, may be null.
 * @return True for a terminal. False for null, pipes, files and redirected output.
 */
REX_API bool IsTerminal(FILE* stream);

/**
 * Turns on ANSI escape sequence handling for a stream. Does nothing on POSIX.
 *
 * @param stream Stream to configure, may be null.
 * @return True when escapes are handled on return, already on included. False for null or
 *         when the Windows console mode query or update fails.
 */
REX_API bool EnableANSIEscapes(FILE* stream);

/** Switches the process console output code page to UTF-8. Does nothing on POSIX. */
REX_API void SetUTF8OutputCodePage();

}  // namespace rex::platform
