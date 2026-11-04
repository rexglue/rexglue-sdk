/**
 * @file        hid/binding_set.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rex/hid/binding.h>
#include <rex/platform.h>

namespace rex::hid {

class BindingSet final : public IBindingSet {
 public:
  /**
   * Parses the defaults of every binding. The descriptions must outlive the set.
   *
   * @param configs Bindings, referred to by index elsewhere.
   * @param scope The setting scope, one name without a dot.
   */
  explicit BindingSet(std::span<const BindingConfig> configs, std::string_view scope = "bind");
  ~BindingSet() override;
  BindingSet(const BindingSet&) = delete;
  BindingSet& operator=(const BindingSet&) = delete;

  [[nodiscard]] Result<void> Register(ISettingsRegistry& settings) override;
  void Unregister() override;
  [[nodiscard]] size_t GetCount() const override;
  [[nodiscard]] std::span<const InputSource> GetSources(size_t binding) const override;
  bool SetSource(size_t binding, size_t slot, const InputSource& source) override;
  bool SetSources(size_t binding, std::span<const InputSource> sources) override;
  bool Clear(size_t binding) override;
  bool Reset(size_t binding) override;
  bool ResetGroup(u32 group) override;
  [[nodiscard]] std::optional<size_t> FindConflict(size_t binding,
                                                   const InputSource& source) const override;
  [[nodiscard]] std::optional<size_t> FindBySetting(std::string_view setting_name) const override;
  [[nodiscard]] u32 GetGeneration() const override;
  [[nodiscard]] std::vector<std::string> TakeParseErrors() override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rex::hid
