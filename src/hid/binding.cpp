/**
 * @file        hid/binding.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "hid/binding_set.h"

#include <algorithm>
#include <array>
#include <format>
#include <iterator>

#include <rex/string/text.h>

namespace rex::hid {

namespace {

bool IsStickRow(const BindingConfig& config) {
  return config.axes != PadAxes::kNone;
}

}  // namespace

class BindingOwner final : public SettingOwner {
 public:
  explicit BindingOwner(std::string_view scope) : SettingOwner(scope) {}
};

struct BindingSet::Impl {
  explicit Impl(std::string_view scope_name) : owner(scope_name) {}

  std::span<const BindingConfig> configs;
  std::string scope;
  BindingOwner owner;
  std::vector<std::vector<InputSource>> sources;
  std::vector<std::unique_ptr<Setting<std::string>>> vars;
  std::vector<ScopeGuard> watches;
  std::vector<std::string> parse_errors;
  ISettings* registry = nullptr;
  u32 generation = 1;

  void Apply(size_t binding, std::string_view value) {
    const BindingConfig& config = configs[binding];
    std::vector<InputSource> out;
    std::vector<std::string> errors;
    u8 key_half = 0;
    size_t position = 0;
    while (position <= value.size()) {
      const size_t comma = value.find(',', position);
      const bool last = comma == std::string_view::npos;
      const std::string_view token =
          string::Trim(value.substr(position, last ? std::string_view::npos : comma - position));
      position = last ? value.size() + 1 : comma + 1;
      if (token.empty()) {
        if (IsStickRow(config) && key_half < kMaxAxisKeyHalves) {
          ++key_half;
        }
        continue;
      }
      auto parsed = ParseInputSource(token);
      if (!parsed) {
        errors.push_back(std::format("{}: {}", config.name, parsed.error().message));
        continue;
      }
      InputSource source = *parsed;
      if (IsStickRow(config) && source.type != InputSourceType::kPadAxes) {
        if (key_half >= kMaxAxisKeyHalves) {
          errors.push_back(
              std::format("{}: '{}' is ignored, the stick row already has {} key halves",
                          config.name, token, kMaxAxisKeyHalves));
          continue;
        }
        source.axis_slot = ++key_half;
      }
      const bool repeat = !IsStickRow(config) && std::ranges::any_of(out, [&](const auto& have) {
        return IsSameInput(have, source);
      });
      if (!repeat) {
        out.push_back(source);
      }
    }
    if (!errors.empty()) {
      std::ranges::move(errors, std::back_inserter(parse_errors));
      return;
    }
    sources[binding] = std::move(out);
    ++generation;
  }

  std::string Format(std::span<const InputSource> list) const {
    std::array<std::string, kMaxAxisKeyHalves> halves;
    u8 last_half = 0;
    for (const InputSource& source : list) {
      if (source.axis_slot == 0 || source.axis_slot > kMaxAxisKeyHalves) {
        continue;
      }
      std::string text = InputSourceToString(source);
      if (text.empty()) {
        continue;
      }
      halves[source.axis_slot - 1] = std::move(text);
      last_half = std::max(last_half, source.axis_slot);
    }
    std::string out;
    bool first = true;
    const auto add = [&](const std::string& text) {
      if (!first) {
        out += ",";
      }
      out += text;
      first = false;
    };
    for (u8 i = 0; i < last_half; ++i) {
      add(halves[i]);
    }
    for (const InputSource& source : list) {
      if (source.axis_slot != 0) {
        continue;
      }
      std::string text = InputSourceToString(source);
      if (!text.empty()) {
        add(text);
      }
    }
    return out;
  }

  bool SetText(size_t binding, std::string_view text) {
    if (!registry || binding >= configs.size()) {
      return false;
    }
    return vars[binding]->Set(std::string(text)).has_value();
  }
};

BindingSet::BindingSet(std::span<const BindingConfig> configs, std::string_view scope)
    : impl_(std::make_unique<Impl>(scope)) {
  impl_->configs = configs;
  impl_->scope = std::string(scope);
  impl_->sources.resize(configs.size());
  impl_->vars.reserve(configs.size());
  for (size_t i = 0; i < configs.size(); ++i) {
    impl_->Apply(i, configs[i].defaults);
    impl_->vars.push_back(std::make_unique<Setting<std::string>>(
        &impl_->owner, configs[i].name, std::string(configs[i].defaults), configs[i].description,
        kKeyChord));
  }
}

BindingSet::~BindingSet() {
  Unregister();
}

Result<void> BindingSet::Register(ISettings& settings) {
  if (impl_->registry) {
    return Err<void>(ErrorCategory::kConfig, "the bindings are already registered");
  }
  if (auto added = settings.Register(impl_->owner); !added) {
    return added;
  }
  impl_->registry = &settings;
  for (size_t i = 0; i < impl_->vars.size(); ++i) {
    impl_->watches.push_back(impl_->vars[i]->OnChange(
        [impl = impl_.get(), i](const std::string& value) { impl->Apply(i, value); }));
    impl_->Apply(i, impl_->vars[i]->Get());
  }
  return Ok();
}

void BindingSet::Unregister() {
  impl_->watches.clear();
  if (!impl_->registry) {
    return;
  }
  impl_->registry->Unregister(impl_->owner);
  impl_->registry = nullptr;
}

size_t BindingSet::GetCount() const {
  return impl_->configs.size();
}

std::span<const InputSource> BindingSet::GetSources(size_t binding) const {
  if (binding >= impl_->sources.size()) {
    return {};
  }
  return impl_->sources[binding];
}

bool BindingSet::SetSource(size_t binding, size_t slot, const InputSource& source) {
  if (binding >= impl_->sources.size() || InputSourceToString(source).empty()) {
    return false;
  }
  std::vector<InputSource> updated = impl_->sources[binding];
  if (slot < updated.size()) {
    updated[slot] = source;
  } else {
    updated.push_back(source);
  }
  return SetSources(binding, updated);
}

bool BindingSet::SetSources(size_t binding, std::span<const InputSource> sources) {
  if (binding >= impl_->sources.size()) {
    return false;
  }
  return impl_->SetText(binding, impl_->Format(sources));
}

bool BindingSet::Clear(size_t binding) {
  return impl_->SetText(binding, "");
}

bool BindingSet::Reset(size_t binding) {
  if (!impl_->registry || binding >= impl_->configs.size()) {
    return false;
  }
  return impl_->vars[binding]->Reset().has_value();
}

bool BindingSet::ResetGroup(u32 group) {
  bool any = false;
  bool all = true;
  for (size_t i = 0; i < impl_->configs.size(); ++i) {
    if (impl_->configs[i].group != group) {
      continue;
    }
    any = true;
    all = Reset(i) && all;
  }
  return any && all;
}

std::optional<size_t> BindingSet::FindConflict(size_t binding, const InputSource& source) const {
  if (binding >= impl_->configs.size()) {
    return std::nullopt;
  }
  const u32 group = impl_->configs[binding].group;
  for (size_t i = 0; i < impl_->configs.size(); ++i) {
    if (i == binding || impl_->configs[i].group != group) {
      continue;
    }
    if (std::ranges::any_of(impl_->sources[i],
                            [&](const auto& have) { return IsSameInput(have, source); })) {
      return i;
    }
  }
  return std::nullopt;
}

std::optional<size_t> BindingSet::FindBySetting(std::string_view setting_name) const {
  const std::string_view scope = impl_->scope;
  if (setting_name.size() <= scope.size() || !setting_name.starts_with(scope) ||
      setting_name[scope.size()] != '.') {
    return std::nullopt;
  }
  const std::string_view name = setting_name.substr(scope.size() + 1);
  for (size_t i = 0; i < impl_->configs.size(); ++i) {
    if (impl_->configs[i].name == name) {
      return i;
    }
  }
  return std::nullopt;
}

u32 BindingSet::GetGeneration() const {
  return impl_->generation;
}

std::vector<std::string> BindingSet::TakeParseErrors() {
  return std::exchange(impl_->parse_errors, {});
}

}  // namespace rex::hid
