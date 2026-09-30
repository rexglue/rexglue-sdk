/**
 * @file        rex/platform/console.h
 * @brief       Platform-agnostic terminal capability queries.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstdio>

namespace rex::platform {

// Returns true if the stream is attached to a terminal. False for nullptr or
// any non-terminal stream (pipes, files, redirected output).
bool IsTerminal(FILE* stream);

// Enables ANSI escape sequence interpretation on the stream. No-op on POSIX.
// Returns true if escapes are interpreted on return (including the case where
// they already were); false only when the Windows console probe fails.
bool EnableANSIEscapes(FILE* stream);

// Switches the process console output to UTF-8. Process-wide on Windows; no
// per-stream component, hence the no-arg signature. No-op on POSIX.
void SetUTF8OutputCodePage();

}  // namespace rex::platform
