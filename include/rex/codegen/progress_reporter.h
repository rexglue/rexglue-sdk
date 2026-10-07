/**
 * @file        rex/codegen/progress_reporter.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace rex::codegen {

/** Identifying fields from a loaded XEX, surfaced before recompilation work starts. */
struct BinaryInfo {
  std::string_view name;            ///< Display name (filename of the input XEX).
  uint32_t title_id = 0;            ///< xex2_opt_execution_info.title_id
  uint32_t media_id = 0;            ///< xex2_opt_execution_info.media_id
  uint32_t version_major = 0;       ///< xex2_version.major (4 bits)
  uint32_t version_minor = 0;       ///< xex2_version.minor (4 bits)
  uint32_t version_build = 0;       ///< xex2_version.build (16 bits)
  uint32_t version_qfe = 0;         ///< xex2_version.qfe   (8 bits)
  uint32_t pe_time_date_stamp = 0;  ///< IMAGE_FILE_HEADER.TimeDateStamp (Unix epoch seconds)
};

/**
 * Optional progress callback invoked by the codegen pipeline at module
 * and phase boundaries. CLI consumers implement this to drive a progress
 * view; library-internal callers (tests, headless callers) can pass
 * nullptr to opt out.
 *
 * All methods are called from the thread that drove the pipeline; no
 * cross-thread synchronization is implied.
 */
class ProgressReporter {
 public:
  virtual ~ProgressReporter() = default;

  /** Identifying info for an input binary, emitted right after each XEX is loaded. */
  virtual void binaryInfo(const BinaryInfo&) {}

  /** A new module's analysis+write cycle is starting. `index` is 0-based. */
  virtual void moduleStarted(std::string_view name, std::size_t index, std::size_t total) = 0;

  /** A named phase within the current module is starting. */
  virtual void phaseChanged(std::string_view name) = 0;

  /** The current module finished successfully. */
  virtual void moduleFinished(std::chrono::milliseconds elapsed) = 0;

  /** Project-level (non-module) emit phase started, e.g. "module_registry". */
  virtual void projectPhaseStarted(std::string_view name) = 0;

  /** Project-level emit phase finished. */
  virtual void projectPhaseFinished() = 0;
};

}  // namespace rex::codegen
