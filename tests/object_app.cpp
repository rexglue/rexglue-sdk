/**
 * @file        tests/object_app.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "object_app.h"

#include "apu/audio_module.h"
#include "filesystem/filesystem_module.h"
#include "hid/input_module.h"
#include "kernel/rexkernel_module.h"
#include "xam/xam_module.h"
#include "ui/ui_module.h"

namespace rex::test {

struct ObjectModules::Impl {
  filesystem::FileSystemModule filesystem_module;
  hid::InputModule input_module;
  apu::AudioModule audio_module;
  ui::UIModule ui_module;
  xam::XAMModule xam_module;
  kernel::KernelModule kernel_module;
};

ObjectModules::ObjectModules() : impl_(std::make_unique<Impl>()) {}

ObjectModules::~ObjectModules() = default;

std::vector<IModule*> ObjectModules::GetAll() {
  return {&impl_->filesystem_module, &impl_->input_module, &impl_->audio_module,
          &impl_->ui_module,         &impl_->xam_module,   &impl_->kernel_module};
}

}  // namespace rex::test
