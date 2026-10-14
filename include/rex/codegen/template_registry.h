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

namespace rex::codegen {

/** Exception thrown on template parse or render errors. */
class TemplateError : public std::runtime_error {
 public:
  /**
   * Creates an error for 1 template.
   *
   * @param template_id Canonical ID of the failing template
   * @param message Description of the failure
   * @param source Where the template came from, "embedded" or an override file path
   */
  TemplateError(const std::string& template_id, const std::string& message,
                const std::string& source = "embedded");

  const std::string& GetTemplateId() const { return template_id_; }
  const std::string& GetSource() const { return source_; }

 private:
  std::string template_id_;
  std::string source_;
};

/**
 * Resolves and renders inja templates by canonical ID.
 * Supports embedded defaults with optional filesystem overrides.
 *
 * Uses pimpl to keep inja.hpp and nlohmann/json.hpp out of this header, so Render() accepts
 * serialized JSON in a std::string and nlohmann::json stays out of the public API.
 */
class TemplateRegistry : public rex::MoveOnly {
 public:
  /** Creates a registry holding the embedded templates. */
  TemplateRegistry();
  /** Destroys the registry. */
  ~TemplateRegistry();

  /**
   * Takes ownership of the templates of another registry.
   *
   * @param other Registry to take from, left empty.
   */
  TemplateRegistry(TemplateRegistry&&) noexcept;
  /**
   * Releases the owned templates, then takes those of another registry.
   *
   * @param other Registry to take from, left empty.
   * @return This registry
   */
  TemplateRegistry& operator=(TemplateRegistry&&) noexcept;

  /**
   * Loads overrides from a directory. Files that do not match a known canonical ID produce a
   * warning log.
   *
   * @param dir Directory holding the override files
   */
  void LoadOverrides(const std::filesystem::path& dir);

  /**
   * Renders a template by canonical ID.
   *
   * @param id Canonical template ID
   * @param json_data Template data as serialized JSON
   * @return Rendered text
   * @throws TemplateError on parse or render failure
   */
  std::string Render(const std::string& id, const std::string& json_data);

  /**
   * Renders a raw template string for testing. Not tied to a canonical ID.
   *
   * @param template_content Template source
   * @param json_data Template data as serialized JSON
   * @return Rendered text
   */
  std::string RenderString(const std::string& template_content, const std::string& json_data);

  /**
   * Lists all registered canonical IDs.
   *
   * @return Registered IDs
   */
  std::vector<std::string> GetRegisteredIds() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/**
 * Hashes every embedded template with SHA256, ordered by ID. Fold it into any cache key built
 * from the SDK version, which is stable across dev builds, so a template edit alone would not
 * otherwise invalidate the key.
 *
 * @return Hash of the embedded templates
 */
std::string GetEmbeddedTemplatesHash();

}  // namespace rex::codegen
