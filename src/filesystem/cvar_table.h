/**
 * @file        filesystem/cvar_table.h
 * @brief       filesystem module cvars
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/cvar.h>

namespace rex::filesystem::cvars {

REXCVAR_STRING(fs, dump_source, "", "Specifies the file to dump from");
REXCVAR_STRING(fs, dump_path, "", "Specifies the directory to dump files to");
REXCVAR_TABLE(kFilesystemCvars, dump_source, dump_path);

}  // namespace rex::filesystem::cvars
