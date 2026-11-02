/**
 * @file        kernel/xboxkrnl/video_export.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/platform.h>

namespace rex {
namespace kernel {
class TitleRuntime;
}  // namespace kernel
}  // namespace rex

namespace rex::kernel {
struct ModuleObject;
}  // namespace rex::kernel

namespace rex::kernel::xboxkrnl {

void RegisterVideoExports(kernel::ModuleObject& module, TitleRuntime& runtime);

}  // namespace rex::kernel::xboxkrnl
