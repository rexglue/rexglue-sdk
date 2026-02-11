/**
 * @file        codegen/recompile.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/codegen/recompile.h>
#include <rex/codegen/recompiler.h>
#include <rex/logging.h>
#include <filesystem>

 // TODO(tomc): this can probably be absorbed into the larger codegen workflow. 
 // This was a compromise with the devil while doing a larger refactor of the codegen flow, 
 // but ideally we should be able to just call recompiler.recompile() directly from the main workflow 
 // and have it handle the force flag and error checking internally. 
 // For now this is a separate function that can be called from the CLI with --force to bypass validation errors and generate output anyway.

namespace rex::codegen {

Result<void> Recompile(CodegenContext& ctx, bool force) {
    REXLOG_INFO("Recompile: starting code generation...");

    // Check for validation errors (unless force is set)
    if (ctx.errors.HasErrors() && !force) {
        REXLOG_ERROR("Code generation blocked: {} validation errors. Use --force to override.",
                    ctx.errors.Count());
        return Err(ErrorCategory::Validation,
                   "Code generation blocked due to validation errors. Use --force to override.");
    }

    // Set up output directory
    auto& config = ctx.Config();
    std::filesystem::path output_path = ctx.configDir() / config.outDirectoryPath;
    REXLOG_INFO("Output path: {}", output_path.string());
    std::filesystem::create_directories(output_path);

    // Clean up old generated files
    std::string prefix = config.projectName + "_";
    for (const auto& entry : std::filesystem::directory_iterator(output_path)) {
        auto ext = entry.path().extension();
        if (ext == ".cpp" || ext == ".h" || ext == ".cmake") {
            std::string filename = entry.path().filename().string();
            if (filename == "sources.cmake" ||
                filename.starts_with(prefix) ||
                filename.starts_with("ppc_recomp") || filename.starts_with("ppc_func_mapping") ||
                filename.starts_with("function_table_init") || filename.starts_with("ppc_config")) {
                std::filesystem::remove(entry.path());
            }
        }
    }

    REXLOG_INFO("Old files cleaned up, starting code generation...");

    Recompiler recompiler;
    recompiler.ctx_ = &ctx;

    if (!recompiler.recompile(force)) {
        return Err(ErrorCategory::Validation,
                   "Code generation failed.");
    }

    REXLOG_INFO("Code generation complete");
    return Ok();
}

} // namespace rex::codegen
