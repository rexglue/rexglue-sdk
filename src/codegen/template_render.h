/**
 * @file        codegen/template_render.h
 * @brief       Renders from parsed JSON for TemplateRegistry
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include "template_registry.h"
#include <nlohmann/json.hpp>

namespace rex::codegen {

/** Renders a template from parsed JSON, avoiding a second serialization. */
std::string RenderWithJSON(TemplateRegistry& registry, const std::string& id,
                           const nlohmann::json& data);

}  // namespace rex::codegen
