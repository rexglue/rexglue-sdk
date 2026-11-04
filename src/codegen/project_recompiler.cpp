#include "project_recompiler.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "pipeline.h"
#include "binary_view.h"
#include "codegen_context.h"
#include "codegen_writer.h"
#include "config.h"
#include "hook_scan.h"
#include "output_stamp.h"
#include "progress_reporter.h"
#include "template_registry.h"
#include <rex/memory/endian.h>
#include <rex/filesystem/file_system_factory.h>
#include <rex/filesystem/xex.h>
#include <rex/kernel/types/xex.h>
#include <rex/module.h>
#include <rex/platform/file.h>
#include <rex/logging/log_category.h>
#include "codegen_categories.h"
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/object/module_object.h>
#include <rex/kernel/module.h>
#include <rex/kernel/object/object_table.h>

#include <chrono>

#include "codegen_tag.h"
#include "file_io.h"
#include "tool_runtime.h"
#include "template_render.h"

namespace rex::codegen {

namespace {

[[maybe_unused]] const bool g_tag_named = (memory::SetTagName(kCodegen, "Codegen"), true);

/** Revision of the emitted code, part of the module fingerprint. */
constexpr u32 kEmitterRevision = 6;

std::string DeriveTargetNameFromFilePath(const std::string& file_path) {
  std::filesystem::path p(file_path);
  std::string name = p.stem().string();
  std::replace(name.begin(), name.end(), '.', '_');
  std::replace(name.begin(), name.end(), ' ', '_');
  return name;
}

std::vector<std::filesystem::path> CollectModuleInputs(const RecompilerConfig& cfg,
                                                       const std::filesystem::path& config_dir,
                                                       const std::filesystem::path& manifest_path) {
  namespace fs = std::filesystem;

  std::vector<fs::path> inputs;
  inputs.push_back(config_dir / cfg.file_path);
  inputs.push_back(manifest_path);
  for (const auto& loaded : cfg.loaded_files) {
    inputs.emplace_back(loaded);
  }
  if (!cfg.template_dir.empty()) {
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(config_dir / cfg.template_dir, ec)) {
      if (entry.is_regular_file())
        inputs.push_back(entry.path());
    }
  }
  std::sort(inputs.begin(), inputs.end());
  return inputs;
}

std::string FingerprintModule(const RecompilerConfig& cfg,
                              std::span<const std::filesystem::path> inputs, const Limits& limits,
                              std::string_view sdk_version) {
  const auto& vm = cfg.vector_math;
  std::vector<std::string> flags{
      std::format("emitter={}", kEmitterRevision),
      std::format("templates={}", GetEmbeddedTemplatesHash()),
      std::format("vector_math={}{}{}{}{}{}{}{}{}", static_cast<u32>(vm.vmaddfp),
                  static_cast<u32>(vm.vnmsubfp), static_cast<u32>(vm.vmsum3fp128),
                  static_cast<u32>(vm.vmsum4fp128), static_cast<u32>(vm.vrefp),
                  static_cast<u32>(vm.vrsqrtefp), static_cast<u32>(vm.vexptefp),
                  static_cast<u32>(vm.fres), static_cast<u32>(vm.frsqrte)),
      std::format("generate_exception_handlers={}", cfg.generate_exception_handlers),
      std::format("max_file_size_bytes={}", limits.max_file_size_bytes),
      std::format("max_discovery_iterations={}", limits.max_discovery_iterations),
      std::format("max_vtable_iterations={}", limits.max_vtable_iterations),
      std::format("max_resolve_iterations={}", limits.max_resolve_iterations),
      std::format("max_eh_states={}", limits.max_eh_states),
      std::format("max_eh_try_blocks={}", limits.max_eh_try_blocks),
      std::format("max_eh_ip_map_entries={}", limits.max_eh_ip_map_entries),
      std::format("max_seh_scope_entries={}", limits.max_seh_scope_entries),
      std::format("backward_scan_limit={}", limits.backward_scan_limit),
      std::format("max_jump_table_entries={}", limits.max_jump_table_entries),
  };

  std::vector<u32> hook_addresses;
  for (const auto& [address, hook] : cfg.mid_asm_hooks) {
    if (hook.from_source) {
      hook_addresses.push_back(address);
    }
  }
  std::sort(hook_addresses.begin(), hook_addresses.end());
  for (u32 address : hook_addresses) {
    const MidAsmHook& hook = cfg.mid_asm_hooks.at(address);
    std::string registers;
    for (const auto& reg : hook.registers) {
      registers += reg + ",";
    }
    std::string parameters;
    for (const auto& parameter : hook.parameters) {
      parameters += parameter + ",";
    }
    flags.push_back(std::format("source_hook={:08X}:{}:{}:{}:{}:{}:{:08X}", address, hook.name,
                                registers, parameters, hook.after_instruction, hook.return_on_true,
                                hook.jump_address_on_true));
  }

  return ComputeInputFingerprint(inputs, sdk_version, flags);
}

void ReportBinaryInfo(IProgressReporter* reporter, std::string_view display_name,
                      const rex::kernel::XEXModule& xex) {
  if (!reporter)
    return;
  BinaryIdentity info{};
  info.name = display_name;
  info.pe_time_date_stamp = xex.GetPE().time_date_stamp;
  if (auto* exec = xex.GetImage().GetOptHeader<kernel::XEXOptExecutionInfo>(
          kernel::kXEXHeaderExecutionInfo)) {
    info.title_id = exec->title_id;
    info.media_id = exec->media_id;
    auto version = exec->GetVersion();
    info.version_major = version.major;
    info.version_minor = version.minor;
    info.version_build = version.build;
    info.version_qfe = version.qfe;
  }
  reporter->BinaryLoaded(info);
}

/**
 * Reads the base address and image size of an XEX from its header without decoding the image.
 *
 * @param files The file system factory that parses the header.
 * @return The base address and image size, or the failure.
 */
Result<std::pair<u32, u32>> ReadImageRange(rex::filesystem::IFileSystemFactory& files,
                                           const std::filesystem::path& path) {
  auto prefix = rex::platform::ReadHostFile(path, 0, 16);
  if (!prefix || prefix->size() < 16) {
    return Err<std::pair<u32, u32>>(ErrorCategory::kIO,
                                    std::format("Cannot read the XEX header of {}", path.string()));
  }
  const u32 header_size = rex::memory::load_and_swap<u32>(prefix->data() + 8);
  auto header = rex::platform::ReadHostFile(path, 0, header_size);
  if (!header) {
    return Err<std::pair<u32, u32>>(header.error());
  }
  auto image = files.ParseXEX(*header);
  if (!image) {
    return Err<std::pair<u32, u32>>(image.error());
  }
  const u32 base = (*image)
                       ->GetOptHeaderValue(rex::kernel::kXEXHeaderImageBaseAddress)
                       .value_or((*image)->GetSecurity().load_address);
  return std::pair<u32, u32>{base, (*image)->GetImageSize()};
}

}  // namespace

ProjectRecompiler::ProjectRecompiler(ManifestConfig manifest) : manifest_(std::move(manifest)) {}

Result<void> ProjectRecompiler::Run(const ProjectRecompilerOptions& opts) {
  namespace fs = std::filesystem;

  deleted_files_.clear();
  written_files_.clear();
  unchanged_files_.clear();

  if (manifest_.modules.empty()) {
    REXCG_TRACE("Recompiling '{}' (entrypoint)", manifest_.project_name);
  } else {
    REXCG_TRACE("Recompiling '{}' (entrypoint + {} DLL{})", manifest_.project_name,
                manifest_.modules.size(), manifest_.modules.size() == 1 ? "" : "s");
  }

  struct ModuleEntry {
    ModuleEntry(std::string target_name, std::string guest_path, RecompilerConfig recompiler_config)
        : target_name(std::move(target_name)),
          guest_path(std::move(guest_path)),
          config(std::move(recompiler_config)) {}
    ModuleEntry(const ModuleEntry&) = default;
    ModuleEntry& operator=(const ModuleEntry&) = default;
    ModuleEntry(ModuleEntry&&) noexcept(false) = default;
    ModuleEntry& operator=(ModuleEntry&&) noexcept(false) = default;
    ~ModuleEntry() = default;

    std::string target_name;
    std::string guest_path;
    RecompilerConfig config;
  };

  std::vector<ModuleEntry> all_modules;
  all_modules.emplace_back(DeriveTargetNameFromFilePath(manifest_.entrypoint.recompiler.file_path),
                           "", std::move(manifest_.entrypoint.recompiler));
  for (auto& mod : manifest_.modules) {
    all_modules.emplace_back(DeriveTargetNameFromFilePath(mod.recompiler.file_path), mod.guest_path,
                             std::move(mod.recompiler));
  }

  if (!opts.targets.empty()) {
    std::vector<std::string> unknown;
    for (const auto& t : opts.targets) {
      bool match = std::any_of(all_modules.begin() + 1, all_modules.end(),
                               [&t](const ModuleEntry& m) { return m.target_name == t; });
      if (!match) {
        unknown.push_back(t);
      }
    }
    if (!unknown.empty()) {
      std::string known;
      for (size_t i = 1; i < all_modules.size(); ++i) {
        if (i > 1)
          known += ", ";
        known += all_modules[i].target_name;
      }
      std::string list;
      for (size_t i = 0; i < unknown.size(); ++i) {
        if (i > 0)
          list += ", ";
        list += unknown[i];
      }
      return Err<void>(ErrorCategory::kConfig,
                       std::format("Unknown --target value(s): {}. Known DLL targets: {}", list,
                                   known.empty() ? "(none)" : known));
    }
  }

  std::vector<ModuleEntry> targeted;
  std::vector<std::string> untargeted_files;
  targeted.push_back(std::move(all_modules[0]));
  for (size_t i = 1; i < all_modules.size(); ++i) {
    if (opts.targets.empty() || std::find(opts.targets.begin(), opts.targets.end(),
                                          all_modules[i].target_name) != opts.targets.end()) {
      targeted.push_back(std::move(all_modules[i]));
    } else {
      untargeted_files.push_back(all_modules[i].config.file_path);
    }
  }

  std::unordered_map<std::string, std::string> out_dir_owner;
  for (const auto& m : targeted) {
    const auto& out_dir = m.config.out_directory_path;
    if (out_dir.empty())
      continue;
    auto [it, inserted] = out_dir_owner.emplace(out_dir, m.target_name);
    if (!inserted) {
      return Err<void>(
          ErrorCategory::kValidation,
          std::format("out_directory_path '{}' is shared by '{}' and '{}'. Each binary "
                      "needs its own output directory",
                      out_dir, it->second, m.target_name));
    }
  }

  const auto& entry_config = targeted[0].config;
  auto config_dir = manifest_.manifest_dir;

  std::vector<fs::path> hook_files;
  std::erase_if(manifest_.shared.mid_asm_hooks,
                [](const auto& entry) { return entry.second.from_source; });
  if (!manifest_.hook_sources.empty()) {
    std::vector<fs::path> roots;
    roots.reserve(manifest_.hook_sources.size());
    for (const auto& folder : manifest_.hook_sources) {
      roots.push_back(config_dir / folder);
    }
    auto scanned = ScanHookSources(roots, hook_files);
    if (!scanned) {
      return Err<void>(scanned.error());
    }
    for (const auto& hook : *scanned) {
      const bool claimed = manifest_.shared.mid_asm_hooks.contains(hook.address) ||
                           std::ranges::any_of(targeted, [&](const ModuleEntry& module) {
                             return module.config.mid_asm_hooks.contains(hook.address);
                           });
      if (claimed) {
        return Err<void>(ErrorCategory::kValidation,
                         std::format("{}:{}: 0x{:08X} already has a [[midasm_hook]] in the config",
                                     hook.file.string(), hook.line, hook.address));
      }
      manifest_.shared.mid_asm_hooks.emplace(hook.address, ToMidAsmHook(hook));
    }
  }
  fs::path entry_xex_path = config_dir / entry_config.file_path;
  if (!fs::exists(entry_xex_path)) {
    return Err<void>(ErrorCategory::kIO,
                     std::format("Entrypoint XEX not found: {}", entry_xex_path.string()));
  }
  entry_xex_path = fs::canonical(entry_xex_path);

  fs::path game_root;
  if (manifest_.game_root && !manifest_.game_root->empty()) {
    fs::path resolved = config_dir / *manifest_.game_root;
    if (!fs::exists(resolved) || !fs::is_directory(resolved)) {
      return Err<void>(ErrorCategory::kValidation,
                       std::format("[project].game_root '{}' does not resolve to a directory",
                                   resolved.string()));
    }
    game_root = fs::canonical(resolved);
  } else {
    game_root = fs::canonical(entry_xex_path.parent_path());
  }

  fs::path entry_rel = fs::relative(entry_xex_path, game_root);
  if (entry_rel.empty() || *entry_rel.begin() == "..") {
    return Err<void>(ErrorCategory::kValidation,
                     std::format("Entrypoint XEX '{}' resolves outside game root '{}'",
                                 entry_xex_path.string(), game_root.string()));
  }

  if (opts.tools == nullptr) {
    return Err<void>(ErrorCategory::kValidation, "the run needs a tool app");
  }

  std::vector<std::string> imports;
  auto to_vfs_path = [&](const fs::path& file) -> std::optional<std::string> {
    if (!fs::exists(file)) {
      return std::nullopt;
    }
    auto relative = fs::relative(fs::canonical(file), game_root);
    if (relative.empty() || *relative.begin() == "..") {
      return std::nullopt;
    }
    std::string text = relative.string();
    std::replace(text.begin(), text.end(), '/', '\\');
    return text;
  };
  for (size_t i = 1; i < targeted.size(); ++i) {
    if (auto relative = to_vfs_path(config_dir / targeted[i].config.file_path)) {
      imports.push_back(*relative);
    }
  }
  for (const auto& file : untargeted_files) {
    if (auto relative = to_vfs_path(config_dir / file)) {
      imports.push_back(*relative);
    }
  }

  std::string entry_rel_str = entry_rel.string();
  std::replace(entry_rel_str.begin(), entry_rel_str.end(), '/', '\\');
  auto tool = opts.tools->Open(game_root, entry_rel_str, imports);
  if (!tool) {
    return Err<void>(ErrorCategory::kIO,
                     std::format("Failed to load entrypoint XEX: {}", tool.error().message));
  }
  rex::kernel::TitleRuntime* runtime = &tool->GetRuntime();

  auto entry_display_name = std::filesystem::path(targeted[0].config.file_path).filename().string();
  auto entry_view = [&] {
    auto executable = runtime->GetObjectTable().Get<rex::kernel::ModuleObject>(
        runtime->GetModuleRegistry().GetExecutable().value_or(0));
    ReportBinaryInfo(opts.reporter, entry_display_name, *executable->GetXEX());
    return BinaryView::FromModule(*executable->GetXEX());
  }();

  std::vector<BinaryView> dll_views;
  for (size_t i = 1; i < targeted.size(); ++i) {
    const auto& dll_config = targeted[i].config;
    fs::path dll_xex_path = config_dir / dll_config.file_path;
    if (!fs::exists(dll_xex_path)) {
      return Err<void>(ErrorCategory::kIO,
                       std::format("DLL XEX not found: {}", dll_xex_path.string()));
    }
    dll_xex_path = fs::canonical(dll_xex_path);

    auto rel_path = fs::relative(dll_xex_path, game_root);
    if (rel_path.empty() || *rel_path.begin() == "..") {
      return Err<void>(
          ErrorCategory::kValidation,
          std::format("DLL '{}' resolves outside game root '{}': {}", targeted[i].target_name,
                      game_root.string(), dll_xex_path.string()));
    }
    std::string rel_str = rel_path.string();
    std::replace(rel_str.begin(), rel_str.end(), '/', '\\');
    auto dll_vfs_path = "game:\\" + rel_str;
    auto loaded = runtime->GetModuleRegistry().LoadUserModule(dll_vfs_path, false);
    {
      auto user_mod = runtime->GetObjectTable().Get<rex::kernel::ModuleObject>(
          loaded ? *loaded : rex::kernel::X_HANDLE(0));
      if (!user_mod) {
        return Err<void>(ErrorCategory::kIO,
                         std::format("Failed to load DLL module: {}", targeted[i].target_name));
      }
      REXCG_TRACE("Loaded DLL module '{}' at base 0x{:08X}", targeted[i].target_name,
                  user_mod->GetXEX()->GetBaseAddress());
      auto dll_display_name =
          std::filesystem::path(targeted[i].config.file_path).filename().string();
      ReportBinaryInfo(opts.reporter, dll_display_name, *user_mod->GetXEX());
      dll_views.push_back(BinaryView::FromModule(*user_mod->GetXEX()));
    }
    runtime->GetModuleRegistry().UnloadUserModule(*loaded, false);
  }

  std::vector<std::pair<u32, u32>> image_ranges;
  image_ranges.emplace_back(entry_view.GetBaseAddress(), entry_view.GetImageSize());
  for (const auto& view : dll_views) {
    image_ranges.emplace_back(view.GetBaseAddress(), view.GetImageSize());
  }
  rex::filesystem::IFileSystemFactory* files = REX_INTERFACE(rex::filesystem::IFileSystemFactory);
  if (!files) {
    return Err<void>(ErrorCategory::kModule, "the file system module is not running");
  }
  for (const auto& file : untargeted_files) {
    auto range = ReadImageRange(*files, config_dir / file);
    if (!range) {
      return Err<void>(range.error());
    }
    image_ranges.push_back(*range);
  }
  const auto outside = manifest_.shared.ListEntriesOutside(image_ranges);
  if (!outside.empty()) {
    std::string list;
    for (const auto& entry : outside) {
      list += (list.empty() ? "" : ", ") + entry;
    }
    return Err<void>(ErrorCategory::kValidation,
                     std::format("Manifest entries are in no binary image: {}", list));
  }

  struct ContextEntry {
    CodegenContext ctx;
    const ModuleEntry* module;
    std::string display_name;
  };
  std::vector<ContextEntry> contexts;

  auto merge_shared = [this](RecompilerConfig& cfg, const BinaryView& view) -> Result<void> {
    cfg.MergeSharedEntriesIn(manifest_.shared, view.GetBaseAddress(), view.GetImageSize());
    const auto checked = cfg.Validate();
    if (!checked.valid) {
      return Err<void>(ErrorCategory::kValidation,
                       std::format("Config invalid after the shared manifest entries: {}",
                                   checked.errors.front()));
    }
    return Ok();
  };

  auto make_display_name = [](const std::string& file_path) {
    return std::filesystem::path(file_path).filename().string();
  };

  {
    auto bv = std::move(entry_view);

    auto entry_display = make_display_name(targeted[0].config.file_path);
    RecompilerConfig cfg = std::move(targeted[0].config);
    if (auto merged = merge_shared(cfg, bv); !merged) {
      return merged;
    }

    auto ctx = CodegenContext::Create(std::move(bv), std::move(cfg));
    ctx.SetLimits(opts.limits);
    ctx.SetConfigDir(config_dir);
    ctx.GetAnalysisState().format = "xex";
    ctx.GetAnalysisState().load_address = ctx.GetBinary().GetBaseAddress();
    ctx.GetAnalysisState().entry_point = ctx.GetBinary().GetEntryPoint();
    ctx.GetAnalysisState().image_size = ctx.GetBinary().GetImageSize();
    ctx.SetHasDLLModules(!manifest_.modules.empty());
    if (const auto& is_dll = ctx.GetConfig().is_dll)
      ctx.SetDLLModule(*is_dll);

    contexts.push_back({std::move(ctx), &targeted[0], std::move(entry_display)});
  }

  for (size_t i = 0; i < dll_views.size(); ++i) {
    auto bv = std::move(dll_views[i]);

    auto dll_display = make_display_name(targeted[i + 1].config.file_path);
    RecompilerConfig cfg = std::move(targeted[i + 1].config);
    if (auto merged = merge_shared(cfg, bv); !merged) {
      return merged;
    }

    auto ctx = CodegenContext::Create(std::move(bv), std::move(cfg));
    ctx.SetLimits(opts.limits);
    ctx.SetConfigDir(config_dir);
    ctx.GetAnalysisState().format = "xex";
    ctx.GetAnalysisState().load_address = ctx.GetBinary().GetBaseAddress();
    ctx.GetAnalysisState().entry_point = ctx.GetBinary().GetEntryPoint();
    ctx.GetAnalysisState().image_size = ctx.GetBinary().GetImageSize();
    ctx.SetDLLModule(ctx.GetConfig().is_dll.value_or(true));
    ctx.SetHasDLLModules(true);

    contexts.push_back({std::move(ctx), &targeted[i + 1], std::move(dll_display)});
  }

  skipped_modules_.clear();
  std::vector<std::string> fingerprints(contexts.size());
  std::vector<bool> skip(contexts.size(), false);
  std::vector<fs::path> all_inputs;
  all_inputs.insert(all_inputs.end(), hook_files.begin(), hook_files.end());

  for (size_t i = 0; i < contexts.size(); ++i) {
    auto& entry = contexts[i];
    auto out_dir = entry.ctx.GetConfigDir() / entry.ctx.GetConfig().out_directory_path;
    auto inputs = CollectModuleInputs(entry.ctx.GetConfig(), entry.ctx.GetConfigDir(),
                                      manifest_.manifest_path);
    fingerprints[i] =
        FingerprintModule(entry.ctx.GetConfig(), inputs, opts.limits, opts.sdk_version);
    all_inputs.insert(all_inputs.end(), inputs.begin(), inputs.end());

    if (opts.ignore_stamp || opts.force)
      continue;

    auto stamp = OutputStamp::Load(out_dir / kStampFileName);
    if (stamp && AreOutputsUpToDate(*stamp, fingerprints[i], out_dir)) {
      REXCG_TRACE("'{}' is up to date, skipping", entry.module->target_name);
      skip[i] = true;
      skipped_modules_.push_back(entry.module->target_name);
    }
  }

  std::vector<std::chrono::steady_clock::time_point> module_started_at(contexts.size());
  for (size_t i = 0; i < contexts.size(); ++i) {
    if (skip[i])
      continue;
    auto& entry = contexts[i];
    if (opts.reporter) {
      opts.reporter->ModuleStarted(entry.display_name, i, contexts.size());
    }
    module_started_at[i] = std::chrono::steady_clock::now();
    REXCG_TRACE("Analyzing '{}'...", entry.module->target_name);
    auto result = Analyze(entry.ctx, opts.reporter);
    if (!result) {
      if (opts.force && result.error().category == ErrorCategory::kValidation) {
        REXCG_WARN("Analysis errors for '{}' (continuing due to --force): {}",
                   entry.module->target_name, result.error().message);
      } else {
        REXCG_FAIL(result, "Analysis failed for '{}'", entry.module->target_name);
      }
    }
  }

  for (size_t i = 0; i < contexts.size(); ++i) {
    if (skip[i])
      continue;
    auto& entry = contexts[i];
    if (opts.reporter) {
      opts.reporter->ModuleStarted(entry.display_name, i, contexts.size());
      opts.reporter->ModulePhaseStarted("Write");
    }
    REXCG_TRACE("Writing output for '{}'...", entry.module->target_name);
    CodegenWriter writer(entry.ctx);
    if (!writer.Write(opts.force)) {
      return Err<void>(ErrorCategory::kValidation,
                       std::format("Write failed for '{}'", entry.module->target_name));
    }
    deleted_files_.insert(deleted_files_.end(), writer.GetDeletedFiles().begin(),
                          writer.GetDeletedFiles().end());
    written_files_.insert(written_files_.end(), writer.GetWrittenFiles().begin(),
                          writer.GetWrittenFiles().end());
    unchanged_files_.insert(unchanged_files_.end(), writer.GetUnchangedFiles().begin(),
                            writer.GetUnchangedFiles().end());

    OutputStamp stamp;
    stamp.fingerprint = fingerprints[i];
    stamp.outputs.insert(stamp.outputs.end(), writer.GetWrittenFiles().begin(),
                         writer.GetWrittenFiles().end());
    stamp.outputs.insert(stamp.outputs.end(), writer.GetUnchangedFiles().begin(),
                         writer.GetUnchangedFiles().end());
    std::sort(stamp.outputs.begin(), stamp.outputs.end());

    auto out_dir = entry.ctx.GetConfigDir() / entry.ctx.GetConfig().out_directory_path;
    if (WriteIfChanged(out_dir / kStampFileName, stamp.Serialize()) == WriteOutcome::kFailed) {
      return Err<void>(ErrorCategory::kIO,
                       std::format("Failed to write stamp for '{}'", entry.module->target_name));
    }

    if (opts.reporter) {
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - module_started_at[i]);
      opts.reporter->ModuleFinished(elapsed);
    }
  }

  if (!contexts.empty()) {
    auto build_dir =
        contexts[0].ctx.GetConfigDir() / contexts[0].ctx.GetConfig().out_directory_path;
    std::sort(all_inputs.begin(), all_inputs.end());
    all_inputs.erase(std::unique(all_inputs.begin(), all_inputs.end()), all_inputs.end());

    auto build_stamp = fs::absolute(build_dir / kBuildStampFileName);
    if (!WriteDepfile(build_dir / kDepfileName, build_stamp, all_inputs)) {
      return Err<void>(ErrorCategory::kIO,
                       std::format("Failed to write {}", (build_dir / kDepfileName).string()));
    }
    std::string stamp_text;
    for (const auto& fingerprint : fingerprints) {
      stamp_text += fingerprint;
      stamp_text += '\n';
    }
    if (!rex::platform::WriteHostFile(build_stamp, stamp_text)) {
      return Err<void>(ErrorCategory::kIO, std::format("Failed to write {}", build_stamp.string()));
    }
  }

  if (manifest_.modules.empty()) {
    REXCG_TRACE("Project recompiler complete");
    return Ok();
  }

  auto output_path =
      contexts[0].ctx.GetConfigDir() / contexts[0].ctx.GetConfig().out_directory_path;
  std::error_code mk_ec;
  fs::create_directories(output_path, mk_ec);
  if (mk_ec) {
    return Err<void>(ErrorCategory::kIO, std::format("Failed to create output dir {}: {}",
                                                     output_path.string(), mk_ec.message()));
  }

  if (opts.reporter)
    opts.reporter->ProjectPhaseStarted("module_registry");
  {
    nlohmann::json registry_data;
    registry_data["project"] = manifest_.project_name;

    auto& dll_array = registry_data["dll_modules"];
    dll_array = nlohmann::json::array();
    for (size_t i = 1; i < targeted.size(); ++i) {
      auto& mod = targeted[i];
      auto guest_path = mod.guest_path;
      std::replace(guest_path.begin(), guest_path.end(), '\\', '/');
      nlohmann::json dll_entry;
      dll_entry["pe_name"] = fs::path(mod.guest_path).filename().string();
      dll_entry[kModuleGuestPathKey] = guest_path;
      dll_entry["shared_lib_name"] = manifest_.project_name + "_" + mod.target_name;
      dll_array.push_back(dll_entry);
    }

    TemplateRegistry registry;
    auto registry_content = RenderWithJSON(registry, "codegen/module_registry_cpp", registry_data);

    auto registry_path = output_path / "module_registry.cpp";
    switch (WriteIfChanged(registry_path, registry_content)) {
      case WriteOutcome::kFailed:
        return Err<void>(ErrorCategory::kIO,
                         std::format("Failed while writing {}", registry_path.string()));
      case WriteOutcome::kUnchanged:
        unchanged_files_.push_back(registry_path.filename().string());
        break;
      case WriteOutcome::kWritten:
        written_files_.push_back(registry_path.filename().string());
        break;
    }
    REXCG_TRACE("Wrote {}", registry_path.string());
  }
  if (opts.reporter)
    opts.reporter->ProjectPhaseFinished();

  if (opts.reporter)
    opts.reporter->ProjectPhaseStarted("dll_targets.cmake");
  {
    nlohmann::json dll_targets_data;
    dll_targets_data["project"] = manifest_.project_name;
    auto& dll_targets_array = dll_targets_data["dll_modules"];
    dll_targets_array = nlohmann::json::array();
    for (size_t i = 0; i < dll_views.size(); ++i) {
      auto& mod = targeted[i + 1];
      auto& dll_ctx = contexts[i + 1].ctx;
      nlohmann::json entry;
      entry["project"] = dll_ctx.GetConfig().project_name;
      entry["target_name"] = mod.target_name;
      entry["lib_name"] = manifest_.project_name + "_" + mod.target_name;
      entry["output_dir"] = dll_ctx.GetConfig().out_directory_path;
      dll_targets_array.push_back(entry);
    }

    TemplateRegistry registry;
    auto dll_cmake_content =
        RenderWithJSON(registry, "codegen/dll_targets_cmake", dll_targets_data);

    auto dll_cmake_path = output_path / "dll_targets.cmake";
    switch (WriteIfChanged(dll_cmake_path, dll_cmake_content)) {
      case WriteOutcome::kFailed:
        return Err<void>(ErrorCategory::kIO,
                         std::format("Failed while writing {}", dll_cmake_path.string()));
      case WriteOutcome::kUnchanged:
        unchanged_files_.push_back(dll_cmake_path.filename().string());
        break;
      case WriteOutcome::kWritten:
        written_files_.push_back(dll_cmake_path.filename().string());
        break;
    }
    REXCG_TRACE("Wrote {}", dll_cmake_path.string());
  }
  if (opts.reporter)
    opts.reporter->ProjectPhaseFinished();

  REXCG_TRACE("Project recompiler complete");
  return Ok();
}

}  // namespace rex::codegen
