/**
 * @file        codegen/template_registry.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <stdexcept>

#include <rex/types.h>
#include <rex/utility.h>

namespace rex::codegen {

class TemplateError : public std::runtime_error {
 public:
  /**
   * @param template_id Canonical ID of the failing template.
   * @param source Where the template came from, "embedded" or an override file path.
   */
  TemplateError(const std::string& template_id, const std::string& message,
                const std::string& source = "embedded");

  const std::string& GetTemplateID() const { return template_id_; }
  const std::string& GetSource() const { return source_; }

 private:
  std::string template_id_;
  std::string source_;
};

/**
 * Resolves and renders inja templates by canonical ID, with embedded defaults and optional
 * filesystem overrides. Render() takes serialized JSON so inja.hpp and nlohmann/json.hpp stay out
 * of this header.
 */
class TemplateRegistry : public rex::MoveOnly {
 public:
  TemplateRegistry();
  ~TemplateRegistry();

  TemplateRegistry(TemplateRegistry&&) noexcept;
  TemplateRegistry& operator=(TemplateRegistry&&) noexcept;

  /**
   * Loads overrides from a directory. Files that do not match a known canonical ID log a warning.
   */
  void LoadOverrides(const std::filesystem::path& dir);

  /**
   * Renders a template by canonical ID.
   *
   * @param json_data Template data as serialized JSON.
   * @throws TemplateError on parse or render failure.
   */
  std::string Render(const std::string& id, const std::string& json_data);

  /**
   * Renders a raw template string for testing. Not tied to a canonical ID.
   *
   * @param json_data Template data as serialized JSON.
   */
  std::string RenderString(const std::string& template_content, const std::string& json_data);

  /** Lists all registered canonical IDs. */
  std::vector<std::string> GetRegisteredIDs() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/** Returns the SHA256 hash of every embedded template, ordered by ID. */
std::string GetEmbeddedTemplatesHash();

}  // namespace rex::codegen
