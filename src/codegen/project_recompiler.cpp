/**
 * @file        codegen/project_recompiler.cpp
 * @brief       Project-level recompiler driving manifest-based multi-binary codegen
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/codegen/project_recompiler.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <span>
#include <cstring>
#include <cstdlib>
#include <unordered_map>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <nlohmann/json.hpp>

#include <rex/codegen/analyze.h>
#include <rex/codegen/binary_view.h>
#include <rex/codegen/codegen.h>
#include <rex/codegen/codegen_context.h>
#include <rex/codegen/codegen_writer.h>
#include <rex/codegen/config.h>
#include <rex/codegen/output_stamp.h>
#include <rex/codegen/progress_reporter.h>
#include <rex/codegen/template_registry.h>
#include <rex/kernel/init.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/user_module.h>
#include <rex/system/xex_module.h>

#include <chrono>

#include "codegen_flags.h"
#include "codegen_logging.h"
#include "file_io.h"
#include "template_registry_internal.h"

namespace rex::codegen {

namespace {

std::string DeriveTargetNameFromFilePath(const std::string& file_path) {
  std::filesystem::path p(file_path);
  std::string name = p.stem().string();
  std::replace(name.begin(), name.end(), '.', '_');
  std::replace(name.begin(), name.end(), ' ', '_');
  return name;
}

std::vector<std::filesystem::path> CollectModuleInputs(const RecompilerConfig& cfg,
                                                       const std::filesystem::path& configDir,
                                                       const std::filesystem::path& manifestPath) {
  namespace fs = std::filesystem;

  std::vector<fs::path> inputs;
  inputs.push_back(configDir / cfg.filePath);
  inputs.push_back(manifestPath);
  for (const auto& loaded : cfg.loadedFiles) {
    inputs.emplace_back(loaded);
  }
  if (!cfg.templateDir.empty()) {
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(configDir / cfg.templateDir, ec)) {
      if (entry.is_regular_file())
        inputs.push_back(entry.path());
    }
  }
  std::sort(inputs.begin(), inputs.end());
  return inputs;
}

std::string FingerprintModule(const RecompilerConfig& cfg,
                              std::span<const std::filesystem::path> inputs,
                              std::string_view sdkVersion) {
  std::vector<std::string> flags{
      fmt::format("templates={}", EmbeddedTemplatesHash()),
      fmt::format("generate_exception_handlers={}", cfg.generateExceptionHandlers),
      fmt::format("max_jump_extension={}", cfg.maxJumpExtension),
      fmt::format("data_region_threshold={}", cfg.dataRegionThreshold),
      fmt::format("max_file_size_bytes={}", REXCVAR_GET(max_file_size_bytes)),
      fmt::format("max_discovery_iterations={}", REXCVAR_GET(max_discovery_iterations)),
      fmt::format("max_vtable_iterations={}", REXCVAR_GET(max_vtable_iterations)),
      fmt::format("max_resolve_iterations={}", REXCVAR_GET(max_resolve_iterations)),
      fmt::format("max_eh_states={}", REXCVAR_GET(max_eh_states)),
      fmt::format("max_eh_try_blocks={}", REXCVAR_GET(max_eh_try_blocks)),
      fmt::format("max_eh_ip_map_entries={}", REXCVAR_GET(max_eh_ip_map_entries)),
      fmt::format("max_seh_scope_entries={}", REXCVAR_GET(max_seh_scope_entries)),
      fmt::format("backward_scan_limit={}", REXCVAR_GET(backward_scan_limit)),
      fmt::format("max_jump_table_entries={}", REXCVAR_GET(max_jump_table_entries)),
      fmt::format("max_blocks_per_function={}", REXCVAR_GET(max_blocks_per_function)),
  };

  return ComputeInputFingerprint(inputs, sdkVersion, flags);
}

void ReportBinaryInfo(ProgressReporter* reporter, std::string_view display_name,
                      const rex::runtime::XexModule& xex) {
  if (!reporter)
    return;
  BinaryInfo info{};
  info.name = display_name;
  info.pe_time_date_stamp = xex.pe_time_date_stamp();
  if (auto* exec = xex.opt_execution_info()) {
    info.title_id = exec->title_id;
    info.media_id = exec->media_id;
    auto version = exec->version();
    info.version_major = version.major;
    info.version_minor = version.minor;
    info.version_build = version.build;
    info.version_qfe = version.qfe;
  }
  reporter->binaryInfo(info);
}

}  // namespace

ProjectRecompiler::ProjectRecompiler(ManifestConfig manifest) : manifest_(std::move(manifest)) {}

Result<void> ProjectRecompiler::Run(const ProjectRecompilerOptions& opts) {
  namespace fs = std::filesystem;

  deletedFiles_.clear();
  writtenFiles_.clear();
  unchangedFiles_.clear();

  if (manifest_.modules.empty()) {
    REXCODEGEN_TRACE("Recompiling '{}' (entrypoint)", manifest_.projectName);
  } else {
    REXCODEGEN_TRACE("Recompiling '{}' (entrypoint + {} DLL{})", manifest_.projectName,
                     manifest_.modules.size(), manifest_.modules.size() == 1 ? "" : "s");
  }

  struct ModuleEntry {
    std::string targetName;
    std::string guestPath;
    bool isDll;
    RecompilerConfig config;
  };

  std::vector<ModuleEntry> allModules;
  allModules.push_back({DeriveTargetNameFromFilePath(manifest_.entrypoint.recompiler.filePath), "",
                        false, std::move(manifest_.entrypoint.recompiler)});
  for (auto& mod : manifest_.modules) {
    allModules.push_back({DeriveTargetNameFromFilePath(mod.recompiler.filePath), mod.guestPath,
                          true, std::move(mod.recompiler)});
  }

  if (!opts.targets.empty()) {
    std::vector<std::string> unknown;
    for (const auto& t : opts.targets) {
      bool match = std::any_of(allModules.begin() + 1, allModules.end(),
                               [&t](const ModuleEntry& m) { return m.targetName == t; });
      if (!match) {
        unknown.push_back(t);
      }
    }
    if (!unknown.empty()) {
      std::string known;
      for (size_t i = 1; i < allModules.size(); ++i) {
        if (i > 1)
          known += ", ";
        known += allModules[i].targetName;
      }
      std::string list;
      for (size_t i = 0; i < unknown.size(); ++i) {
        if (i > 0)
          list += ", ";
        list += unknown[i];
      }
      return Err<void>(ErrorCategory::Config,
                       fmt::format("Unknown --target value(s): {}. Known DLL targets: {}", list,
                                   known.empty() ? "(none)" : known));
    }
  }

  std::vector<ModuleEntry> targeted;
  targeted.push_back(std::move(allModules[0]));
  for (size_t i = 1; i < allModules.size(); ++i) {
    if (opts.targets.empty() || std::find(opts.targets.begin(), opts.targets.end(),
                                          allModules[i].targetName) != opts.targets.end()) {
      targeted.push_back(std::move(allModules[i]));
    }
  }

  // Two binaries sharing an out_directory_path would clobber each other's
  // sources.cmake on emit (the writer's cleanup sweep is unprefixed for
  // that file).
  std::unordered_map<std::string, std::string> outDirOwner;
  for (const auto& m : targeted) {
    const auto& outDir = m.config.outDirectoryPath;
    if (outDir.empty())
      continue;
    auto [it, inserted] = outDirOwner.emplace(outDir, m.targetName);
    if (!inserted) {
      return Err<void>(
          ErrorCategory::Validation,
          fmt::format("out_directory_path '{}' is shared by '{}' and '{}'; each binary "
                      "needs its own output directory",
                      outDir, it->second, m.targetName));
    }
  }

  const auto& entryConfig = targeted[0].config;
  auto configDir = manifest_.manifestDir;
  fs::path entryXexPath = configDir / entryConfig.filePath;
  if (!fs::exists(entryXexPath)) {
    return Err<void>(ErrorCategory::IO,
                     fmt::format("Entrypoint XEX not found: {}", entryXexPath.string()));
  }
  entryXexPath = fs::canonical(entryXexPath);

  // gameRoot anchors VFS root and DLL guest_path derivation. Honor the
  // manifest override if set; otherwise default to the entrypoint's parent.
  fs::path gameRoot;
  if (manifest_.gameRoot && !manifest_.gameRoot->empty()) {
    fs::path resolved = configDir / *manifest_.gameRoot;
    if (!fs::exists(resolved) || !fs::is_directory(resolved)) {
      return Err<void>(ErrorCategory::Validation,
                       fmt::format("[project].game_root '{}' does not resolve to a directory",
                                   resolved.string()));
    }
    gameRoot = fs::canonical(resolved);
  } else {
    gameRoot = fs::canonical(entryXexPath.parent_path());
  }

  fs::path entryRel = fs::relative(entryXexPath, gameRoot);
  if (entryRel.empty() || *entryRel.begin() == "..") {
    return Err<void>(ErrorCategory::Validation,
                     fmt::format("Entrypoint XEX '{}' resolves outside game root '{}'",
                                 entryXexPath.string(), gameRoot.string()));
  }

  auto runtime = std::make_unique<Runtime>(gameRoot.string());
  auto rtStatus = runtime->Setup(rex::RuntimeConfig{
      .kernel_init = rex::kernel::InitializeKernel,
      .tool_mode = true,
  });
  if (rtStatus != X_STATUS_SUCCESS) {
    return Err<void>(ErrorCategory::IO,
                     fmt::format("Failed to initialize Runtime: {:#x}", rtStatus));
  }

  std::string entryRelStr = entryRel.string();
  std::replace(entryRelStr.begin(), entryRelStr.end(), '/', '\\');
  auto entryVfsPath = "game:\\" + entryRelStr;
  rtStatus = runtime->LoadXexImage(entryVfsPath);
  if (rtStatus != X_STATUS_SUCCESS) {
    return Err<void>(ErrorCategory::IO,
                     fmt::format("Failed to load entrypoint XEX: {:#x}", rtStatus));
  }

  auto entry_display_name = std::filesystem::path(targeted[0].config.filePath).filename().string();
  ReportBinaryInfo(opts.reporter, entry_display_name,
                   *runtime->kernel_state()->GetExecutableModule()->xex_module());

  std::vector<rex::system::object_ref<rex::system::UserModule>> dllModules;
  for (size_t i = 1; i < targeted.size(); ++i) {
    const auto& dllConfig = targeted[i].config;
    fs::path dllXexPath = configDir / dllConfig.filePath;
    if (!fs::exists(dllXexPath)) {
      return Err<void>(ErrorCategory::IO,
                       fmt::format("DLL XEX not found: {}", dllXexPath.string()));
    }
    dllXexPath = fs::canonical(dllXexPath);

    auto relPath = fs::relative(dllXexPath, gameRoot);
    if (relPath.empty() || *relPath.begin() == "..") {
      return Err<void>(ErrorCategory::Validation,
                       fmt::format("DLL '{}' resolves outside game root '{}': {}",
                                   targeted[i].targetName, gameRoot.string(), dllXexPath.string()));
    }
    std::string relStr = relPath.string();
    std::replace(relStr.begin(), relStr.end(), '/', '\\');
    auto dllVfsPath = "game:\\" + relStr;
    auto userMod = runtime->kernel_state()->LoadUserModule(dllVfsPath, false);
    if (!userMod) {
      return Err<void>(ErrorCategory::IO,
                       fmt::format("Failed to load DLL module: {}", targeted[i].targetName));
    }
    REXCODEGEN_TRACE("Loaded DLL module '{}' at base 0x{:08X}", targeted[i].targetName,
                     userMod->xex_module()->base_address());
    auto dll_display_name = std::filesystem::path(targeted[i].config.filePath).filename().string();
    ReportBinaryInfo(opts.reporter, dll_display_name, *userMod->xex_module());
    dllModules.push_back(std::move(userMod));
  }

  struct ContextEntry {
    CodegenContext ctx;
    const ModuleEntry* module;
    std::string display_name;
  };
  std::vector<ContextEntry> contexts;

  auto make_display_name = [](const std::string& filePath) {
    return std::filesystem::path(filePath).filename().string();
  };

  auto* resolver = runtime->export_resolver();

  {
    auto execMod = runtime->kernel_state()->GetExecutableModule();
    auto bv = BinaryView::fromModule(*execMod->xex_module());

    // Optional: dump the decompressed/decrypted guest image for external analysis
    // (e.g. the jump-table IDA pass). Off unless REX_DUMP_IMAGE names an output path.
    // Reconstruct a contiguous image: each section copied to (sec base - image base);
    // gaps (PE headers, inter-section padding) stay zero, matching guest layout.
    if (const char* dumpPath = std::getenv("REX_DUMP_IMAGE")) {
      std::vector<uint8_t> img(bv.imageSize(), 0);
      for (const auto& sec : bv.sections()) {
        uint32_t off = sec.baseAddress - bv.baseAddress();
        if (sec.data && off <= img.size() && off + sec.size <= img.size())
          std::memcpy(img.data() + off, sec.data, sec.size);
      }
      std::ofstream ofs(dumpPath, std::ios::binary);
      ofs.write(reinterpret_cast<const char*>(img.data()), img.size());
      REXCODEGEN_INFO("Dumped guest image to {} ({:#x} bytes, base {:#010x}, {} sections)",
                      dumpPath, bv.imageSize(), bv.baseAddress(), bv.sections().size());
    }

    auto entry_display = make_display_name(targeted[0].config.filePath);
    RecompilerConfig cfg = std::move(targeted[0].config);
    if (opts.enableExceptionHandlers)
      cfg.generateExceptionHandlers = true;

    auto ctx = CodegenContext::Create(std::move(bv), std::move(cfg));
    ctx.setResolver(resolver);
    ctx.setConfigDir(configDir);
    ctx.analysisState().format = "xex";
    ctx.analysisState().loadAddress = ctx.binary().baseAddress();
    ctx.analysisState().entryPoint = ctx.binary().entryPoint();
    ctx.analysisState().imageSize = ctx.binary().imageSize();
    ctx.setHasDllModules(!manifest_.modules.empty());
    if (ctx.Config().isDll.has_value())
      ctx.setDllModule(*ctx.Config().isDll);

    contexts.push_back({std::move(ctx), &targeted[0], std::move(entry_display)});
  }

  for (size_t i = 0; i < dllModules.size(); ++i) {
    auto& userMod = dllModules[i];
    auto bv = BinaryView::fromModule(*userMod->xex_module());

    auto dll_display = make_display_name(targeted[i + 1].config.filePath);
    RecompilerConfig cfg = std::move(targeted[i + 1].config);
    if (opts.enableExceptionHandlers)
      cfg.generateExceptionHandlers = true;

    auto ctx = CodegenContext::Create(std::move(bv), std::move(cfg));
    ctx.setResolver(resolver);
    ctx.setConfigDir(configDir);
    ctx.analysisState().format = "xex";
    ctx.analysisState().loadAddress = ctx.binary().baseAddress();
    ctx.analysisState().entryPoint = ctx.binary().entryPoint();
    ctx.analysisState().imageSize = ctx.binary().imageSize();
    ctx.setDllModule(ctx.Config().isDll.value_or(true));
    ctx.setHasDllModules(true);

    contexts.push_back({std::move(ctx), &targeted[i + 1], std::move(dll_display)});
  }

  skippedModules_.clear();
  std::vector<std::string> fingerprints(contexts.size());
  std::vector<bool> skip(contexts.size(), false);
  std::vector<fs::path> allInputs;

  for (size_t i = 0; i < contexts.size(); ++i) {
    auto& entry = contexts[i];
    auto outDir = entry.ctx.configDir() / entry.ctx.Config().outDirectoryPath;
    auto inputs =
        CollectModuleInputs(entry.ctx.Config(), entry.ctx.configDir(), manifest_.manifestPath);
    fingerprints[i] = FingerprintModule(entry.ctx.Config(), inputs, opts.sdkVersion);
    allInputs.insert(allInputs.end(), inputs.begin(), inputs.end());

    if (opts.ignoreStamp || opts.force)
      continue;

    auto stamp = OutputStamp::Load(outDir / kStampFileName);
    if (stamp && OutputsAreUpToDate(*stamp, fingerprints[i], outDir)) {
      REXCODEGEN_TRACE("'{}' is up to date, skipping", entry.module->targetName);
      skip[i] = true;
      skippedModules_.push_back(entry.module->targetName);
    }
  }

  std::vector<std::chrono::steady_clock::time_point> module_started_at(contexts.size());
  for (size_t i = 0; i < contexts.size(); ++i) {
    if (skip[i])
      continue;
    auto& entry = contexts[i];
    if (opts.reporter) {
      opts.reporter->moduleStarted(entry.display_name, i, contexts.size());
    }
    module_started_at[i] = std::chrono::steady_clock::now();
    REXCODEGEN_TRACE("Analyzing '{}'...", entry.module->targetName);
    auto result = Analyze(entry.ctx, opts.reporter);
    if (!result) {
      if (opts.force && result.error().category == ErrorCategory::Validation) {
        REXLOG_WARN("Analysis errors for '{}' (continuing due to --force): {}",
                    entry.module->targetName, result.error().message);
      } else {
        REXLOG_ERROR("Analysis failed for '{}'", entry.module->targetName);
        return result;
      }
    }
  }

  for (size_t i = 0; i < contexts.size(); ++i) {
    uint32_t a_base = contexts[i].ctx.binary().baseAddress();
    uint32_t a_end = a_base + contexts[i].ctx.binary().imageSize();
    for (size_t j = i + 1; j < contexts.size(); ++j) {
      uint32_t b_base = contexts[j].ctx.binary().baseAddress();
      uint32_t b_end = b_base + contexts[j].ctx.binary().imageSize();
      if (a_base < b_end && b_base < a_end) {
        return Err<void>(ErrorCategory::Validation,
                         fmt::format("Module '{}' [{:08X}, {:08X}) overlaps '{}' [{:08X}, {:08X})",
                                     contexts[i].module->targetName, a_base, a_end,
                                     contexts[j].module->targetName, b_base, b_end));
      }
    }
  }

  for (size_t i = 0; i < contexts.size(); ++i) {
    if (skip[i])
      continue;
    auto& entry = contexts[i];
    if (opts.reporter) {
      opts.reporter->moduleStarted(entry.display_name, i, contexts.size());
      opts.reporter->phaseChanged("Write");
    }
    REXCODEGEN_TRACE("Writing output for '{}'...", entry.module->targetName);
    CodegenWriter writer(entry.ctx, runtime.get());
    if (!writer.write(opts.force)) {
      return Err<void>(ErrorCategory::Validation,
                       fmt::format("Write failed for '{}'", entry.module->targetName));
    }
    deletedFiles_.insert(deletedFiles_.end(), writer.deletedFiles().begin(),
                         writer.deletedFiles().end());
    writtenFiles_.insert(writtenFiles_.end(), writer.writtenFiles().begin(),
                         writer.writtenFiles().end());
    unchangedFiles_.insert(unchangedFiles_.end(), writer.unchangedFiles().begin(),
                           writer.unchangedFiles().end());

    OutputStamp stamp;
    stamp.fingerprint = fingerprints[i];
    stamp.outputs.insert(stamp.outputs.end(), writer.writtenFiles().begin(),
                         writer.writtenFiles().end());
    stamp.outputs.insert(stamp.outputs.end(), writer.unchangedFiles().begin(),
                         writer.unchangedFiles().end());
    std::sort(stamp.outputs.begin(), stamp.outputs.end());

    auto outDir = entry.ctx.configDir() / entry.ctx.Config().outDirectoryPath;
    if (WriteIfChanged(outDir / kStampFileName, stamp.Serialize()) == WriteOutcome::Failed) {
      return Err<void>(ErrorCategory::IO,
                       fmt::format("Failed to write stamp for '{}'", entry.module->targetName));
    }

    if (opts.reporter) {
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - module_started_at[i]);
      opts.reporter->moduleFinished(elapsed);
    }
  }

  if (!contexts.empty()) {
    auto buildDir = contexts[0].ctx.configDir() / contexts[0].ctx.Config().outDirectoryPath;
    std::sort(allInputs.begin(), allInputs.end());
    allInputs.erase(std::unique(allInputs.begin(), allInputs.end()), allInputs.end());

    auto buildStamp = fs::absolute(buildDir / kBuildStampFileName);
    if (!WriteDepfile(buildDir / kDepfileName, buildStamp, allInputs)) {
      return Err<void>(ErrorCategory::IO,
                       fmt::format("Failed to write {}", (buildDir / kDepfileName).string()));
    }
    // Never WriteIfChanged: the build rule re-runs until this mtime passes its inputs.
    if (!WriteFileBytes(buildStamp, fmt::format("{}\n", fmt::join(fingerprints, "\n")))) {
      return Err<void>(ErrorCategory::IO, fmt::format("Failed to write {}", buildStamp.string()));
    }
  }

  if (manifest_.modules.empty()) {
    REXCODEGEN_TRACE("Project recompiler complete");
    return Ok();
  }

  auto outputPath = contexts[0].ctx.configDir() / contexts[0].ctx.Config().outDirectoryPath;
  std::error_code mk_ec;
  fs::create_directories(outputPath, mk_ec);
  if (mk_ec) {
    return Err<void>(ErrorCategory::IO, fmt::format("Failed to create output dir {}: {}",
                                                    outputPath.string(), mk_ec.message()));
  }

  if (opts.reporter)
    opts.reporter->projectPhaseStarted("module_registry");
  {
    nlohmann::json registryData;
    registryData["project"] = manifest_.projectName;

    auto& dllArray = registryData["dll_modules"];
    dllArray = nlohmann::json::array();
    for (size_t i = 1; i < targeted.size(); ++i) {
      auto& mod = targeted[i];
      auto guestPath = mod.guestPath;
      std::replace(guestPath.begin(), guestPath.end(), '\\', '/');
      nlohmann::json dllEntry;
      dllEntry["pe_name"] = fs::path(mod.guestPath).filename().string();
      dllEntry["guest_path"] = guestPath;
      dllEntry["shared_lib_name"] = manifest_.projectName + "_" + mod.targetName;
      dllArray.push_back(dllEntry);
    }

    TemplateRegistry registry;
    auto registryContent = renderWithJson(registry, "codegen/module_registry_cpp", registryData);

    auto registryPath = outputPath / "module_registry.cpp";
    switch (WriteIfChanged(registryPath, registryContent)) {
      case WriteOutcome::Failed:
        return Err<void>(ErrorCategory::IO,
                         fmt::format("Failed while writing {}", registryPath.string()));
      case WriteOutcome::Unchanged:
        unchangedFiles_.push_back(registryPath.filename().string());
        break;
      case WriteOutcome::Written:
        writtenFiles_.push_back(registryPath.filename().string());
        break;
    }
    REXCODEGEN_TRACE("Wrote {}", registryPath.string());
  }
  if (opts.reporter)
    opts.reporter->projectPhaseFinished();

  if (opts.reporter)
    opts.reporter->projectPhaseStarted("dll_targets.cmake");
  {
    nlohmann::json dllTargetsData;
    dllTargetsData["project"] = manifest_.projectName;
    auto& dllTargetsArray = dllTargetsData["dll_modules"];
    dllTargetsArray = nlohmann::json::array();
    for (size_t i = 0; i < dllModules.size(); ++i) {
      auto& mod = targeted[i + 1];
      auto& dllCtx = contexts[i + 1].ctx;
      nlohmann::json entry;
      entry["target_name"] = mod.targetName;
      entry["lib_name"] = manifest_.projectName + "_" + mod.targetName;
      entry["output_dir"] = dllCtx.Config().outDirectoryPath;
      dllTargetsArray.push_back(entry);
    }

    TemplateRegistry registry;
    auto dllCmakeContent = renderWithJson(registry, "codegen/dll_targets_cmake", dllTargetsData);

    auto dllCmakePath = outputPath / "dll_targets.cmake";
    switch (WriteIfChanged(dllCmakePath, dllCmakeContent)) {
      case WriteOutcome::Failed:
        return Err<void>(ErrorCategory::IO,
                         fmt::format("Failed while writing {}", dllCmakePath.string()));
      case WriteOutcome::Unchanged:
        unchangedFiles_.push_back(dllCmakePath.filename().string());
        break;
      case WriteOutcome::Written:
        writtenFiles_.push_back(dllCmakePath.filename().string());
        break;
    }
    REXCODEGEN_TRACE("Wrote {}", dllCmakePath.string());
  }
  if (opts.reporter)
    opts.reporter->projectPhaseFinished();

  REXCODEGEN_TRACE("Project recompiler complete");
  return Ok();
}

}  // namespace rex::codegen
