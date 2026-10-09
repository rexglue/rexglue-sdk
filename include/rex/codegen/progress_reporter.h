/**
 * @file        codegen/progress_reporter.h
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

#include <rex/types.h>

namespace rex::codegen {

/** Identifying fields from a loaded XEX, surfaced before recompilation work starts. */
struct BinaryInfo {
  std::string_view name; /**< Filename of the input XEX */
  u32 title_id = 0;      /**< xex2_opt_execution_info.title_id */
  u32 media_id = 0;      /**< xex2_opt_execution_info.media_id */
  u32 version_major = 0; /**< xex2_version.major, 4 bits */
  u32 version_minor = 0; /**< xex2_version.minor, 4 bits */
  u32 version_build = 0; /**< xex2_version.build, 16 bits */
  u32 version_qfe = 0;   /**< xex2_version.qfe, 8 bits */
  /** IMAGE_FILE_HEADER.TimeDateStamp in Unix epoch seconds. */
  u32 pe_time_date_stamp = 0;
};

/**
 * Optional progress callback invoked by the codegen pipeline at module
 * and phase boundaries. CLI consumers implement it to update a progress
 * view. Callers without a view, such as tests, pass nullptr.
 *
 * All methods are called from the thread that drove the pipeline, and no
 * cross-thread synchronization is implied.
 */
class ProgressReporter {
 public:
  virtual ~ProgressReporter() = default;

  /** Identifying info for an input binary, emitted right after each XEX is loaded. */
  virtual void binaryInfo(const BinaryInfo&) {}

  /**
   * Called when a module's analysis and write cycle starts.
   *
   * @param name The module name.
   * @param index The 0 based position of the module.
   * @param total The number of modules in the run.
   */
  virtual void moduleStarted(std::string_view name, std::size_t index, std::size_t total) = 0;

  /**
   * Called when a named phase within the current module starts.
   *
   * @param name The phase name.
   */
  virtual void phaseChanged(std::string_view name) = 0;

  /**
   * Called when the current module finishes successfully.
   *
   * @param elapsed Time the module took from start to finish.
   */
  virtual void moduleFinished(std::chrono::milliseconds elapsed) = 0;

  /**
   * Called when a project level emit phase starts, outside any module.
   *
   * @param name The phase name, such as "module_registry".
   */
  virtual void projectPhaseStarted(std::string_view name) = 0;

  /** Project-level emit phase finished. */
  virtual void projectPhaseFinished() = 0;
};

}  // namespace rex::codegen
