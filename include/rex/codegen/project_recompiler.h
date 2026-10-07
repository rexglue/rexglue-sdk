/**
 * @file        rex/codegen/project_recompiler.h
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

struct ProjectRecompilerOptions {
  std::vector<std::string> targets;  // empty = all
  bool force = false;
  bool enableExceptionHandlers = false;
  ProgressReporter* reporter = nullptr;

  /// Bypass the stamp gate and regenerate every targeted module.
  bool ignoreStamp = false;

  /// Analysis limits applied to every targeted module and folded into the fingerprint.
  Limits limits;

  /// Folded into the input fingerprint. Must be stable across commits
  /// ("0.10.0-dev") or every SDK rebuild forces a full re-analysis.
  std::string sdkVersion;
};

class ProjectRecompiler {
 public:
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

  /// Basenames already up to date across all modules in the last Run().
  const std::vector<std::string>& unchangedFiles() const { return unchangedFiles_; }

  /// Targets skipped in the last Run() because their stamp still matched.
  const std::vector<std::string>& skippedModules() const { return skippedModules_; }

 private:
  ManifestConfig manifest_;
  std::vector<std::string> deletedFiles_;
  std::vector<std::string> writtenFiles_;
  std::vector<std::string> unchangedFiles_;
  std::vector<std::string> skippedModules_;
};

}  // namespace rex::codegen
