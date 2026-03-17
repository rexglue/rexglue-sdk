/**
 * @file        rex/codegen/template_registry.h
 * @brief       Template registry for inja-based code generation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */

#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <stdexcept>

namespace rex::codegen {

/// Exception thrown on template parse or render errors.
class TemplateError : public std::runtime_error {
 public:
  TemplateError(const std::string& templateId, const std::string& message,
                const std::string& source = "embedded");

  const std::string& templateId() const { return templateId_; }
  const std::string& source() const { return source_; }

 private:
  std::string templateId_;
  std::string source_;
};

/// Resolves and renders inja templates by canonical ID.
/// Supports embedded defaults with optional filesystem overrides.
///
/// Uses pimpl to keep inja.hpp and nlohmann/json.hpp out of this header.
/// The render() method accepts serialized JSON (const std::string&) to avoid
/// exposing nlohmann::json in the public API.
class TemplateRegistry {
 public:
  TemplateRegistry();
  ~TemplateRegistry();

  TemplateRegistry(const TemplateRegistry&) = delete;
  TemplateRegistry& operator=(const TemplateRegistry&) = delete;
  TemplateRegistry(TemplateRegistry&&) noexcept;
  TemplateRegistry& operator=(TemplateRegistry&&) noexcept;

  /// Load overrides from a directory. Files that don't match
  /// a known canonical ID produce a warning log.
  void loadOverrides(const std::filesystem::path& dir);

  /// Render a template by canonical ID with JSON data (as serialized string).
  /// @throws TemplateError on parse or render failure
  std::string render(const std::string& id, const std::string& jsonData);

  /// Render from raw template string (for testing). Not tied to a canonical ID.
  std::string renderString(const std::string& templateContent, const std::string& jsonData);

  /// List all registered canonical IDs.
  std::vector<std::string> registeredIds() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rex::codegen
