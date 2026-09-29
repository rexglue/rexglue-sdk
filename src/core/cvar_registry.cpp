/**
 * @file        core/cvar_registry.cpp
 * @brief       The cvar registry implementation and the environment reader
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "core/cvar_registry.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>
#include <map>
#include <ranges>

#include <rex/api.h>
#include <rex/logging.h>
#include <rex/platform/env.h>

namespace rex::detail {

namespace {

using cvar::CvarSource;
using cvar::Desc;
using cvar::IVar;
using cvar::MakeError;
using E = cvar::Error;

bool LowerToken(std::string_view part) {
  if (part.empty() || !std::islower(static_cast<unsigned char>(part.front())))
    return false;
  return std::ranges::all_of(part, [](char c) {
    return std::islower(static_cast<unsigned char>(c)) ||
           std::isdigit(static_cast<unsigned char>(c)) || c == '_';
  });
}

bool ValidName(std::string_view name, std::string_view scope) {
  auto dot = name.find('.');
  if (dot == std::string_view::npos || name.find('.', dot + 1) != std::string_view::npos) {
    return false;
  }
  return name.substr(0, dot) == scope && LowerToken(scope) && LowerToken(name.substr(dot + 1));
}

const char* SourceName(CvarSource s) {
  switch (s) {
    case CvarSource::Default:
      return "Default";
    case CvarSource::Config:
      return "Config";
    case CvarSource::Environment:
      return "Environment";
    case CvarSource::CommandLine:
      return "CommandLine";
    case CvarSource::Runtime:
      return "Runtime";
  }
  return "Unknown";
}

std::string EscapeToml(std::string_view s) {
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
                                    : entry.var->DefaultText();
}

Status CvarRegistryImpl::Register(std::string_view scope, std::span<const IVar* const> vars) {
  std::lock_guard lock(mutex_);
  if (!LowerToken(scope)) {
    return Err(MakeError(E::BadName, std::format("scope '{}' is not lowercase [a-z0-9_]", scope)));
  }
  if (std::ranges::find(scopes_, scope) != scopes_.end()) {
    return Err(
        MakeError(E::DuplicateScope, std::format("scope '{}' is already registered", scope)));
  }
  for (size_t i = 0; i < vars.size(); ++i) {
    const auto& d = vars[i]->desc();
    if (!ValidName(d.name, scope)) {
      return Err(MakeError(
          E::BadName,
          std::format("cvar '{}' is not '{}.leaf' in lowercase [a-z0-9_]", d.name, scope)));
    }
    if (index_.contains(d.name)) {
      return Err(
          MakeError(E::DuplicateName, std::format("cvar '{}' is already registered", d.name)));
    }
    for (size_t j = 0; j < i; ++j) {
      if (vars[j]->desc().name == d.name) {
        return Err(MakeError(E::DuplicateName,
                             std::format("cvar '{}' appears twice in the table", d.name)));
      }
    }
  }
  scopes_.emplace_back(scope);
  if (vars.empty())
    pinned_scopes_.emplace_back(scope);
  for (const IVar* var : vars) {
    index_[var->desc().name] = entries_.size();
    Entry entry;
    entry.var = var;
    entries_.push_back(std::move(entry));
    cvar::detail_access::set_registry(*var, this);
  }
  return Ok();
}

Status CvarRegistryImpl::Add(std::span<const IVar* const> vars) {
  Fired fired;
  {
    std::lock_guard lock(mutex_);
    for (size_t i = 0; i < vars.size(); ++i) {
      const auto& d = vars[i]->desc();
      if (std::ranges::find(scopes_, d.scope) == scopes_.end()) {
        return Err(MakeError(E::BadName,
                             std::format("cvar '{}' names a scope that is not reserved", d.name)));
      }
      if (!ValidName(d.name, d.scope)) {
        return Err(MakeError(
            E::BadName, std::format("cvar '{}' is not scope.leaf in lowercase [a-z0-9_]", d.name)));
      }
      if (index_.contains(d.name)) {
        return Err(
            MakeError(E::DuplicateName, std::format("cvar '{}' is already registered", d.name)));
      }
      for (size_t j = 0; j < i; ++j) {
        if (vars[j]->desc().name == d.name) {
          return Err(MakeError(E::DuplicateName,
                               std::format("cvar '{}' appears twice in the table", d.name)));
        }
      }
    }
    for (const IVar* var : vars) {
      index_[var->desc().name] = entries_.size();
      Entry entry;
      entry.var = var;
      entries_.push_back(std::move(entry));
      cvar::detail_access::set_registry(*var, this);
      ApplyPending(entries_.back(), fired);
    }
  }
  Fire(fired);
  return Ok();
}

void CvarRegistryImpl::ApplyPending(Entry& entry, Fired& fired) {
  const std::string_view name = entry.var->desc().name;
  for (CvarSource source : {CvarSource::Default, CvarSource::Config, CvarSource::Environment,
                            CvarSource::CommandLine}) {
    for (const CvarOverride& o : pending_) {
      if (o.source != source || o.name != name)
        continue;
      Status status = Ok();
      if (source == CvarSource::Default) {
        status = Validate(entry, o.value, source);
        if (status) {
          entry.default_override = o.value;
          entry.has_default_override = true;
          if (entry.source != CvarSource::Default)
            continue;
        }
      }
      if (status)
        status = Apply(entry, o.value, source, fired);
      if (!status)
        REXLOG_WARN("{}", status.error().message);
    }
  }
  std::erase_if(pending_, [&](const CvarOverride& o) { return o.name == name; });
}

void CvarRegistryImpl::Unregister(std::span<const IVar* const> vars) {
  std::lock_guard lock(mutex_);
  for (const IVar* var : vars) {
    auto it = index_.find(var->desc().name);
    if (it == index_.end())
      continue;
    cvar::detail_access::set_registry(*var, nullptr);
    size_t removed = it->second;
    entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(removed));
    index_.erase(it);
    for (auto& [name, idx] : index_) {
      if (idx > removed)
        --idx;
    }
    std::erase_if(callbacks_, [&](const std::shared_ptr<Callback>& cb) {
      return cb->name == var->desc().name;
    });
    std::erase(pending_restart_, std::string(var->desc().name));
  }
  std::erase_if(scopes_, [&](const std::string& scope) {
    return std::ranges::find(pinned_scopes_, scope) == pinned_scopes_.end() &&
           std::ranges::none_of(entries_,
                                [&](const Entry& e) { return e.var->desc().scope == scope; });
  });
}

Status CvarRegistryImpl::Validate(const Entry& entry, std::string_view value,
                                  CvarSource source) const {
  const Desc& d = entry.var->desc();
  const auto& c = d.constraints;
  auto rejected = [&](std::string_view reason) {
    return Err(MakeError(E::Invalid, std::format("cvar '{}' rejected value '{}' from {}: {}",
                                                 d.name, value, SourceName(source), reason)));
  };
  if (c.min || c.max) {
    double v = 0;
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

Status CvarRegistryImpl::Apply(Entry& entry, std::string_view value, CvarSource source,
                               Fired& fired) {
  const Desc& d = entry.var->desc();
  if (d.type == cvar::Type::Command) {
    return Err(MakeError(E::NotAVar, std::format("'{}' is a command", d.name)));
  }
  if (entry.locked && source == CvarSource::Runtime) {
    return Err(MakeError(E::Locked, std::format("cvar '{}' is locked", d.name)));
  }
  if (finalized_ && d.lifecycle == cvar::Lifecycle::InitOnly && source == CvarSource::Runtime) {
    return Err(MakeError(E::InitOnly,
                         std::format("cvar '{}' can only be set before Init completes", d.name)));
  }
  REX_TRY_VOID(Validate(entry, value, source));
  std::string before = entry.var->Render();
  auto* var = const_cast<IVar*>(entry.var);
  if (auto status = var->Parse(value); !status) {
    return Err(
        MakeError(E::Invalid, std::format("cvar '{}' rejected value '{}' from {}: {}", d.name,
                                          value, SourceName(source), status.error().message)));
  }
  entry.source = source;
  if (finalized_ && source == CvarSource::Runtime &&
      d.lifecycle == cvar::Lifecycle::RequiresRestart &&
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

Status CvarRegistryImpl::Set(std::string_view name, std::string_view value) {
  Fired fired;
  {
    std::lock_guard lock(mutex_);
    Entry* entry = FindEntry(name);
    if (!entry)
      return Err(MakeError(E::NotFound, std::format("no cvar named '{}'", name)));
    REX_TRY_VOID(Apply(*entry, value, CvarSource::Runtime, fired));
  }
  Fire(fired);
  return Ok();
}

Result<std::string> CvarRegistryImpl::Get(std::string_view name) const {
  std::lock_guard lock(mutex_);
  const Entry* entry = FindEntry(name);
  if (!entry) {
    return Err<std::string>(MakeError(E::NotFound, std::format("no cvar named '{}'", name)));
  }
  return entry->var->Render();
}

Status CvarRegistryImpl::SetDefault(std::string_view name, std::string_view value) {
  Fired fired;
  {
    std::lock_guard lock(mutex_);
    Entry* entry = FindEntry(name);
    if (!entry)
      return Err(MakeError(E::NotFound, std::format("no cvar named '{}'", name)));
    REX_TRY_VOID(Validate(*entry, value, CvarSource::Default));
    entry->default_override = std::string(value);
    entry->has_default_override = true;
    if (entry->source == CvarSource::Default) {
      REX_TRY_VOID(Apply(*entry, value, CvarSource::Default, fired));
    }
  }
  Fire(fired);
  return Ok();
}

Status CvarRegistryImpl::Lock(std::string_view name) {
  std::lock_guard lock(mutex_);
  Entry* entry = FindEntry(name);
  if (!entry)
    return Err(MakeError(E::NotFound, std::format("no cvar named '{}'", name)));
  entry->locked = true;
  return Ok();
}

Status CvarRegistryImpl::ResetToDefault(std::string_view name) {
  Fired fired;
  {
    std::lock_guard lock(mutex_);
    Entry* entry = FindEntry(name);
    if (!entry)
      return Err(MakeError(E::NotFound, std::format("no cvar named '{}'", name)));
    std::string value(DefaultOf(*entry));
    REX_TRY_VOID(Apply(*entry, value, CvarSource::Default, fired));
  }
  Fire(fired);
  return Ok();
}

const IVar* CvarRegistryImpl::Find(std::string_view name) const {
  std::lock_guard lock(mutex_);
  const Entry* entry = FindEntry(name);
  return entry ? entry->var : nullptr;
}

CvarSource CvarRegistryImpl::Source(std::string_view name) const {
  std::lock_guard lock(mutex_);
  const Entry* entry = FindEntry(name);
  return entry ? entry->source : CvarSource::Default;
}

std::vector<const IVar*> CvarRegistryImpl::Enumerate(std::string_view scope) const {
  std::lock_guard lock(mutex_);
  std::vector<const IVar*> out;
  for (const Entry& e : entries_) {
    if (scope.empty() || e.var->desc().scope == scope)
      out.push_back(e.var);
  }
  return out;
}

std::vector<std::string_view> CvarRegistryImpl::Scopes() const {
  std::lock_guard lock(mutex_);
  return {scopes_.begin(), scopes_.end()};
}

Result<cvar::CallbackHandle> CvarRegistryImpl::OnChange(std::string_view name,
                                                        cvar::ChangeCallback callback) {
  std::lock_guard lock(mutex_);
  if (!FindEntry(name)) {
    return Err<cvar::CallbackHandle>(
        MakeError(E::NotFound, std::format("no cvar named '{}'", name)));
  }
  uint64_t id = next_callback_id_++;
  callbacks_.push_back(
      std::make_shared<Callback>(Callback{id, std::string(name), std::move(callback)}));
  return cvar::CallbackHandle(this, id);
}

void CvarRegistryImpl::RemoveCallback(uint64_t id) {
  std::lock_guard lock(mutex_);
  std::erase_if(callbacks_, [id](const std::shared_ptr<Callback>& cb) { return cb->id == id; });
}

Status CvarRegistryImpl::Invoke(std::string_view name, std::string_view args) {
  const IVar* var = nullptr;
  {
    std::lock_guard lock(mutex_);
    const Entry* entry = FindEntry(name);
    if (!entry)
      return Err(MakeError(E::NotFound, std::format("no command named '{}'", name)));
    if (entry->var->desc().type != cvar::Type::Command) {
      return Err(MakeError(E::NotACommand, std::format("'{}' is not a command", name)));
    }
    var = entry->var;
  }
  return const_cast<IVar*>(var)->Invoke(args);
}

std::vector<std::string> CvarRegistryImpl::PendingRestart() const {
  std::lock_guard lock(mutex_);
  return pending_restart_;
}

std::string CvarRegistryImpl::Serialize() const {
  std::lock_guard lock(mutex_);
  std::map<std::string_view, std::vector<std::string>> tables;
  for (const Entry& e : entries_) {
    const Desc& d = e.var->desc();
    if (d.type == cvar::Type::Command)
      continue;
    std::string value = e.var->Render();
    if (value == DefaultOf(e))
      continue;
    std::string_view leaf = d.name.substr(d.name.find('.') + 1);
    std::string rendered = d.type == cvar::Type::String ? EscapeToml(value) : value;
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
    for (CvarSource source : {CvarSource::Default, CvarSource::Config, CvarSource::Environment,
                              CvarSource::CommandLine}) {
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
        if (source == CvarSource::Default) {
          REX_TRY_VOID(Validate(*entry, o.value, source));
          entry->default_override = o.value;
          entry->has_default_override = true;
          if (entry->source != CvarSource::Default)
            continue;
        }
        REX_TRY_VOID(Apply(*entry, o.value, source, fired));
      }
    }
  }
  Fire(fired);
  return unknown;
}

Status CvarRegistryImpl::ApplyLocks(std::span<const std::string> names) {
  std::lock_guard lock(mutex_);
  for (const std::string& name : names) {
    Entry* entry = FindEntry(name);
    if (!entry) {
      return Err(MakeError(E::NotFound, std::format("cannot lock unknown cvar '{}'", name)));
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
    cvar::detail_access::set_registry(*e.var, nullptr);
  entries_.clear();
  index_.clear();
  scopes_.clear();
  pinned_scopes_.clear();
  pending_.clear();
  callbacks_.clear();
  pending_restart_.clear();
  finalized_ = false;
}

std::vector<std::string> CvarRegistryImpl::RegisteredNames() const {
  std::lock_guard lock(mutex_);
  std::vector<std::string> out;
  for (const Entry& e : entries_)
    out.emplace_back(e.var->desc().name);
  return out;
}

}  // namespace rex::detail

namespace rex::cvar {

void ApplyEnvironment(rex::Config& config) {
  for (auto& [name, value] : rex::platform::env::enumerate()) {
    if (!name.starts_with("REX_"))
      continue;
    std::string_view rest = std::string_view(name).substr(4);
    auto underscore = rest.find('_');
    if (underscore == std::string_view::npos || underscore == 0 || underscore + 1 >= rest.size()) {
      continue;
    }
    std::string cvar_name =
        std::format("{}.{}", rest.substr(0, underscore), rest.substr(underscore + 1));
    std::ranges::transform(cvar_name, cvar_name.begin(),
                           [](unsigned char c) { return char(std::tolower(c)); });
    config.cvars.values.push_back({cvar_name, value, CvarSource::Environment});
  }
}

}  // namespace rex::cvar
