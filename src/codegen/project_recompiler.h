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

#include "codegen_context.h"
#include "manifest.h"
#include "progress_reporter.h"
#include <rex/result.h>

namespace rex::codegen {

class ToolApp;

/** Settings for one ProjectRecompiler run. */
struct ProjectRecompilerOptions {
  std::vector<std::string> targets; /**< Empty selects all. */
  bool force = false; /**< Skips the stamp check and continues past validation errors. */
  IProgressReporter* reporter = nullptr; /**< Receives progress events, null for none. */
  bool ignore_stamp = false; /**< Skips the stamp check and regenerates every targeted module. */
  Limits limits; /**< Analysis limits for every targeted module, folded into the fingerprint. */

  /**
   * Folded into the input fingerprint. Keep it stable across commits, such as "0.10.0-dev", or
   * every SDK rebuild forces a full re-analysis.
   */
  std::string sdk_version;

  ToolApp* tools = nullptr; /**< App that opens the tool runtime for the run. Required. */
};

/** Runs code generation for every targeted module of a manifest. */
class ProjectRecompiler {
 public:
  /**
   * Creates a recompiler for a manifest.
   *
   * @param manifest Moved into the recompiler.
   */
  explicit ProjectRecompiler(ManifestConfig manifest);

  /**
   * Generates code for the targeted modules.
   *
   * @param opts Targets, force flag and progress reporter for this run.
   */
  [[nodiscard]] Result<void> Run(const ProjectRecompilerOptions& opts);

  /** Returns the basenames of files removed across all modules by the last successful Run(). */
  const std::vector<std::string>& GetDeletedFiles() const { return deleted_files_; }

  /** Returns the basenames of files written across all modules by the last successful Run(). */
  const std::vector<std::string>& GetWrittenFiles() const { return written_files_; }

  /** Returns the basenames of files left unchanged across all modules by the last Run(). */
  const std::vector<std::string>& GetUnchangedFiles() const { return unchanged_files_; }

  /** Returns the targets skipped in the last Run() because their stamp matched. */
  const std::vector<std::string>& GetSkippedModules() const { return skipped_modules_; }

 private:
  ManifestConfig manifest_;
  std::vector<std::string> deleted_files_;
  std::vector<std::string> written_files_;
  std::vector<std::string> unchanged_files_;
  std::vector<std::string> skipped_modules_;
};

}  // namespace rex::codegen
