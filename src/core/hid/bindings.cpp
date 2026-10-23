#include <rex/hid/bindings.h>

#include <algorithm>
#include <array>
#include <format>

#include <rex/cvar.h>
#include <rex/string.h>

namespace rex::hid {

namespace {

bool IsStickRow(const BindingDesc& desc) {
  return desc.axes != PadAxes::kNone;
}

}  // namespace

struct BindingSet::Impl {
  std::span<const BindingDesc> descs;
  std::string scope;
  std::vector<std::vector<InputSource>> sources;
  std::vector<std::string> names;
  std::vector<std::unique_ptr<cvar::Var<std::string>>> vars;
  std::vector<cvar::CallbackHandle> callbacks;
  std::vector<std::string> parse_errors;
  cvar::ICvarRegistry* registry = nullptr;
  u32 generation = 1;

  void Apply(size_t binding, std::string_view value) {
    const BindingDesc& desc = descs[binding];
    std::vector<InputSource>& out = sources[binding];
    out.clear();
    u8 key_half = 0;
    size_t position = 0;
    while (position <= value.size()) {
      const size_t comma = value.find(',', position);
      const bool last = comma == std::string_view::npos;
      const std::string_view token =
          Trim(value.substr(position, last ? std::string_view::npos : comma - position));
      position = last ? value.size() + 1 : comma + 1;
      if (token.empty()) {
        if (IsStickRow(desc) && key_half < kMaxAxisKeyHalves) {
          ++key_half;
        }
        continue;
      }
      auto parsed = ParseInputSource(token);
      if (!parsed) {
        parse_errors.push_back(std::format("{}: {}", desc.name, parsed.error().message));
        continue;
      }
      InputSource source = *parsed;
      if (IsStickRow(desc) && source.kind != InputSourceKind::kPadAxes) {
        if (key_half >= kMaxAxisKeyHalves) {
          parse_errors.push_back(
              std::format("{}: '{}' is ignored, the stick row already has {} key halves", desc.name,
                          token, kMaxAxisKeyHalves));
          continue;
        }
        source.axis_slot = ++key_half;
      }
      const bool repeat = !IsStickRow(desc) && std::ranges::any_of(out, [&](const auto& have) {
        return IsSameInput(have, source);
      });
      if (!repeat) {
        out.push_back(source);
      }
    }
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
    if (!registry || binding >= descs.size()) {
      return false;
    }
    return registry->Set(names[binding], text).has_value();
  }
};

BindingSet::BindingSet(std::span<const BindingDesc> descs, std::string_view scope)
    : impl_(std::make_unique<Impl>()) {
  impl_->descs = descs;
  impl_->scope = std::string(scope);
  impl_->sources.resize(descs.size());
  impl_->names.reserve(descs.size());
  impl_->vars.reserve(descs.size());
  for (size_t i = 0; i < descs.size(); ++i) {
    impl_->names.push_back(std::format("{}.{}", impl_->scope, descs[i].name));
    impl_->Apply(i, descs[i].defaults);
  }
  for (size_t i = 0; i < descs.size(); ++i) {
    impl_->vars.push_back(std::make_unique<cvar::Var<std::string>>(
        impl_->names[i], descs[i].defaults, descs[i].description));
  }
}

BindingSet::~BindingSet() {
  Unregister();
}

Result<void> BindingSet::Register(cvar::ICvarRegistry& registry) {
  if (impl_->registry) {
    return Err<void>(ErrorCategory::kConfig, "the bindings are already registered");
  }
  std::vector<const cvar::VarBase*> table;
  table.reserve(impl_->vars.size());
  for (const auto& var : impl_->vars) {
    table.push_back(var.get());
  }
  if (auto added = registry.Add(table); !added) {
    return added;
  }
  impl_->registry = &registry;
  for (size_t i = 0; i < impl_->vars.size(); ++i) {
    auto handle = registry.OnChange(
        impl_->names[i], [impl = impl_.get(), i](std::string_view, std::string_view value) {
          impl->Apply(i, value);
        });
    if (!handle) {
      Unregister();
      return Err<void>(std::move(handle.error()));
    }
    impl_->callbacks.push_back(std::move(*handle));
  }
  for (size_t i = 0; i < impl_->vars.size(); ++i) {
    impl_->Apply(i, impl_->vars[i]->Get());
  }
  return Ok();
}

void BindingSet::Unregister() {
  impl_->callbacks.clear();
  if (!impl_->registry) {
    return;
  }
  std::vector<const cvar::VarBase*> table;
  table.reserve(impl_->vars.size());
  for (const auto& var : impl_->vars) {
    table.push_back(var.get());
  }
  impl_->registry->Unregister(table);
  impl_->registry = nullptr;
}

size_t BindingSet::GetCount() const {
  return impl_->descs.size();
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
  if (!impl_->registry || binding >= impl_->descs.size()) {
    return false;
  }
  return impl_->registry->ResetToDefault(impl_->names[binding]).has_value();
}

bool BindingSet::ResetGroup(u32 group) {
  bool any = false;
  bool all = true;
  for (size_t i = 0; i < impl_->descs.size(); ++i) {
    if (impl_->descs[i].group != group) {
      continue;
    }
    any = true;
    all = Reset(i) && all;
  }
  return any && all;
}

std::optional<size_t> BindingSet::FindConflict(size_t binding, const InputSource& source) const {
  if (binding >= impl_->descs.size()) {
    return std::nullopt;
  }
  const u32 group = impl_->descs[binding].group;
  for (size_t i = 0; i < impl_->descs.size(); ++i) {
    if (i == binding || impl_->descs[i].group != group) {
      continue;
    }
    if (std::ranges::any_of(impl_->sources[i],
                            [&](const auto& have) { return IsSameInput(have, source); })) {
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
