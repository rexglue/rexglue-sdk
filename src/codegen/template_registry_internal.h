/**
 * @file        codegen/template_registry_internal.h
 * @brief       Internal json-native render for TemplateRegistry
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/codegen/template_registry.h>
#include <nlohmann/json.hpp>

namespace rex::codegen {

/**
 * Renders a template from parsed json, avoiding a second serialization.
 *
 * @param registry Registry holding the template.
 * @param id Template ID.
 * @param data Template data.
 * @return The rendered text.
 */
std::string RenderWithJSON(TemplateRegistry& registry, const std::string& id,
                           const nlohmann::json& data);

}  // namespace rex::codegen
