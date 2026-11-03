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
#include <rex/utility.h>

namespace rex::codegen {

/** Identifying fields from a loaded XEX. */
struct BinaryIdentity {
  std::string_view name;      /**< Filename of the input XEX. */
  u32 title_id = 0;           /**< XEXOptExecutionInfo.title_id. */
  u32 media_id = 0;           /**< XEXOptExecutionInfo.media_id. */
  u32 version_major = 0;      /**< XEXVersion.major, 4 bits. */
  u32 version_minor = 0;      /**< XEXVersion.minor, 4 bits. */
  u32 version_build = 0;      /**< XEXVersion.build, 16 bits. */
  u32 version_qfe = 0;        /**< XEXVersion.qfe, 8 bits. */
  u32 pe_time_date_stamp = 0; /**< IMAGE_FILE_HEADER.TimeDateStamp in Unix epoch seconds. */
};

/**
 * Optional progress callback the codegen pipeline invokes at module and phase boundaries. Callers
 * that show no progress, such as tests, pass null. All methods run on the pipeline thread.
 */
class IProgressReporter : public NonCopyable {
 public:
  virtual ~IProgressReporter() = default;

  /** Reports identifying info for an input binary, called after each XEX is loaded. */
  virtual void BinaryLoaded(const BinaryIdentity& info) = 0;

  /**
   * Called when a module's analysis and write cycle starts.
   *
   * @param index The 0 based position of the module.
   */
  virtual void ModuleStarted(std::string_view name, std::size_t index, std::size_t total) = 0;

  /** Called when a named phase within the current module starts. */
  virtual void ModulePhaseStarted(std::string_view name) = 0;

  /** Called when the current module finishes successfully. */
  virtual void ModuleFinished(std::chrono::milliseconds elapsed) = 0;

  /**
   * Called when a project level emit phase starts, outside any module.
   *
   * @param name The phase name, such as "module_registry".
   */
  virtual void ProjectPhaseStarted(std::string_view name) = 0;

  /** Called when a project level emit phase finishes. */
  virtual void ProjectPhaseFinished() = 0;
};

}  // namespace rex::codegen
