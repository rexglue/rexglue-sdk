/**
 * @file        rexglue/commands/init_command.cpp
 * @brief       Project initialization command implementation
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#include "init_command.h"
#include "../ui/ui.h"
#include "template_utils.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <vector>

#include <rex/codegen/manifest.h>
#include <rex/codegen/template_registry.h>
#include <rex/kernel/init.h>
#include <rex/logging.h>
#include <rex/result.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/xdbf_utils.h>
#include <rex/version.h>

#include <CLI/CLI.hpp>
#include <fmt/chrono.h>
#include <nlohmann/json.hpp>
#include <toml++/toml.hpp>

namespace fs = std::filesystem;

namespace rexglue::cli {

using rex::Err;
using rex::ErrorCategory;
using rex::Ok;

namespace {

std::string LowercaseAscii(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::string ManifestPath(const fs::path& target, const fs::path& base) {
  std::error_code ec;
  fs::path rel = fs::relative(target, base, ec);
  if (ec || rel.empty()) {
    return LowercaseAscii(target.generic_string());
  }
  return LowercaseAscii(rel.generic_string());
}

std::string ModuleStem(const fs::path& xex) {
  std::string stem = LowercaseAscii(xex.stem().string());
  std::replace(stem.begin(), stem.end(), '.', '_');
  std::replace(stem.begin(), stem.end(), ' ', '_');
  return stem;
}

std::string IsoUtcStamp() {
  auto now = std::chrono::system_clock::now();
  auto t = std::chrono::system_clock::to_time_t(now);
  return fmt::format("{:%Y-%m-%d %H:%M:%S} UTC", fmt::gmtime(t));
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

Result<void> InitProject(const InitOptions& opts, const CliContext& ctx) {
  (void)ctx;

  if (opts.project_name.empty())
    return Err<void>(ErrorCategory::Config, "--project_name is required");
  if (opts.xex_path.empty())
    return Err<void>(ErrorCategory::Config, "--xex_path is required (path to entrypoint XEX)");

  std::string validation_error;
  if (!validate_app_name(opts.project_name, validation_error))
    return Err<void>(ErrorCategory::Config, validation_error);
  auto names = parse_app_name(opts.project_name);

  std::error_code ec;
  fs::path projectRoot =
      opts.project_root.empty() ? fs::current_path(ec) : ResolveDir(opts.project_root, ec);
  if (ec)
    return Err<void>(ErrorCategory::IO, "Failed to resolve project root: " + ec.message());

  fs::path xexAbs = ResolveDir(opts.xex_path, ec);
  if (ec)
    return Err<void>(ErrorCategory::IO,
                     "Failed to resolve --xex_path '" + opts.xex_path + "': " + ec.message());
  if (!fs::exists(xexAbs))
    return Err<void>(ErrorCategory::IO, "Entrypoint XEX not found: " + xexAbs.string());
  if (!fs::is_regular_file(xexAbs))
    return Err<void>(ErrorCategory::IO, "--xex_path is not a regular file: " + xexAbs.string());

  std::string xexStem = xexAbs.stem().string();
  if (xexStem.empty())
    return Err<void>(ErrorCategory::Config, "--xex_path has no filename: " + opts.xex_path);

  fs::path gameRootAbs;
  if (opts.game_root.empty()) {
    gameRootAbs = xexAbs.parent_path();
  } else {
    gameRootAbs = ResolveDir(opts.game_root, ec);
    if (ec)
      return Err<void>(ErrorCategory::IO,
                       "Failed to resolve --game_root '" + opts.game_root + "': " + ec.message());
  }
  if (!fs::exists(gameRootAbs) || !fs::is_directory(gameRootAbs))
    return Err<void>(ErrorCategory::IO, "--game_root is not a directory: " + gameRootAbs.string());

  fs::path xexRelToGame = fs::relative(xexAbs, gameRootAbs, ec);
  if (ec || xexRelToGame.empty() || *xexRelToGame.begin() == fs::path("..")) {
    return Err<void>(ErrorCategory::Config, "--xex_path (" + xexAbs.string() +
                                                ") is not inside --game_root (" +
                                                gameRootAbs.string() + ")");
  }

  std::string outDir = LowercaseAscii("generated/" + xexStem);
  std::string xexRelManifest = ManifestPath(xexAbs, projectRoot);
  std::string gameRootRelManifest = ManifestPath(gameRootAbs, projectRoot);

  nlohmann::json modulesJson = nlohmann::json::array();
  if (opts.scan_dlls) {
    std::vector<fs::path> dllPaths;
    fs::recursive_directory_iterator it(gameRootAbs, fs::directory_options::skip_permission_denied,
                                        ec);
    fs::recursive_directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
      if (!it->is_regular_file())
        continue;
      if (LowercaseAscii(it->path().extension().string()) != ".dll")
        continue;
      dllPaths.push_back(it->path());
    }
    std::sort(dllPaths.begin(), dllPaths.end());

    for (const auto& dllAbs : dllPaths) {
      fs::path relUnderGame = fs::relative(dllAbs, gameRootAbs, ec);
      if (ec || relUnderGame.empty()) {
        ec.clear();
        continue;
      }
      modulesJson.push_back({
          {"guest_path", rex::codegen::CanonicalizeModuleGuestPath(relUnderGame.generic_string(),
                                                                   names.original)},
          {"file_path", ManifestPath(dllAbs, projectRoot)},
          {"out_directory_path", "generated/" + ModuleStem(dllAbs)},
      });
    }
  }

  rex::codegen::TemplateRegistry registry;
  if (!opts.template_dir.empty())
    registry.loadOverrides(opts.template_dir);

  nlohmann::json data = {
      {"names", names_to_json(names)},
      {"sdk_version", REXGLUE_VERSION_FLOOR},
      {"sdk_version_full", REXGLUE_VERSION_STRING},
      {"generated_on", IsoUtcStamp()},
      {"include_stamp", true},
      {"xex_path", xexRelManifest},
      {"game_root", gameRootRelManifest},
      {"out_directory_path", outDir},
      {"entrypoint_out_dir", outDir},
      {"modules", modulesJson},
  };
  std::string jsonStr = data.dump();

  std::vector<ui::KeyValueRow> header_rows;
  header_rows.push_back({"Project", names.original});
  header_rows.push_back({"Root", projectRoot.string()});
  header_rows.push_back({"Entrypoint", xexRelManifest});
  header_rows.push_back({"Game root", gameRootRelManifest});
  if (opts.scan_dlls) {
    header_rows.push_back({"DLL modules", std::to_string(modulesJson.size())});
  }
  ui::KeyValueBlock("Initializing project:", header_rows);

  if (fs::exists(projectRoot) && !fs::is_directory(projectRoot)) {
    return Err<void>(ErrorCategory::IO,
                     "Path exists but is not a directory: " + projectRoot.string());
  }

  enum class RegeneratePolicy { AlwaysRegenerate, FirstInitOnly, RequiresForce };
  struct Render {
    std::string template_id;
    fs::path out;
    RegeneratePolicy policy;
  };
  std::string app_header = names.original + "_app.h";
  std::string manifest_file = names.original + "_manifest.toml";
  std::vector<Render> renders = {
      {"init/cmakelists", projectRoot / "CMakeLists.txt", RegeneratePolicy::RequiresForce},
      {"init/rexglue_cmake", projectRoot / "generated" / "rexglue.cmake",
       RegeneratePolicy::AlwaysRegenerate},
      {"init/main_cpp", projectRoot / "src" / "main.cpp", RegeneratePolicy::FirstInitOnly},
      {"init/app_header", projectRoot / "src" / app_header, RegeneratePolicy::FirstInitOnly},
      {"init/manifest_toml", projectRoot / manifest_file, RegeneratePolicy::RequiresForce},
      {"init/cmake_presets", projectRoot / "CMakePresets.json", RegeneratePolicy::RequiresForce},
  };

  if (!opts.force) {
    std::vector<std::string> blocked;
    for (const auto& r : renders) {
      if (r.policy == RegeneratePolicy::RequiresForce && fs::exists(r.out)) {
        blocked.push_back(fs::relative(r.out, projectRoot).generic_string());
      }
    }
    if (!blocked.empty()) {
      std::string msg = "Existing project files would be overwritten. Use --force to proceed:";
      for (const auto& path : blocked) {
        msg += "\n  - " + path;
      }
      return Err<void>(ErrorCategory::IO, msg);
    }
  }

  REXLOG_TRACE("Creating directory structure...");
  for (const auto& dir : {projectRoot, projectRoot / "src", projectRoot / "generated"}) {
    fs::create_directories(dir, ec);
    if (ec) {
      return Err<void>(ErrorCategory::IO,
                       fmt::format("Failed to create {}: {}", dir.string(), ec.message()));
    }
  }

  REXLOG_TRACE("Generating project files...");
  for (const auto& r : renders) {
    if (r.policy == RegeneratePolicy::FirstInitOnly && fs::exists(r.out)) {
      REXLOG_DEBUG("  Skipped {} (preserving user content)",
                   fs::relative(r.out, projectRoot).generic_string());
      continue;
    }
    if (!write_file_atomic(r.out, registry.render(r.template_id, jsonStr))) {
      return Err<void>(ErrorCategory::IO, "Failed to write " + r.out.string());
    }
    REXLOG_DEBUG("  Created {}", fs::relative(r.out, projectRoot).generic_string());
  }
  return Ok();
}

Result<void> InitModule(const InitModuleOptions& opts, const CliContext& ctx) {
  (void)ctx;

  fs::path root = fs::absolute(opts.app_root);
  std::error_code dir_ec;
  fs::path manifestPath;
  for (const auto& entry : fs::directory_iterator(root, dir_ec)) {
    if (entry.path().extension() == ".toml" &&
        rex::codegen::ManifestConfig::IsManifest(entry.path())) {
      manifestPath = entry.path();
      break;
    }
  }
  if (dir_ec) {
    return Err<void>(ErrorCategory::IO, fmt::format("Cannot read project root '{}': {}",
                                                    root.string(), dir_ec.message()));
  }
  if (manifestPath.empty()) {
    return Err<void>(ErrorCategory::Config,
                     "No manifest found in project root. Run 'rexglue init' first.");
  }

  auto manifest = rex::codegen::ManifestConfig::Load(manifestPath);
  if (!manifest)
    return Err<void>(ErrorCategory::Config, "Failed to load manifest");

  fs::path xexAbs = fs::weakly_canonical(fs::absolute(opts.xex_path));
  if (!fs::exists(xexAbs))
    return Err<void>(ErrorCategory::IO, fmt::format("XEX file not found: {} (resolved from {})",
                                                    xexAbs.string(), opts.xex_path));
  std::string moduleName = ModuleStem(xexAbs);
  std::string xexRel = fs::relative(xexAbs, root).generic_string();
  std::string guestPath =
      rex::codegen::CanonicalizeModuleGuestPath(opts.guest_path, manifest->projectName);

  toml::table manifestTbl;
  try {
    manifestTbl = toml::parse_file(manifestPath.string());
  } catch (const toml::parse_error& err) {
    return Err<void>(ErrorCategory::Config, fmt::format("Manifest parse error: {}", err.what()));
  }
  if (auto* modulesArr = manifestTbl["modules"].as_array()) {
    for (const auto& mod : *modulesArr) {
      auto* modTbl = mod.as_table();
      if (!modTbl)
        continue;
      if ((*modTbl)["file_path"].value_or<std::string>("") == xexRel ||
          (*modTbl)["guest_path"].value_or<std::string>("") == guestPath) {
        REXLOG_TRACE("Manifest already lists this module; nothing to do.");
        return Ok();
      }
    }
  }

  std::ifstream in(manifestPath);
  if (!in)
    return Err<void>(ErrorCategory::IO,
                     "Failed to open manifest for reading: " + manifestPath.string());
  std::stringstream ss;
  ss << in.rdbuf();
  std::string content = ss.str();
  in.close();

  if (!content.empty() && content.back() != '\n')
    content.push_back('\n');
  content += fmt::format(
      "\n[[modules]]\nguest_path = \"{}\"\nfile_path = \"{}\"\nout_directory_path = "
      "\"generated/{}\"\nincludes = []\n",
      guestPath, xexRel, moduleName);

  fs::path tmpPath = manifestPath;
  tmpPath += ".tmp";
  {
    std::ofstream out(tmpPath, std::ios::binary);
    if (!out)
      return Err<void>(ErrorCategory::IO,
                       "Failed to open manifest tmp for writing: " + tmpPath.string());
    out << content;
    if (!out.good()) {
      std::error_code ignore;
      fs::remove(tmpPath, ignore);
      return Err<void>(ErrorCategory::IO, "Failed while writing manifest tmp: " + tmpPath.string());
    }
  }
  std::error_code ec;
  fs::rename(tmpPath, manifestPath, ec);
  if (ec) {
    std::error_code ignore;
    fs::remove(tmpPath, ignore);
    return Err<void>(ErrorCategory::IO,
                     "Failed to rename manifest tmp into place: " + ec.message());
  }

  std::vector<ui::KeyValueRow> rows;
  rows.push_back({"file_path", xexRel});
  rows.push_back({"guest_path", guestPath});
  ui::KeyValueBlock(fmt::format("Module '{}' added to manifest:", moduleName), rows);
  return Ok();
}

Result<void> InitAchievements(const InitAchievementsOptions& opts, const CliContext& /*ctx*/) {
  std::error_code ec;
  fs::path xexPath = fs::absolute(opts.xex_path, ec);
  if (ec || !fs::exists(xexPath)) {
    return Err<void>(ErrorCategory::IO, "XEX not found: " + opts.xex_path);
  }
  xexPath = fs::canonical(xexPath, ec);

  fs::path outDir =
      opts.output_dir.empty() ? xexPath.parent_path() : fs::absolute(opts.output_dir, ec);
  fs::create_directories(outDir, ec);
  if (ec) {
    return Err<void>(ErrorCategory::IO, "Cannot create output directory: " + outDir.string());
  }

  // Build VFS path: game:\ maps to the XEX's parent directory.
  fs::path gameRoot = xexPath.parent_path();
  fs::path entryRel = xexPath.filename();
  std::string entryRelStr = entryRel.string();
  auto entryVfsPath = "game:\\" + entryRelStr;

  auto runtime = std::make_unique<rex::Runtime>(gameRoot.string());
  auto rtStatus = runtime->Setup(rex::RuntimeConfig{
      .kernel_init = rex::kernel::InitializeKernel,
      .tool_mode = true,
  });
  if (!XSUCCEEDED(rtStatus)) {
    return Err<void>(ErrorCategory::IO, fmt::format("Runtime init failed: {:#x}", rtStatus));
  }

  rtStatus = runtime->LoadXexImage(entryVfsPath);
  if (!XSUCCEEDED(rtStatus)) {
    return Err<void>(ErrorCategory::IO,
                     fmt::format("Failed to load XEX '{}': {:#x}", opts.xex_path, rtStatus));
  }

  const rex::system::util::XdbfGameData db = runtime->kernel_state()->title_xdbf();
  if (!db.is_valid()) {
    return Err<void>(ErrorCategory::IO, "No XDBF resource found in XEX (title_id section absent)");
  }

  const rex::system::XLanguage lang =
      db.GetExistingLanguage(static_cast<rex::system::XLanguage>(opts.language));
  const auto achievements = db.GetAchievements();
  const uint32_t title_id = runtime->kernel_state()->title_id();

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
        out += fmt::format("\\u{:04X}", ch);
      else
        out += c;
    }
    return out;
  };

  std::string content;
  content += fmt::format("# Achievements metadata \xe2\x80\x94 auto-generated by rexglue\n");
  content += fmt::format("# Source:    {}\n", xexPath.filename().string());
  content += fmt::format("# Title ID:  {:08X}\n", title_id);
  content += fmt::format("# Language:  {}\n", static_cast<uint32_t>(lang));
  content += fmt::format("# Generated: {}\n", IsoUtcStamp());
  content += fmt::format("# {} achievements\n\n", achievements.size());

  for (const auto& entry : achievements) {
    content += "[[achievements]]\n";
    content += fmt::format("id                    = {}\n", uint32_t(entry.id));
    content += fmt::format("label                 = \"{}\"\n",
                           escape_toml(db.GetStringTableEntry(lang, entry.label_id)));
    content += fmt::format("description           = \"{}\"\n",
                           escape_toml(db.GetStringTableEntry(lang, entry.description_id)));
    content += fmt::format("unachieved_description = \"{}\"\n",
                           escape_toml(db.GetStringTableEntry(lang, entry.unachieved_id)));
    content += fmt::format("gamerscore            = {}\n", uint32_t(entry.gamerscore));
    content += fmt::format("image_id              = {}\n", uint32_t(entry.image_id));
    content += fmt::format("icon_path             = \"icons/{}.png\"\n", uint32_t(entry.image_id));
    content += fmt::format("flags                 = {}\n\n", uint32_t(entry.flags));
  }

  fs::path outPath = outDir / "achievements.toml";
  {
    std::ofstream out(outPath, std::ios::binary);
    if (!out) {
      return Err<void>(ErrorCategory::IO, "Failed to open for writing: " + outPath.string());
    }
    out << content;
    if (!out.good()) {
      return Err<void>(ErrorCategory::IO, "Write error: " + outPath.string());
    }
  }

  REXLOG_INFO("Wrote {} achievements to {}", achievements.size(), outPath.string());

  // ---- Icons ---------------------------------------------------------------
  fs::path iconsDir = outDir / "icons";
  fs::create_directories(iconsDir, ec);
  if (ec) {
    REXLOG_WARN("Could not create icons directory: {}", ec.message());
  } else {
    int icons_written = 0;
    int icons_missing = 0;
    std::vector<uint32_t> seen;

    for (const auto& entry : achievements) {
      uint32_t img_id = entry.image_id;
      if (std::find(seen.begin(), seen.end(), img_id) != seen.end())
        continue;
      seen.push_back(img_id);

      auto block =
          db.GetEntry(rex::system::util::XdbfSection::kImage, static_cast<uint64_t>(img_id));
      if (!block || !block.buffer || block.size == 0) {
        ++icons_missing;
        continue;
      }

      fs::path icon_path = iconsDir / fmt::format("{}.png", img_id);
      std::ofstream f(icon_path, std::ios::binary);
      if (!f) {
        REXLOG_WARN("Cannot write icon {}", icon_path.string());
        continue;
      }
      f.write(reinterpret_cast<const char*>(block.buffer),
              static_cast<std::streamsize>(block.size));
      if (f.good())
        ++icons_written;
    }

    if (icons_written > 0) {
      REXLOG_INFO("Wrote {} icon(s) to {}", icons_written, iconsDir.string());
    }
    if (icons_missing > 0) {
      REXLOG_WARN("{} achievement(s) had no icon in XDBF", icons_missing);
    }

    // Game title icon (not tied to any achievement).
    if (auto game_icon = db.icon(); game_icon && game_icon.size > 0) {
      fs::path game_icon_path = iconsDir / "title.png";
      std::ofstream f(game_icon_path, std::ios::binary);
      if (f) {
        f.write(reinterpret_cast<const char*>(game_icon.buffer),
                static_cast<std::streamsize>(game_icon.size));
      }
      if (f && f.good()) {
        REXLOG_INFO("Wrote game icon to {}", game_icon_path.string());
      } else {
        REXLOG_WARN("Cannot write game icon {}", game_icon_path.string());
      }
    } else {
      REXLOG_WARN("No game icon found in XDBF");
    }
  }

  return Ok();
}

namespace {

struct InitArgs {
  std::string project_name;
  std::string project_root;
  std::string xex_path;
  std::string game_root;
  std::string template_dir;
  bool scan_dll = false;
};

struct InitModuleArgs {
  std::string app_root;
  std::string xex_path;
  std::string guest_path;
};

struct InitAchievementsArgs {
  std::string xex_path;
  std::string output_dir;
  uint32_t language = static_cast<uint32_t>(rex::system::XLanguage::kEnglish);
};

}  // namespace

void RegisterInit(CLI::App& parent, const CliContext& ctx, DeferredAction& pending) {
  auto* init = parent.add_subcommand("init", "Initialize a new project")->fallthrough();
  auto args = std::make_shared<InitArgs>();
  init->add_option("--project-name", args->project_name,
                   "Project name (becomes [project].name in the manifest)")
      ->type_name("NAME")
      ->required();
  init->add_option("--xex-path", args->xex_path, "Path to entrypoint XEX (e.g. assets/Default.xex)")
      ->type_name("PATH")
      ->required();
  init->add_option("--game-root", args->game_root, "Game asset root for DLL guest-path derivation")
      ->type_name("PATH");
  init->add_option("--project-root", args->project_root,
                   "Where to create the project (defaults to current directory)")
      ->type_name("PATH");
  init->add_flag("--scan-dll", args->scan_dll,
                 "Scan --game-root for .dll files and add each as a [[modules]] entry");
  init->add_option("--template-dir", args->template_dir, "Custom template directory for overrides")
      ->type_name("PATH");

  auto* mod =
      init->add_subcommand("module", "Add a DLL module to an existing project")->fallthrough();
  auto modArgs = std::make_shared<InitModuleArgs>();
  mod->add_option("--project-root", modArgs->app_root, "Project root containing the manifest")
      ->type_name("PATH")
      ->required();
  mod->add_option("--xex-path", modArgs->xex_path, "Path to the DLL XEX")
      ->type_name("PATH")
      ->required();
  mod->add_option("--guest-path", modArgs->guest_path, "Guest path for XexLoadImage matching")
      ->type_name("PATH")
      ->required();

  init->callback([args, &ctx, &pending]() {
    if (pending)
      return;  // a subcommand already claimed the action
    pending = [args, &ctx]() -> Result<void> {
      InitOptions opts;
      opts.project_name = args->project_name;
      opts.project_root = args->project_root;
      opts.xex_path = args->xex_path;
      opts.game_root = args->game_root;
      opts.scan_dlls = args->scan_dll;
      opts.template_dir = args->template_dir;
      opts.force = ctx.overwrite_existing;
      return InitProject(opts, ctx);
    };
  });
  mod->callback([modArgs, &ctx, &pending]() {
    pending = [modArgs, &ctx]() -> Result<void> {
      InitModuleOptions opts;
      opts.app_root = modArgs->app_root;
      opts.xex_path = modArgs->xex_path;
      opts.guest_path = modArgs->guest_path;
      return InitModule(opts, ctx);
    };
  });

  auto* ach = init->add_subcommand("achievements", "Extract achievement metadata from XEX to TOML")
                  ->fallthrough();
  auto achArgs = std::make_shared<InitAchievementsArgs>();
  ach->add_option("xex_path", achArgs->xex_path, "Path to the XEX binary")->required();
  ach->add_option("output_dir", achArgs->output_dir,
                  "Output directory for achievements.toml (default: XEX directory)");
  ach->add_option("--language", achArgs->language,
                  "Language ID to extract strings for (1=English, 2=Japanese, 3=German, "
                  "4=French, 5=Spanish, 6=Italian, 7=Korean, 8=TChinese, 9=Portuguese, "
                  "10=SChinese, 11=Polish, 12=Russian; default: English)");
  ach->callback([achArgs, &ctx, &pending]() {
    pending = [achArgs, &ctx]() -> Result<void> {
      InitAchievementsOptions opts;
      opts.xex_path = achArgs->xex_path;
      opts.output_dir = achArgs->output_dir;
      opts.language = achArgs->language;
      return InitAchievements(opts, ctx);
    };
  });
}

}  // namespace rexglue::cli
