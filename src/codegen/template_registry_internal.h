/**
 * @file        codegen/template_registry_internal.h
 * @brief       Internal json-native render for TemplateRegistry
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */

#pragma once

#include <rex/codegen/template_registry.h>
#include <nlohmann/json.hpp>

namespace rex::codegen {

/// Render with pre-parsed json. Avoids double-serialization for internal callers.
/// Implementation lives in template_registry.cpp.
std::string renderWithJson(TemplateRegistry& registry, const std::string& id,
                           const nlohmann::json& data);

}  // namespace rex::codegen
