/**
 * @file        tests/unit/gpu/fake_driver_context.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/driver_context.h>
#include <rex/logging/logger.h>
#include <rex/settings.h>
#include <rex/utility.h>

namespace rex::test {

/** Serves the global settings and logger. */
class FakeDriverContext final : public IDriverContext {
 public:
  ISettings& GetSettings() override { return REX_SETTINGS(); }
  ILogger& GetLogger() override { return REX_LOGGER(); }
  Result<void> RunOnUIThread(MoveOnlyFunction<void()> fn) override {
    fn();
    return Ok();
  }
  void PostToUIThread(MoveOnlyFunction<void()> fn) override { fn(); }
};

}  // namespace rex::test
