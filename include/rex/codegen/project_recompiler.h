/**
 * @file        codegen/project_recompiler.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <string>
#include <vector>

#include <rex/codegen/limits.h>
#include <rex/codegen/manifest.h>
#include <rex/codegen/progress_reporter.h>
#include <rex/result.h>

namespace rex::codegen {

/** Settings for 1 ProjectRecompiler run. */
struct ProjectRecompilerOptions {
  std::vector<std::string> targets;     /**< Empty selects all */
  bool force = false;                   /**< Rewrite outputs whose content is unchanged */
  bool enableExceptionHandlers = false; /**< Generate SEH exception handler wrappers */
  ProgressReporter* reporter = nullptr; /**< Receives progress events, null for none */

  /** Bypasses the stamp gate and regenerates every targeted module. */
  bool ignoreStamp = false;

  /** Analysis limits applied to every targeted module and folded into the fingerprint. */
  Limits limits;

  /**
   * Folded into the input fingerprint. Must be stable across commits such as "0.10.0-dev" or
   * every SDK rebuild forces a full re-analysis.
   */
  std::string sdkVersion;
};

/**
 * Runs code generation for every targeted module of a manifest.
 */
class ProjectRecompiler {
 public:
  /**
   * Creates a recompiler for a manifest.
   *
   * @param manifest Manifest to process, moved into the recompiler
   */
  explicit ProjectRecompiler(ManifestConfig manifest);
  [[nodiscard]] Result<void> Run(const ProjectRecompilerOptions& opts);

  /**
   * Aggregated basenames of files removed across all modules during the most
   * recent Run() call. Empty until Run() completes successfully.
   */
  const std::vector<std::string>& deletedFiles() const { return deletedFiles_; }

  /**
   * Aggregated basenames of files written across all modules during the most
   * recent Run() call. Empty until Run() completes successfully.
   */
  const std::vector<std::string>& writtenFiles() const { return writtenFiles_; }

  /** Returns the basenames already up to date across all modules in the last Run(). */
  const std::vector<std::string>& unchangedFiles() const { return unchangedFiles_; }

  /** Returns the targets skipped in the last Run() because their stamp still matched. */
  const std::vector<std::string>& skippedModules() const { return skippedModules_; }

 private:
  ManifestConfig manifest_;
  std::vector<std::string> deletedFiles_;
  std::vector<std::string> writtenFiles_;
  std::vector<std::string> unchangedFiles_;
  std::vector<std::string> skippedModules_;
};

}  // namespace rex::codegen
