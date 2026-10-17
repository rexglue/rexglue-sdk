#include "core/cvar_registry.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>
#include <limits>
#include <map>
#include <ranges>

#include <rex/string/utf8.h>
#include <rex/api.h>
#include <rex/cvar.h>
#include <rex/logging/log.h>
#include <rex/platform/env.h>
#include <rex/string/numeric.h>

namespace rex::detail {

namespace {

using cvar::BuildError;
using cvar::CvarDesc;
using cvar::CvarSource;
using cvar::VarBase;
using E = cvar::CvarError;

bool ValidName(std::string_view name, std::string_view scope) {
  return cvar::IsName(name) && name.substr(0, name.find('.')) == scope;
}

const char* SourceName(CvarSource s) {
  switch (s) {
    case CvarSource::kDefault:
      return "Default";
    case CvarSource::kConfig:
      return "Config";
    case CvarSource::kEnvironment:
      return "Environment";
    case CvarSource::kCommandLine:
      return "CommandLine";
    case CvarSource::kRuntime:
      return "Runtime";
  }
  return "Unknown";
}

std::string EscapeTOML(std::string_view s) {
  std::string out = "\"";
  for (char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out += c;
    }
  }
  return out + "\"";
}

}  // namespace

CvarRegistryImpl::~CvarRegistryImpl() {
  Reset();
}

std::unique_ptr<CvarRegistryImpl> CreateCvarRegistry() {
  return std::make_unique<CvarRegistryImpl>();
}

CvarRegistryImpl::Entry* CvarRegistryImpl::FindEntry(std::string_view name) {
  auto it = index_.find(name);
  return it == index_.end() ? nullptr : &entries_[it->second];
}

const CvarRegistryImpl::Entry* CvarRegistryImpl::FindEntry(std::string_view name) const {
  auto it = index_.find(name);
  return it == index_.end() ? nullptr : &entries_[it->second];
}

std::string_view CvarRegistryImpl::DefaultOf(const Entry& entry) const {
  return entry.has_default_override ? std::string_view(entry.default_override)
                                    : entry.var->GetDefaultText();
}

Result<void> CvarRegistryImpl::Register(std::string_view scope,
                                        std::span<const VarBase* const> vars) {
  std::lock_guard lock(mutex_);
  if (!cvar::IsNamePart(scope)) {
    return Err(
        BuildError(E::kBadName, std::format("scope '{}' is not lowercase [a-z0-9_]", scope)));
  }
  if (std::ranges::find(scopes_, scope) != scopes_.end()) {
    return Err(
        BuildError(E::kDuplicateScope, std::format("scope '{}' is already registered", scope)));
  }
  for (size_t i = 0; i < vars.size(); ++i) {
    const auto& d = vars[i]->GetDesc();
    if (!ValidName(d.name, scope)) {
      return Err(BuildError(
          E::kBadName,
          std::format("cvar '{}' is not '{}.leaf' in lowercase [a-z0-9_]", d.name, scope)));
    }
    if (index_.contains(d.name)) {
      return Err(
          BuildError(E::kDuplicateName, std::format("cvar '{}' is already registered", d.name)));
    }
    for (size_t j = 0; j < i; ++j) {
      if (vars[j]->GetDesc().name == d.name) {
        return Err(BuildError(E::kDuplicateName,
                              std::format("cvar '{}' appears twice in the table", d.name)));
      }
    }
  }
  scopes_.emplace_back(scope);
  if (vars.empty())
    pinned_scopes_.emplace_back(scope);
  for (const VarBase* var : vars) {
    index_[var->GetDesc().name] = entries_.size();
    Entry entry;
    entry.var = var;
    entries_.push_back(std::move(entry));
    cvar::DetailAccess::SetRegistry(*var, this);
  }
  return Ok();
}

Result<void> CvarRegistryImpl::Add(std::span<const VarBase* const> vars) {
  Fired fired;
  {
    std::lock_guard lock(mutex_);
    for (size_t i = 0; i < vars.size(); ++i) {
      const auto& d = vars[i]->GetDesc();
      if (std::ranges::find(scopes_, d.scope) == scopes_.end()) {
        return Err(BuildError(E::kBadName,
                              std::format("cvar '{}' names a scope that is not reserved", d.name)));
      }
      if (!ValidName(d.name, d.scope)) {
        return Err(
            BuildError(E::kBadName,
                       std::format("cvar '{}' is not scope.leaf in lowercase [a-z0-9_]", d.name)));
      }
      if (index_.contains(d.name)) {
        return Err(
            BuildError(E::kDuplicateName, std::format("cvar '{}' is already registered", d.name)));
      }
      for (size_t j = 0; j < i; ++j) {
        if (vars[j]->GetDesc().name == d.name) {
          return Err(BuildError(E::kDuplicateName,
                                std::format("cvar '{}' appears twice in the table", d.name)));
        }
      }
    }
    for (const VarBase* var : vars) {
      index_[var->GetDesc().name] = entries_.size();
      Entry entry;
      entry.var = var;
      entries_.push_back(std::move(entry));
      cvar::DetailAccess::SetRegistry(*var, this);
      ApplyPending(entries_.back(), fired);
    }
  }
  Fire(fired);
  return Ok();
}

void CvarRegistryImpl::ApplyPending(Entry& entry, Fired& fired) {
  const std::string_view name = entry.var->GetDesc().name;
  for (CvarSource source : {CvarSource::kDefault, CvarSource::kConfig, CvarSource::kEnvironment,
                            CvarSource::kCommandLine}) {
    for (const CvarOverride& o : pending_) {
      if (o.source != source || o.name != name)
        continue;
      Result<void> status = Ok();
      if (source == CvarSource::kDefault) {
        status = Validate(entry, o.value, source);
        if (status) {
          entry.default_override = o.value;
          entry.has_default_override = true;
          if (entry.source != CvarSource::kDefault)
            continue;
        }
      }
      if (status)
        status = Apply(entry, o.value, source, fired);
      if (!status)
        REXLOG_WARN(core, "{}", status.error().message);
    }
  }
  std::erase_if(pending_, [&](const CvarOverride& o) { return o.name == name; });
}

void CvarRegistryImpl::Unregister(std::span<const VarBase* const> vars) {
  std::lock_guard lock(mutex_);
  for (const VarBase* var : vars) {
    auto it = index_.find(var->GetDesc().name);
    if (it == index_.end())
      continue;
    cvar::DetailAccess::SetRegistry(*var, nullptr);
    size_t removed = it->second;
    entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(removed));
    index_.erase(it);
    for (auto& [name, idx] : index_) {
      if (idx > removed)
        --idx;
    }
    std::erase_if(callbacks_, [&](const std::shared_ptr<Callback>& cb) {
      return cb->name == var->GetDesc().name;
    });
    std::erase(pending_restart_, std::string(var->GetDesc().name));
  }
  std::erase_if(scopes_, [&](const std::string& scope) {
    return std::ranges::find(pinned_scopes_, scope) == pinned_scopes_.end() &&
           std::ranges::none_of(entries_,
                                [&](const Entry& e) { return e.var->GetDesc().scope == scope; });
  });
}

Result<void> CvarRegistryImpl::Validate(const Entry& entry, std::string_view value,
                                        CvarSource source) const {
  const CvarDesc& d = entry.var->GetDesc();
  const auto& c = d.constraints;
  auto rejected = [&](std::string_view reason) {
    return Err(BuildError(E::kInvalid, std::format("cvar '{}' rejected value '{}' from {}: {}",
                                                   d.name, value, SourceName(source), reason)));
  };
  if (c.min || c.max) {
    f64 v = 0;
    auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), v);
    if (ec != std::errc{} || ptr != value.data() + value.size()) {
      return rejected("not a number");
    }
    if ((c.min && v < *c.min) || (c.max && v > *c.max)) {
      return rejected(
          std::format("outside {} to {}", c.min.value_or(-1e300), c.max.value_or(1e300)));
    }
  }
  if (!c.allowed.empty() && std::ranges::find(c.allowed, value) == c.allowed.end()) {
    return rejected("not in the allowed list");
  }
  if (c.validator && !c.validator(value)) {
    return rejected("validator refused it");
  }
  return Ok();
}

Result<void> CvarRegistryImpl::Apply(Entry& entry, std::string_view value, CvarSource source,
                                     Fired& fired) {
  const CvarDesc& d = entry.var->GetDesc();
  if (d.type == cvar::Type::kCommand) {
    return Err(BuildError(E::kNotAVar, std::format("'{}' is a command", d.name)));
  }
  if (entry.locked && source == CvarSource::kRuntime) {
    return Err(BuildError(E::kLocked, std::format("cvar '{}' is locked", d.name)));
  }
  if (finalized_ && d.lifecycle == cvar::Lifecycle::kInitOnly && source == CvarSource::kRuntime) {
    return Err(BuildError(E::kInitOnly,
                          std::format("cvar '{}' can only be set before Init completes", d.name)));
  }
  REX_TRY_VOID(Validate(entry, value, source));
  std::string before = entry.var->Render();
  auto* var = const_cast<VarBase*>(entry.var);
  if (auto status = var->Parse(value); !status) {
    return Err(
        BuildError(E::kInvalid, std::format("cvar '{}' rejected value '{}' from {}: {}", d.name,
                                            value, SourceName(source), status.error().message)));
  }
  entry.source = source;
  if (finalized_ && source == CvarSource::kRuntime &&
      d.lifecycle == cvar::Lifecycle::kRequiresRestart &&
      std::ranges::find(pending_restart_, d.name) == pending_restart_.end()) {
    pending_restart_.emplace_back(d.name);
  }
  std::string after = entry.var->Render();
  if (before != after) {
    fired.emplace_back(std::string(d.name), std::move(after));
  }
  return Ok();
}

void CvarRegistryImpl::Fire(Fired& fired) {
  for (auto& [name, value] : fired) {
    std::vector<std::shared_ptr<Callback>> targets;
    {
      std::lock_guard lock(mutex_);
      for (auto& cb : callbacks_) {
        if (cb->name == name)
          targets.push_back(cb);
      }
    }
    for (auto& cb : targets) {
      bool alive = false;
      {
        std::lock_guard lock(mutex_);
        alive = std::ranges::find(callbacks_, cb) != callbacks_.end();
      }
      if (alive)
        cb->fn(name, value);
    }
  }
  fired.clear();
}

Result<void> CvarRegistryImpl::Set(std::string_view name, std::string_view value) {
  Fired fired;
  {
    std::lock_guard lock(mutex_);
    Entry* entry = FindEntry(name);
    if (!entry)
      return Err(BuildError(E::kNotFound, std::format("no cvar named '{}'", name)));
    REX_TRY_VOID(Apply(*entry, value, CvarSource::kRuntime, fired));
  }
  Fire(fired);
  return Ok();
}

Result<std::string> CvarRegistryImpl::Get(std::string_view name) const {
  std::lock_guard lock(mutex_);
  const Entry* entry = FindEntry(name);
  if (!entry) {
    return Err<std::string>(BuildError(E::kNotFound, std::format("no cvar named '{}'", name)));
  }
  return entry->var->Render();
}

Result<void> CvarRegistryImpl::SetDefault(std::string_view name, std::string_view value) {
  Fired fired;
  {
    std::lock_guard lock(mutex_);
    Entry* entry = FindEntry(name);
    if (!entry)
      return Err(BuildError(E::kNotFound, std::format("no cvar named '{}'", name)));
    REX_TRY_VOID(Validate(*entry, value, CvarSource::kDefault));
    entry->default_override = std::string(value);
    entry->has_default_override = true;
    if (entry->source == CvarSource::kDefault) {
      REX_TRY_VOID(Apply(*entry, value, CvarSource::kDefault, fired));
    }
  }
  Fire(fired);
  return Ok();
}

Result<void> CvarRegistryImpl::Lock(std::string_view name) {
  std::lock_guard lock(mutex_);
  Entry* entry = FindEntry(name);
  if (!entry)
    return Err(BuildError(E::kNotFound, std::format("no cvar named '{}'", name)));
  entry->locked = true;
  return Ok();
}

Result<void> CvarRegistryImpl::ResetToDefault(std::string_view name) {
  Fired fired;
  {
    std::lock_guard lock(mutex_);
    Entry* entry = FindEntry(name);
    if (!entry)
      return Err(BuildError(E::kNotFound, std::format("no cvar named '{}'", name)));
    std::string value(DefaultOf(*entry));
    REX_TRY_VOID(Apply(*entry, value, CvarSource::kDefault, fired));
  }
  Fire(fired);
  return Ok();
}

std::optional<std::reference_wrapper<const VarBase>> CvarRegistryImpl::Find(
    std::string_view name) const {
  std::lock_guard lock(mutex_);
  const Entry* entry = FindEntry(name);
  if (!entry) {
    return std::nullopt;
  }
  return std::cref(*entry->var);
}

CvarSource CvarRegistryImpl::Source(std::string_view name) const {
  std::lock_guard lock(mutex_);
  const Entry* entry = FindEntry(name);
  return entry ? entry->source : CvarSource::kDefault;
}

std::vector<const VarBase*> CvarRegistryImpl::Enumerate(std::string_view scope) const {
  std::lock_guard lock(mutex_);
  std::vector<const VarBase*> out;
  for (const Entry& e : entries_) {
    if (scope.empty() || e.var->GetDesc().scope == scope)
      out.push_back(e.var);
  }
  return out;
}

std::vector<std::string_view> CvarRegistryImpl::GetScopes() const {
  std::lock_guard lock(mutex_);
  return {scopes_.begin(), scopes_.end()};
}

Result<cvar::CallbackHandle> CvarRegistryImpl::OnChange(std::string_view name,
                                                        cvar::ChangeCallback callback) {
  std::lock_guard lock(mutex_);
  if (!FindEntry(name)) {
    return Err<cvar::CallbackHandle>(
        BuildError(E::kNotFound, std::format("no cvar named '{}'", name)));
  }
  u64 id = next_callback_id_++;
  callbacks_.push_back(
      std::make_shared<Callback>(Callback{id, std::string(name), std::move(callback)}));
  return cvar::CallbackHandle(this, id);
}

void CvarRegistryImpl::RemoveCallback(u64 id) {
  std::lock_guard lock(mutex_);
  std::erase_if(callbacks_, [id](const std::shared_ptr<Callback>& cb) { return cb->id == id; });
}

Result<void> CvarRegistryImpl::Invoke(std::string_view name, std::string_view args) {
  const VarBase* var = nullptr;
  {
    std::lock_guard lock(mutex_);
    const Entry* entry = FindEntry(name);
    if (!entry)
      return Err(BuildError(E::kNotFound, std::format("no command named '{}'", name)));
    if (entry->var->GetDesc().type != cvar::Type::kCommand) {
      return Err(BuildError(E::kNotACommand, std::format("'{}' is not a command", name)));
    }
    var = entry->var;
  }
  return const_cast<VarBase*>(var)->Invoke(args);
}

std::vector<std::string> CvarRegistryImpl::GetPendingRestart() const {
  std::lock_guard lock(mutex_);
  return pending_restart_;
}

std::string CvarRegistryImpl::Serialize() const {
  std::lock_guard lock(mutex_);
  std::map<std::string_view, std::vector<std::string>> tables;
  for (const Entry& e : entries_) {
    const CvarDesc& d = e.var->GetDesc();
    if (d.type == cvar::Type::kCommand)
      continue;
    if (d.no_save)
      continue;
    std::string value = e.var->Render();
    if (value == DefaultOf(e))
      continue;
    std::string_view leaf = d.name.substr(d.name.find('.') + 1);
    std::string rendered = d.type == cvar::Type::kString ? EscapeTOML(value) : value;
    tables[d.scope].push_back(std::format("{} = {}\n", leaf, rendered));
  }
  std::string out;
  for (auto& [scope, lines] : tables) {
    out += std::format("[{}]\n", scope);
    for (auto& line : lines)
      out += line;
    out += "\n";
  }
  return out;
}

bool CvarRegistryImpl::IsFinalized() const {
  std::lock_guard lock(mutex_);
  return finalized_;
}

Result<std::vector<CvarOverride>> CvarRegistryImpl::ApplyOverrides(
    std::span<const CvarOverride> overrides) {
  std::vector<CvarOverride> unknown;
  Fired fired;
  {
    std::lock_guard lock(mutex_);
    for (CvarSource source : {CvarSource::kDefault, CvarSource::kConfig, CvarSource::kEnvironment,
                              CvarSource::kCommandLine}) {
      for (const CvarOverride& o : overrides) {
        if (o.source != source)
          continue;
        Entry* entry = FindEntry(o.name);
        if (!entry) {
          const std::string_view scope = std::string_view(o.name).substr(0, o.name.find('.'));
          if (std::ranges::find(pinned_scopes_, scope) != pinned_scopes_.end()) {
            pending_.push_back(o);
          } else {
            unknown.push_back(o);
          }
          continue;
        }
        if (source == CvarSource::kDefault) {
          REX_TRY_VOID(Validate(*entry, o.value, source));
          entry->default_override = o.value;
          entry->has_default_override = true;
          if (entry->source != CvarSource::kDefault)
            continue;
        }
        REX_TRY_VOID(Apply(*entry, o.value, source, fired));
      }
    }
  }
  Fire(fired);
  return unknown;
}

Result<void> CvarRegistryImpl::ApplyLocks(std::span<const std::string> names) {
  std::lock_guard lock(mutex_);
  for (const std::string& name : names) {
    Entry* entry = FindEntry(name);
    if (!entry) {
      return Err(BuildError(E::kNotFound, std::format("cannot lock unknown cvar '{}'", name)));
    }
    entry->locked = true;
  }
  return Ok();
}

void CvarRegistryImpl::Finalize() {
  std::lock_guard lock(mutex_);
  finalized_ = true;
}

void CvarRegistryImpl::Reset() {
  std::lock_guard lock(mutex_);
  for (Entry& e : entries_)
    cvar::DetailAccess::SetRegistry(*e.var, nullptr);
  entries_.clear();
  index_.clear();
  scopes_.clear();
  pinned_scopes_.clear();
  pending_.clear();
  callbacks_.clear();
  pending_restart_.clear();
  finalized_ = false;
}

std::vector<std::string> CvarRegistryImpl::GetRegisteredNames() const {
  std::lock_guard lock(mutex_);
  std::vector<std::string> out;
  out.reserve(entries_.size());
  for (const Entry& e : entries_)
    out.emplace_back(e.var->GetDesc().name);
  return out;
}

}  // namespace rex::detail

namespace rex::cvar {

bool IsNamePart(std::string_view part) {
  if (part.empty() || !std::islower(static_cast<unsigned char>(part.front())))
    return false;
  return std::ranges::all_of(part, [](char c) {
    return std::islower(static_cast<unsigned char>(c)) ||
           std::isdigit(static_cast<unsigned char>(c)) || c == '_';
  });
}

bool IsName(std::string_view name) {
  const auto dot = name.find('.');
  if (dot == std::string_view::npos || name.find('.', dot + 1) != std::string_view::npos) {
    return false;
  }
  return IsNamePart(name.substr(0, dot)) && IsNamePart(name.substr(dot + 1));
}

namespace {

template <typename T, typename Wide>
std::string ClampIntegerText(const rex::cvar::CvarDesc& desc, std::string_view text) {
  Wide value = std::clamp<Wide>(rex::from_string<Wide>(text), std::numeric_limits<T>::min(),
                                std::numeric_limits<T>::max());
  if (desc.constraints.min && static_cast<f64>(value) < *desc.constraints.min) {
    value = static_cast<Wide>(*desc.constraints.min);
  }
  if (desc.constraints.max && static_cast<f64>(value) > *desc.constraints.max) {
    value = static_cast<Wide>(*desc.constraints.max);
  }
  return std::to_string(value);
}

}  // namespace

std::string ClampIntegerCvarText(const rex::cvar::CvarDesc& desc, std::string_view text) {
  switch (desc.type) {
    case rex::cvar::Type::kInt32:
      return ClampIntegerText<i32, i64>(desc, text);
    case rex::cvar::Type::kUInt32:
      return ClampIntegerText<u32, u64>(desc, text);
    case rex::cvar::Type::kUInt64:
      return ClampIntegerText<u64, u64>(desc, text);
    default:
      return ClampIntegerText<i64, i64>(desc, text);
  }
}

void ApplyEnvironment(rex::Config& config) {
  for (auto& [name, value] : rex::platform::EnumerateEnvironment()) {
    if (!name.starts_with("REX_"))
      continue;
    std::string_view rest = std::string_view(name).substr(4);
    auto underscore = rest.find('_');
    if (underscore == std::string_view::npos || underscore == 0 || underscore + 1 >= rest.size()) {
      continue;
    }
    const std::string cvar_name = utf8_lower_ascii(
        std::format("{}.{}", rest.substr(0, underscore), rest.substr(underscore + 1)));
    config.cvars.values.push_back({cvar_name, value, CvarSource::kEnvironment});
  }
}

}  // namespace rex::cvar
