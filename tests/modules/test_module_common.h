/**
 * @file        tests/modules/test_module_common.h
 * @brief       Event sink shared by the test modules
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/macros.h>

/**
 * Signature of the sink a test installs to observe module events.
 * The first argument is the module name, the second the event
 * ("constructor", "connect", "init", "shutdown", "disconnect").
 */
using RexModTestSink = void (*)(const char* module, const char* event);

/**
 * Defines the sink storage, the sink setter export and a Record helper for 1 test module.
 * Usage at namespace scope in each test module .cpp: REXMOD_TEST_SINK()
 */
#define REXMOD_TEST_SINK()                                            \
  static RexModTestSink g_rexmod_sink = nullptr;                      \
  extern "C" REX_API void rexmod_test_set_sink(RexModTestSink sink) { \
    g_rexmod_sink = sink;                                             \
  }                                                                   \
  static void Record(const char* module, const char* event) {         \
    if (g_rexmod_sink)                                                \
      g_rexmod_sink(module, event);                                   \
  }
