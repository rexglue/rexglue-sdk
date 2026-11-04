#include "init_command.h"
#include "../ui/ui.h"
#include "app_name.h"
#include "codegen/tool_runtime.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <format>
#include <memory>
#include <span>
#include <sstream>
#include <vector>

#include <rex/string/text.h>
#include "codegen/manifest.h"
#include "codegen/template_registry.h"
#include <rex/logging/log.h>
#include "rexglue/log_categories.h"
#include <rex/platform/file.h>
#include <rex/result.h>
#include <rex/kernel/title_runtime.h>
#include <rex/xam/content_manager.h>
#include <rex/filesystem/xdbf.h>
#include <rex/kernel/types/content.h>
#include <rex/version.h>

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>
#include <toml++/toml.hpp>

namespace fs = std::filesystem;

namespace rexglue::cli {

using rex::Err;
using rex::ErrorCategory;
using rex::Ok;

namespace {

struct SDKLocation {
  std::string prefix;
  std::string source_dir;
};

SDKLocation FindSDK(const fs::path& exe) {
  SDKLocation found;
  std::error_code ec;
  if (exe.empty())
    return found;
  const fs::path exe_dir = exe.parent_path();
  const fs::path prefix = exe_dir.parent_path();
  for (const char* libdir : {"lib", "lib64"}) {
    fs::directory_iterator it(prefix / libdir, ec);
    if (ec)
      continue;
    if (fs::exists(prefix / libdir / "cmake" / "rexglue" / "rexglueConfig.cmake", ec)) {
      found.prefix = prefix.generic_string();
      return found;
    }
    for (const auto& entry : it) {
      if (fs::exists(entry.path() / "cmake" / "rexglue" / "rexglueConfig.cmake", ec)) {
        found.prefix = prefix.generic_string();
        return found;
      }
    }
  }
  for (fs::path dir = exe_dir; dir.has_parent_path() && dir != dir.parent_path();
       dir = dir.parent_path()) {
    if (fs::exists(dir / "CMakeLists.txt", ec) &&
        fs::exists(dir / "cmake" / "rexglue_sdk.cmake", ec) &&
        fs::exists(dir / "include" / "rex", ec)) {
      found.source_dir = dir.generic_string();
      return found;
    }
  }
  return found;
}

std::string ManifestPath(const fs::path& target, const fs::path& base) {
  std::error_code ec;
  fs::path rel = fs::relative(target, base, ec);
  if (ec || rel.empty()) {
    return target.generic_string();
  }
  return rel.generic_string();
}

std::string ModuleStem(const fs::path& xex) {
  std::string stem = rex::string::utf8_lower_ascii(xex.stem().string());
  std::replace(stem.begin(), stem.end(), '.', '_');
  std::replace(stem.begin(), stem.end(), ' ', '_');
  return stem;
}

std::string IsoUtcStamp() {
  return std::format("{:%F %T} UTC",
                     std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
}

fs::path ResolveDir(const std::string& raw, std::error_code& ec) {
  fs::path p = fs::absolute(raw, ec);
  if (ec)
    return p;
  fs::path canon = fs::weakly_canonical(p, ec);
  if (ec) {
    ec.clear();
    return p;
  }
  return canon;
}

}  // namespace

Result<void> InitProject(const InitOptions& opts, const CLIContext& ctx) {
  (void)ctx;

  if (opts.project_name.empty())
    return Err<void>(ErrorCategory::kConfig, "--project_name is required");
  if (opts.xex_path.empty())
    return Err<void>(ErrorCategory::kConfig, "--xex_path is required (path to entrypoint XEX)");

  std::string validation_error;
  if (!ValidateProjectName(opts.project_name, validation_error))
    return Err<void>(ErrorCategory::kConfig, validation_error);
  auto names = ParseAppName(opts.project_name);

  std::error_code ec;
  fs::path project_root =
      opts.project_root.empty() ? fs::current_path(ec) : ResolveDir(opts.project_root, ec);
  if (ec)
    return Err<void>(ErrorCategory::kIO, "Failed to resolve project root: " + ec.message());

  fs::path xex_abs = ResolveDir(opts.xex_path, ec);
  if (ec)
    return Err<void>(ErrorCategory::kIO,
                     "Failed to resolve --xex_path '" + opts.xex_path + "': " + ec.message());
  if (!fs::exists(xex_abs))
    return Err<void>(ErrorCategory::kIO, "Entrypoint XEX not found: " + xex_abs.string());
  if (!fs::is_regular_file(xex_abs))
    return Err<void>(ErrorCategory::kIO, "--xex_path is not a regular file: " + xex_abs.string());

  std::string xex_stem = xex_abs.stem().string();
  if (xex_stem.empty())
    return Err<void>(ErrorCategory::kConfig, "--xex_path has no filename: " + opts.xex_path);

  fs::path game_root_abs;
  if (opts.game_root.empty()) {
    game_root_abs = xex_abs.parent_path();
  } else {
    game_root_abs = ResolveDir(opts.game_root, ec);
    if (ec)
      return Err<void>(ErrorCategory::kIO,
                       "Failed to resolve --game_root '" + opts.game_root + "': " + ec.message());
  }
  if (!fs::exists(game_root_abs) || !fs::is_directory(game_root_abs))
    return Err<void>(ErrorCategory::kIO,
                     "--game_root is not a directory: " + game_root_abs.string());

  fs::path xex_rel_to_game = fs::relative(xex_abs, game_root_abs, ec);
  if (ec || xex_rel_to_game.empty() || *xex_rel_to_game.begin() == fs::path("..")) {
    return Err<void>(ErrorCategory::kConfig, "--xex_path (" + xex_abs.string() +
                                                 ") is not inside --game_root (" +
                                                 game_root_abs.string() + ")");
  }

  std::string out_dir = rex::string::utf8_lower_ascii("generated/" + xex_stem);
  std::string xex_rel_manifest = ManifestPath(xex_abs, project_root);
  std::string game_root_rel_manifest = ManifestPath(game_root_abs, project_root);

  nlohmann::json modules_json = nlohmann::json::array();
  if (opts.scan_dlls) {
    std::vector<fs::path> dll_paths;
    fs::recursive_directory_iterator it(game_root_abs,
                                        fs::directory_options::skip_permission_denied, ec);
    fs::recursive_directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
      if (!it->is_regular_file())
        continue;
      if (!rex::string::utf8_equal_case(it->path().extension().string(), ".dll"))
        continue;
      dll_paths.push_back(it->path());
    }
    std::sort(dll_paths.begin(), dll_paths.end());

    for (const auto& dll_abs : dll_paths) {
      fs::path rel_under_game = fs::relative(dll_abs, game_root_abs, ec);
      if (ec || rel_under_game.empty()) {
        ec.clear();
        continue;
      }
      modules_json.push_back({
          {rex::codegen::kModuleGuestPathKey, rex::codegen::CanonicalizeModuleGuestPath(
                                                  rel_under_game.generic_string(), names.original)},
          {"file_path", ManifestPath(dll_abs, project_root)},
          {"out_directory_path", "generated/" + ModuleStem(dll_abs)},
      });
    }
  }

  rex::codegen::TemplateRegistry registry;
  if (!opts.template_dir.empty())
    registry.LoadOverrides(opts.template_dir);

  const SDKLocation sdk =
      FindSDK(opts.sdk_exe.empty() ? rex::platform::GetExecutablePath() : opts.sdk_exe);
  nlohmann::json data = {
      {"names", NamesToJSON(names)},
      {"sdk_version", REXGLUE_VERSION_FLOOR},
      {"sdk_version_full", REXGLUE_VERSION_STRING},
      {"generated_on", IsoUtcStamp()},
      {"include_stamp", true},
      {"xex_path", xex_rel_manifest},
      {"game_root", game_root_rel_manifest},
      {"out_directory_path", out_dir},
      {"entrypoint_out_dir", out_dir},
      {"modules", modules_json},
      {"sdk_prefix", sdk.prefix},
      {"sdk_dir", sdk.source_dir},
  };
  std::string json_str = data.dump();

  std::vector<ui::KeyValueRow> header_rows;
  header_rows.push_back({"Project", names.original});
  header_rows.push_back({"Root", project_root.string()});
  header_rows.push_back({"Entrypoint", xex_rel_manifest});
  header_rows.push_back({"Game root", game_root_rel_manifest});
  if (opts.scan_dlls) {
    header_rows.push_back({"DLL modules", std::to_string(modules_json.size())});
  }
  ui::KeyValueBlock("Initializing project:", header_rows);

  if (fs::exists(project_root) && !fs::is_directory(project_root)) {
    return Err<void>(ErrorCategory::kIO,
                     "Path exists but is not a directory: " + project_root.string());
  }

  enum class RegeneratePolicy : u8 { kAlwaysRegenerate, kFirstInitOnly, kRequiresForce };
  struct Render {
    std::string template_id;
    fs::path out;
    RegeneratePolicy policy;
  };
  std::string manifest_file = names.original + "_manifest.toml";
  std::vector<Render> renders = {
      {"init/cmakelists", project_root / "CMakeLists.txt", RegeneratePolicy::kRequiresForce},
      {"init/rexglue_cmake", project_root / "generated" / "rexglue.cmake",
       RegeneratePolicy::kAlwaysRegenerate},
      {"init/main_cpp", project_root / "src" / "main.cpp", RegeneratePolicy::kFirstInitOnly},
      {"init/hooks_cpp", project_root / "src" / (names.original + "_hooks.cpp"),
       RegeneratePolicy::kFirstInitOnly},
      {"init/manifest_toml", project_root / manifest_file, RegeneratePolicy::kRequiresForce},
      {"init/cmake_presets", project_root / "CMakePresets.json", RegeneratePolicy::kRequiresForce},
  };

  if (!opts.force) {
    std::vector<std::string> blocked;
    for (const auto& r : renders) {
      if (r.policy == RegeneratePolicy::kRequiresForce && fs::exists(r.out)) {
        blocked.push_back(fs::relative(r.out, project_root).generic_string());
      }
    }
    if (!blocked.empty()) {
      std::string msg = "Existing project files would be overwritten. Use --force to proceed:";
      for (const auto& path : blocked) {
        msg += "\n  - " + path;
      }
      return Err<void>(ErrorCategory::kIO, msg);
    }
  }

  REXGLUE_TRACE("Creating directory structure...");
  for (const auto& dir : {project_root, project_root / "src", project_root / "generated"}) {
    fs::create_directories(dir, ec);
    if (ec) {
      return Err<void>(ErrorCategory::kIO,
                       std::format("Failed to create {}: {}", dir.string(), ec.message()));
    }
  }

  REXGLUE_TRACE("Generating project files...");
  for (const auto& r : renders) {
    if (r.policy == RegeneratePolicy::kFirstInitOnly && fs::exists(r.out)) {
      REXGLUE_DEBUG("  Skipped {} (preserving user content)",
                    fs::relative(r.out, project_root).generic_string());
      continue;
    }
    if (auto written =
            rex::platform::WriteHostFile(r.out, registry.Render(r.template_id, json_str));
        !written) {
      return Err<void>(ErrorCategory::kIO,
                       "Failed to write " + r.out.string() + ": " + written.error().message);
    }
    REXGLUE_DEBUG("  Created {}", fs::relative(r.out, project_root).generic_string());
  }
  return Ok();
}

Result<void> InitModule(const InitModuleOptions& opts, const CLIContext& ctx) {
  (void)ctx;

  fs::path root = fs::absolute(opts.project_root);
  std::error_code dir_ec;
  fs::path manifest_path;
  for (const auto& entry : fs::directory_iterator(root, dir_ec)) {
    if (entry.path().extension() == ".toml" &&
        rex::codegen::ManifestConfig::IsManifest(entry.path())) {
      manifest_path = entry.path();
      break;
    }
  }
  if (dir_ec) {
    return Err<void>(ErrorCategory::kIO, std::format("Cannot read project root '{}': {}",
                                                     root.string(), dir_ec.message()));
  }
  if (manifest_path.empty()) {
    return Err<void>(ErrorCategory::kConfig,
                     "No manifest found in project root. Run 'rexglue init' first.");
  }

  auto manifest = rex::codegen::ManifestConfig::Load(manifest_path);
  if (!manifest)
    return Err<void>(ErrorCategory::kConfig, "Failed to load manifest");

  fs::path xex_abs = fs::weakly_canonical(fs::absolute(opts.xex_path));
  if (!fs::exists(xex_abs))
    return Err<void>(ErrorCategory::kIO, std::format("XEX file not found: {} (resolved from {})",
                                                     xex_abs.string(), opts.xex_path));
  std::string module_name = ModuleStem(xex_abs);
  std::string xex_rel = fs::relative(xex_abs, root).generic_string();
  std::string guest_path =
      rex::codegen::CanonicalizeModuleGuestPath(opts.guest_path, manifest->project_name);

  toml::table manifest_tbl;
  try {
    manifest_tbl = toml::parse_file(manifest_path.string());
  } catch (const toml::parse_error& err) {
    return Err<void>(ErrorCategory::kConfig, std::format("Manifest parse error: {}", err.what()));
  }
  if (auto* modules_arr = manifest_tbl["modules"].as_array()) {
    for (const auto& mod : *modules_arr) {
      auto* mod_tbl = mod.as_table();
      if (!mod_tbl)
        continue;
      if ((*mod_tbl)["file_path"].value_or<std::string>("") == xex_rel ||
          (*mod_tbl)[rex::codegen::kModuleGuestPathKey].value_or<std::string>("") == guest_path) {
        REXGLUE_TRACE("Manifest already lists this module. Nothing to do.");
        return Ok();
      }
    }
  }

  auto manifest_bytes = rex::platform::ReadHostFile(manifest_path);
  if (!manifest_bytes)
    return Err<void>(ErrorCategory::kIO,
                     "Failed to open manifest for reading: " + manifest_path.string());
  std::string content(manifest_bytes->begin(), manifest_bytes->end());

  if (!content.empty() && content.back() != '\n')
    content.push_back('\n');
  const nlohmann::json entry = {{"m",
                                 {{rex::codegen::kModuleGuestPathKey, guest_path},
                                  {"file_path", xex_rel},
                                  {"out_directory_path", "generated/" + module_name}}}};
  rex::codegen::TemplateRegistry registry;
  if (!opts.template_dir.empty())
    registry.LoadOverrides(opts.template_dir);
  content += "\n" + registry.Render("init/manifest_module", entry.dump());

  REX_TRY_VOID(rex::platform::WriteHostFile(
      manifest_path, std::span(reinterpret_cast<const u8*>(content.data()), content.size())));

  std::vector<ui::KeyValueRow> rows;
  rows.push_back({"file_path", xex_rel});
  rows.push_back({rex::codegen::kModuleGuestPathKey, guest_path});
  ui::KeyValueBlock(std::format("Module '{}' added to manifest:", module_name), rows);
  return Ok();
}

Result<void> InitAchievements(const InitAchievementsOptions& opts, const CLIContext& ctx) {
  std::error_code ec;
  fs::path xex_path = fs::absolute(opts.xex_path, ec);
  if (ec || !fs::exists(xex_path)) {
    return Err<void>(ErrorCategory::kIO, "XEX not found: " + opts.xex_path);
  }
  xex_path = fs::canonical(xex_path, ec);

  fs::path out_dir =
      opts.output_dir.empty() ? xex_path.parent_path() : fs::absolute(opts.output_dir, ec);
  fs::create_directories(out_dir, ec);
  if (ec) {
    return Err<void>(ErrorCategory::kIO, "Cannot create output directory: " + out_dir.string());
  }

  fs::path game_root = xex_path.parent_path();
  fs::path entry_rel = xex_path.filename();
  std::string entry_rel_str = entry_rel.string();

  auto tool = ctx.tools->Open(game_root, entry_rel_str);
  if (!tool) {
    return Err<void>(ErrorCategory::kIO, std::format("Failed to load XEX '{}': {}", opts.xex_path,
                                                     tool.error().message));
  }
  rex::kernel::TitleRuntime* runtime = &tool->GetRuntime();

  const std::unique_ptr<rex::filesystem::IXDBFTitleData> db = runtime->GetTitleXDBF();
  if (!db->IsValid()) {
    return Err<void>(ErrorCategory::kIO, "No XDBF resource found in XEX (title_id section absent)");
  }

  const rex::kernel::XLanguage lang =
      db->GetSupportedLanguage(rex::kernel::ToXLanguage(opts.language));
  const auto achievements = db->GetAchievements();
  const u32 title_id = runtime->GetTitleID();

  auto escape_toml = [](const std::string& s) -> std::string {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
      unsigned char ch = static_cast<unsigned char>(c);
      if (c == '"')
        out += "\\\"";
      else if (c == '\\')
        out += "\\\\";
      else if (c == '\b')
        out += "\\b";
      else if (c == '\t')
        out += "\\t";
      else if (c == '\n')
        out += "\\n";
      else if (c == '\f')
        out += "\\f";
      else if (c == '\r')
        out += "\\r";
      else if (ch < 0x20 || ch == 0x7F)
        out += std::format("\\u{:04X}", ch);
      else
        out += c;
    }
    return out;
  };

  std::string content;
  content += std::format("# Achievements metadata, generated by rexglue\n");
  content += std::format("# Source:    {}\n", xex_path.filename().string());
  content += std::format("# Title ID:  {:08X}\n", title_id);
  content += std::format("# Language:  {}\n", static_cast<u32>(lang));
  content += std::format("# Generated: {}\n", IsoUtcStamp());
  content += std::format("# {} achievements\n\n", achievements.size());

  for (const auto& entry : achievements) {
    content += "[[achievements]]\n";
    content += std::format("id                    = {}\n", static_cast<u32>(entry.id));
    content += std::format("label                 = \"{}\"\n",
                           escape_toml(db->GetStringTableEntry(lang, entry.label_id)));
    content += std::format("description           = \"{}\"\n",
                           escape_toml(db->GetStringTableEntry(lang, entry.description_id)));
    content += std::format("unachieved_description = \"{}\"\n",
                           escape_toml(db->GetStringTableEntry(lang, entry.unachieved_id)));
    content += std::format("gamerscore            = {}\n", static_cast<u32>(entry.gamerscore));
    content += std::format("image_id              = {}\n", static_cast<u32>(entry.image_id));
    content +=
        std::format("icon_path             = \"icons/{}.png\"\n", static_cast<u32>(entry.image_id));
    content += std::format("flags                 = {}\n\n", static_cast<u32>(entry.flags));
  }

  fs::path out_path = out_dir / "achievements.toml";
  if (!rex::platform::WriteHostFile(out_path, content)) {
    return Err<void>(ErrorCategory::kIO, "Write error: " + out_path.string());
  }

  REXGLUE_INFO("Wrote {} achievements to {}", achievements.size(), out_path.string());

  fs::path icons_dir = out_dir / "icons";
  fs::create_directories(icons_dir, ec);
  if (ec) {
    REXGLUE_WARN("Could not create icons directory: {}", ec.message());
  } else {
    int icons_written = 0;
    int icons_missing = 0;
    std::vector<u32> seen;

    for (const auto& entry : achievements) {
      u32 img_id = entry.image_id;
      if (std::find(seen.begin(), seen.end(), img_id) != seen.end())
        continue;
      seen.push_back(img_id);

      auto block = db->GetEntry(rex::filesystem::XDBFSection::kImage, static_cast<u64>(img_id));
      if (!block || !block.buffer || block.size == 0) {
        ++icons_missing;
        continue;
      }

      fs::path icon_path = icons_dir / std::format("{}.png", img_id);
      if (!rex::platform::WriteHostFile(icon_path, std::span<const u8>(block.buffer, block.size))) {
        REXGLUE_WARN("Cannot write icon {}", icon_path.string());
        continue;
      }
      ++icons_written;
    }

    if (icons_written > 0) {
      REXGLUE_INFO("Wrote {} icon(s) to {}", icons_written, icons_dir.string());
    }
    if (icons_missing > 0) {
      REXGLUE_WARN("{} achievement(s) had no icon in XDBF", icons_missing);
    }

    if (auto game_icon = db->GetIcon(); game_icon && game_icon.size > 0) {
      fs::path game_icon_path = icons_dir / "title.png";
      if (rex::platform::WriteHostFile(game_icon_path,
                                       std::span<const u8>(game_icon.buffer, game_icon.size))) {
        REXGLUE_INFO("Wrote game icon to {}", game_icon_path.string());
      } else {
        REXGLUE_WARN("Cannot write game icon {}", game_icon_path.string());
      }
    } else {
      REXGLUE_WARN("No game icon found in XDBF");
    }
  }

  return Ok();
}

namespace {

struct InitCommandOptions {
  std::string project_name;
  std::string project_root;
  std::string xex_path;
  std::string game_root;
  std::string template_dir;
  bool scan_dlls = false;
};

struct InitModuleCommandOptions {
  std::string project_root;
  std::string xex_path;
  std::string guest_path;
};

struct InitAchievementsCommandOptions {
  std::string xex_path;
  std::string output_dir;
  u32 language = std::to_underlying(rex::kernel::XLanguage::kEnglish);
};

}  // namespace

void RegisterInit(CLI::App& parent, const CLIContext& ctx, DeferredAction& pending) {
  auto* init = parent.add_subcommand("init", "Initialize a new project")->fallthrough();
  auto args = std::make_shared<InitCommandOptions>();
  init->add_option("--project-name", args->project_name,
                   "Project name (becomes [project].name in the manifest)")
      ->type_name("NAME");
  init->add_option("--xex-path", args->xex_path, "Path to entrypoint XEX (e.g. assets/Default.xex)")
      ->type_name("PATH");
  init->add_option("--game-root", args->game_root, "Game asset root for DLL guest-path derivation")
      ->type_name("PATH");
  init->add_option("--project-root", args->project_root,
                   "Where to create the project (defaults to current directory)")
      ->type_name("PATH");
  init->add_flag("--scan-dll", args->scan_dlls,
                 "Scan --game-root for .dll files and add each as a [[modules]] entry");
  init->add_option("--template-dir", args->template_dir, "Custom template directory for overrides")
      ->type_name("PATH");

  auto* mod =
      init->add_subcommand("module", "Add a DLL module to an existing project")->fallthrough();
  auto mod_args = std::make_shared<InitModuleCommandOptions>();
  mod->add_option("--project-root", mod_args->project_root, "Project root containing the manifest")
      ->type_name("PATH")
      ->required();
  mod->add_option("--xex-path", mod_args->xex_path, "Path to the DLL XEX")
      ->type_name("PATH")
      ->required();
  mod->add_option("--guest-path", mod_args->guest_path, "Guest path for XexLoadImage matching")
      ->type_name("PATH")
      ->required();

  init->callback([args, &ctx, &pending]() {
    if (pending)
      return;
    if (args->project_name.empty())
      throw CLI::RequiredError("--project-name");
    if (args->xex_path.empty())
      throw CLI::RequiredError("--xex-path");
    pending = [args, &ctx]() -> Result<void> {
      InitOptions opts;
      opts.project_name = args->project_name;
      opts.project_root = args->project_root;
      opts.xex_path = args->xex_path;
      opts.game_root = args->game_root;
      opts.scan_dlls = args->scan_dlls;
      opts.template_dir = args->template_dir;
      opts.force = ctx.overwrite_existing;
      return InitProject(opts, ctx);
    };
  });
  mod->callback([args, mod_args, &ctx, &pending]() {
    pending = [args, mod_args, &ctx]() -> Result<void> {
      InitModuleOptions opts;
      opts.project_root = mod_args->project_root;
      opts.xex_path = mod_args->xex_path;
      opts.guest_path = mod_args->guest_path;
      opts.template_dir = args->template_dir;
      return InitModule(opts, ctx);
    };
  });

  auto* ach = init->add_subcommand("achievements", "Extract achievement metadata from XEX to TOML")
                  ->fallthrough();
  auto ach_args = std::make_shared<InitAchievementsCommandOptions>();
  ach->add_option("xex_path", ach_args->xex_path, "Path to the XEX binary")->required();
  ach->add_option("output_dir", ach_args->output_dir,
                  "Output directory for achievements.toml (default: XEX directory)");
  ach->add_option("--language", ach_args->language,
                  "Language ID to extract strings for (1=English, 2=Japanese, 3=German, "
                  "4=French, 5=Spanish, 6=Italian, 7=Korean, 8=TChinese, 9=Portuguese, "
                  "11=Polish, 12=Russian, 13=Swedish, 14=Turkish, 15=Norwegian, 16=Dutch, "
                  "17=SChinese. Default is English)");
  ach->callback([ach_args, &ctx, &pending]() {
    pending = [ach_args, &ctx]() -> Result<void> {
      InitAchievementsOptions opts;
      opts.xex_path = ach_args->xex_path;
      opts.output_dir = ach_args->output_dir;
      opts.language = ach_args->language;
      return InitAchievements(opts, ctx);
    };
  });
}

}  // namespace rexglue::cli
