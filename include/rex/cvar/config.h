/**
 * @file        cvar/config.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rex/cvar/registry.h>

namespace rex {

/** 1 value arriving before Init. */
struct CvarOverride {
  /** Dotted cvar name such as gpu.vsync. */
  std::string name;
  /** The value as text. */
  std::string value;
  /** Source the registry stores for the value. */
  cvar::CvarSource source = cvar::CvarSource::kCommandLine;
};

/** A consumer table, registered before any module. */
struct CvarTable {
  /** Scope every cvar in the table belongs to. */
  std::string scope;
  /** The cvars in the table. */
  std::span<const cvar::IVar* const> vars;
};

/** Cvar tables, values and locks that Init applies in order. */
struct CvarConfig {
  /** Consumer tables, registered in order. */
  std::vector<CvarTable> tables;
  /** Values arriving before Init, applied in order. */
  std::vector<CvarOverride> values;
  /** Dotted names of the cvars Init locks. */
  std::vector<std::string> locked;
  /**
   * Appends a Default override, applied only while the cvar's source is Default.
   *
   * @param name Dotted cvar name.
   * @param value The value as text.
   */
  void SetDefault(std::string_view name, std::string_view value) {
    values.push_back({std::string(name), std::string(value), cvar::CvarSource::kDefault});
  }
  /**
   * Appends a CommandLine override.
   *
   * @param name Dotted cvar name.
   * @param value The value as text.
   */
  void Set(std::string_view name, std::string_view value) {
    values.push_back({std::string(name), std::string(value), cvar::CvarSource::kCommandLine});
  }
  /**
   * Appends to locked.
   *
   * @param name Dotted cvar name.
   */
  void Lock(std::string_view name) { locked.emplace_back(name); }

  /**
   * Looks up the value Init would apply to a cvar, before Init runs.
   *
   * @param name Dotted cvar name, such as "gpu.vsync".
   * @return The queued value with the highest source precedence, or std::nullopt when no
   *         value is queued for name.
   */
  std::optional<std::string> Get(std::string_view name) const {
    const CvarOverride* best = nullptr;
    for (const CvarOverride& value : values) {
      if (value.name == name && (!best || value.source >= best->source)) {
        best = &value;
      }
    }
    if (!best) {
      return std::nullopt;
    }
    return best->value;
  }
};

}  // namespace rex
