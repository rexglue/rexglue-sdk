/**
 * @file        base/settings/setting.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <format>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "base/logger.h"
#include "base/settings/settings_internal.h"

#include <rex/console_command.h>
#include <rex/logging/log_category.h>
#include <rex/platform/lock_order.h>
#include <rex/setting.h>
#include <rex/settings_registry.h>
#include <rex/utility.h>

namespace rex {

std::atomic<bool> g_startup_frozen{false};
std::atomic<bool> g_launch_frozen{false};

namespace {

/**
 * Deliveries of one watch from the queue, for the writes of one outermost write, before the drain
 * stops. A chain of settings each derived from the one before delivers each watch once per link,
 * and no chain in the SDK or its consumers is longer than two. A cycle with changing values stops
 * at this bound.
 */
constexpr u32 kMaxChangeRounds = 16;

struct ChangeQueue {
  u32 depth = 0;
  u32 batches = 0;
  bool running = false;
  size_t next = 0; /**< The first watch of pending that is not yet delivered. */
  std::vector<std::shared_ptr<detail::SettingWatch>> pending;
  /** The queue deliveries of each watch in the drain, cleared when it ends. */
  std::vector<std::pair<const detail::SettingWatch*, u32>> deliveries;
};

thread_local ChangeQueue t_changes;

void ReportSettingOrder() {
  platform::detail::PushRankChecked(platform::LockRank::kLeaf);
  platform::detail::PushRankChecked(platform::lock_rank::kSettingsWatch);
  platform::detail::PopRankChecked(platform::lock_rank::kSettingsWatch);
  platform::detail::PopRankChecked(platform::LockRank::kLeaf);
}

void Enqueue(const std::shared_ptr<detail::SettingWatch>& watch) {
  const auto waiting = t_changes.pending.begin() + static_cast<std::ptrdiff_t>(t_changes.next);
  if (std::find(waiting, t_changes.pending.end(), watch) == t_changes.pending.end()) {
    t_changes.pending.push_back(watch);
  }
}

/** Counts one delivery from the queue and returns the count of that watch in this drain. */
u32 CountDelivery(const detail::SettingWatch* watch) {
  auto& deliveries = t_changes.deliveries;
  for (auto& entry : deliveries) {
    if (entry.first == watch) {
      return ++entry.second;
    }
  }
  deliveries.emplace_back(watch, 1);
  return 1;
}

std::string NameWatch(const detail::SettingWatch* watch) {
  std::string out;
  for (const SettingBase* setting : watch->GetSettings()) {
    if (!out.empty()) {
      out += ", ";
    }
    out += setting->GetName();
  }
  return out;
}

/** True when the calling thread delivers immediately instead of queueing. */
bool CanDeliver() {
  const ChangeQueue& queue = t_changes;
  return queue.depth == 0 && queue.batches == 0 && !queue.running;
}

/**
 * Runs the queued change calls of the thread in the order they were queued, each watch once while
 * it waits in the list.
 *
 * @param first The watches delivered before the queued ones. Empty when the queue holds them all.
 */
void RunPending(std::span<const std::shared_ptr<detail::SettingWatch>> first = {}) {
  ChangeQueue& queue = t_changes;
  if (!CanDeliver()) {
    return;
  }
  queue.running = true;
  const ScopeGuard finish([&queue] {
    queue.running = false;
    queue.pending.clear();
    queue.next = 0;
    queue.deliveries.clear();
  });
  for (const auto& watch : first) {
    watch->Deliver();
  }
  while (queue.next < queue.pending.size()) {
    // A copy, since the call may queue a watch and move the list.
    const std::shared_ptr<detail::SettingWatch> watch = queue.pending[queue.next++];
    if (queue.next == queue.pending.size()) {
      queue.pending.clear();
      queue.next = 0;
    }
    if (CountDelivery(watch.get()) > kMaxChangeRounds) {
      REXSYS_ERROR("setting change calls stopped after {} calls of the watch of {}",
                   kMaxChangeRounds, NameWatch(watch.get()));
      return;
    }
    watch->Deliver();
  }
}

/** The address of this is the tag of the calling thread, which names the thread that holds a call.
 */
thread_local char t_thread_tag = 0;

/**
 * Holds rank 0 for a delivery, so a caller holding a ranked lock is reported, and every lock the
 * function takes ranks above it.
 */
struct WatchRank {
  WatchRank() { platform::detail::PushRankChecked(platform::lock_rank::kSettingsWatch); }
  ~WatchRank() { platform::detail::PopRankChecked(platform::lock_rank::kSettingsWatch); }
  WatchRank(const WatchRank&) = delete;
  WatchRank& operator=(const WatchRank&) = delete;
};

struct ChangeCall {
  ChangeCall() { ++t_changes.depth; }
  ~ChangeCall() { --t_changes.depth; }
  ChangeCall(const ChangeCall&) = delete;
  ChangeCall& operator=(const ChangeCall&) = delete;
};

}  // namespace

Result<void> CheckSettingWrite(std::string_view name) {
  if (log::IsInSinks()) {
    ReportSettingOrder();
    return Err(ErrorCategory::kConfig, std::format("{} cannot change inside a log sink", name));
  }
  return Ok();
}

namespace detail {

SettingWatch::SettingWatch(std::vector<SettingBase*> settings)
    : settings_(std::move(settings)), seen_(settings_.size(), 0) {}

SettingWatch::~SettingWatch() = default;

/*
 * A writer never queues behind a running call. Each thread that arrives takes a ticket under
 * mutex_. One thread at a time holds the call, and it makes exactly one call, which serves every
 * ticket issued before that call starts. A thread that arrives while the call is held waits on
 * served_ without a lock. Its ticket is issued while the call in progress runs, so the call in
 * progress may not serve it, and the next call does, because that call starts after the ticket is
 * issued. The thread that ends a call hands the next call to the oldest waiting ticket, so the
 * next call is made by a waiting thread, never by one that keeps writing. A waiter therefore
 * returns after at most the call in progress plus one, under any scheduling.
 *
 * Removal: Remove stores removed_ and then reads runner_. The thread that holds the call stores
 * runner_ and then reads removed_ before calling. All four are seq_cst, so in their single total
 * order either Remove reads the runner and waits for it, or the runner reads removed_ and makes
 * no call. After the call it clears runner_ and then reads removed_, both seq_cst, so a Remove
 * that read the runner is woken. A removed watch starts no new call, so Remove waits for at most
 * the call in progress.
 */
void SettingWatch::Deliver() {
  if (removed_.load(std::memory_order_acquire)) {
    return;
  }
  const WatchRank rank;
  std::unique_lock lock(mutex_);
  if (removed_.load(std::memory_order_relaxed)) {
    return;
  }
  if (!running_) {
    if (!HasNewer()) {
      return;
    }
    running_ = true;
    Run(lock, ++requests_);
    return;
  }
  const u64 ticket = ++requests_;
  lock.unlock();
  if (WaitTurn(ticket)) {
    lock.lock();
    Run(lock, ticket);
  }
}

bool SettingWatch::HasNewer() const {
  for (size_t i = 0; i < settings_.size(); ++i) {
    if (settings_[i]->GetGeneration() > seen_[i]) {
      return true;
    }
  }
  return false;
}

void SettingWatch::Run(std::unique_lock<std::mutex>& lock, u64 ticket) {
  runner_.store(&t_thread_tag, std::memory_order_seq_cst);
  const u64 covering = requests_;
  const bool call = !removed_.load(std::memory_order_seq_cst) && HasNewer();
  lock.unlock();
  /** Ends the call held by this thread, also when the function throws. */
  struct End {
    SettingWatch& watch;
    std::unique_lock<std::mutex>& state;
    u64 own;    /**< The ticket of the thread. */
    u64 served; /**< The last ticket the call serves. */
    ~End() {
      watch.runner_.store(nullptr, std::memory_order_seq_cst);
      if (watch.removed_.load(std::memory_order_seq_cst)) {
        watch.runner_.notify_all();
      }
      state.lock();
      watch.HandOn(state, own, served);
    }
  };
  const End end{*this, lock, ticket, covering};
  if (call) {
    const ChangeCall depth;
    Call();
  }
}

void SettingWatch::HandOn(std::unique_lock<std::mutex>& lock, u64 own, u64 served) {
  if (requests_ > served && !removed_.load(std::memory_order_relaxed)) {
    // The handoff is written before served_, whose release store the waiter reads with acquire.
    handoff_.store(served + 1, std::memory_order_relaxed);
    served_.store(served, std::memory_order_release);
  } else {
    running_ = false;
    served_.store(requests_, std::memory_order_release);
  }
  const bool waiters = requests_ > own;
  lock.unlock();
  if (waiters) {
    served_.notify_all();
  }
}

/*
 * The hold takes a ticket like an arriving writer that becomes the runner, so a write that arrives
 * while the watch is linked takes a later ticket and waits. Ending the hold serves only the hold's
 * own ticket, so the oldest waiting writer is handed the next call, and that call runs only when a
 * generation is newer than the one the hold marked seen. A write that stores after a value is read
 * is linked to the watch by then, so it either takes a ticket during the hold or arrives after it,
 * and either way reaches the function as a change.
 */
SettingWatch::Hold::Hold(SettingWatch& watch) : watch_(watch) {
  std::lock_guard lock(watch_.mutex_);
  watch_.running_ = true;
  ticket_ = ++watch_.requests_;
}

SettingWatch::Hold::~Hold() {
  std::unique_lock lock(watch_.mutex_);
  watch_.HandOn(lock, ticket_, ticket_);
}

bool SettingWatch::WaitTurn(u64 ticket) const {
  for (;;) {
    const u64 served = served_.load(std::memory_order_acquire);
    if (served >= ticket) {
      return false;
    }
    if (served + 1 == ticket && handoff_.load(std::memory_order_relaxed) == ticket) {
      return true;
    }
    served_.wait(served, std::memory_order_acquire);
  }
}

void SettingWatch::Unlink(const SettingBase* skip) {
  if (unlinked_.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  for (SettingBase* setting : settings_) {
    if (setting != skip) {
      setting->UnlinkWatch(this);
    }
  }
}

void SettingWatch::Remove() {
  removed_.store(true, std::memory_order_seq_cst);
  Unlink(nullptr);
  for (;;) {
    const void* runner = runner_.load(std::memory_order_seq_cst);
    if (!runner || runner == &t_thread_tag) {
      break;
    }
    runner_.wait(runner, std::memory_order_acquire);
  }
}

void SettingWatch::Detach(const SettingBase& setting) {
  removed_.store(true, std::memory_order_release);
  Unlink(&setting);
}

std::span<SettingBase* const> SettingWatch::GetSettings() const {
  return settings_;
}

void SettingWatch::MarkSeen(size_t index, u64 generation) {
  seen_[index] = generation;
}

void SettingWatch::Link(const std::shared_ptr<SettingWatch>& self) {
  for (SettingBase* setting : self->settings_) {
    setting->LinkWatch(self);
  }
}

ScopeGuard SettingWatch::Guard(std::shared_ptr<SettingWatch> self) {
  return ScopeGuard([watch = std::move(self)] { watch->Remove(); });
}

ScopeGuard AddWatch(std::shared_ptr<SettingWatch> watch) {
  SettingWatch::Link(watch);
  if (IsInSettingChange()) {
    Enqueue(watch);
  } else {
    watch->Deliver();
    RunPending();
  }
  return SettingWatch::Guard(std::move(watch));
}

bool IsInSettingChange() {
  return t_changes.depth > 0;
}

}  // namespace detail

SettingsBatch::SettingsBatch() {
  ++t_changes.batches;
}

SettingsBatch::~SettingsBatch() {
  --t_changes.batches;
  RunPending();
}

SettingBase::SettingBase(SettingOwner& owner, std::string_view name, SettingType type,
                         std::string_view help)
    : state_(std::make_unique<State>()) {
  state_->owner = &owner;
  state_->name = std::format("{}.{}", owner.GetScope(), name);
  state_->help = help;
  state_->type = type;
  owner.state_->settings.push_back(this);
}

SettingBase::~SettingBase() {
  std::shared_ptr<const State::WatchList> watches;
  {
    std::lock_guard lock(state_->watches_mutex);
    watches = std::exchange(state_->watches, {});
  }
  if (watches) {
    for (const auto& watch : *watches) {
      watch->Detach(*this);
    }
  }
  SettingOwner::State& owner = *state_->owner->state_;
  if (owner.registered) {
    REX_SETTINGS().Detach(*this);
  }
  std::erase(owner.settings, this);
}

const std::string& SettingBase::GetName() const {
  return state_->name;
}

const std::string& SettingBase::GetHelp() const {
  return state_->help;
}

SettingType SettingBase::GetType() const {
  return state_->type;
}

const SettingConstraints& SettingBase::GetConstraints() const {
  return state_->constraints;
}

SettingOwner& SettingBase::GetOwner() const {
  return *state_->owner;
}

SettingLayer SettingBase::GetLayer() const {
  return state_->layer.load(std::memory_order_acquire);
}

SettingConstraints& SettingBase::EditConstraints() {
  return state_->constraints;
}

bool SettingBase::IsFrozen() const {
  if (GetLayer() == SettingLayer::kLock) {
    return true;
  }
  switch (state_->constraints.mode) {
    case SettingMode::kInitOnly:
      return g_startup_frozen.load();
    case SettingMode::kLockedInGame:
      return g_launch_frozen.load();
    default:
      return false;
  }
}

bool SettingBase::IsRestartPending() const {
  return state_->restart_pending.load(std::memory_order_acquire);
}

u64 SettingBase::GetGeneration() const {
  return state_->generation.load(std::memory_order_acquire);
}

std::string SettingBase::GetText() const {
  return Render();
}

std::string SettingBase::GetDefaultText() const {
  return RenderDefault();
}

Result<void> SettingBase::Check(std::string_view text) const {
  const auto fail = [&](std::string_view why) {
    return Err(ErrorCategory::kConfig, std::format("{}: '{}' {}", state_->name, text, why));
  };
  f64 numeric = 0;
  bool is_numeric = true;
  switch (state_->type) {
    case SettingType::kBool: {
      bool v;
      is_numeric = false;
      if (!detail::ParseSetting(text, v))
        return fail("is not a boolean");
      break;
    }
    case SettingType::kI32: {
      i32 v;
      if (!detail::ParseSetting(text, v))
        return fail("is not a 32 bit integer");
      numeric = v;
      break;
    }
    case SettingType::kI64: {
      i64 v;
      if (!detail::ParseSetting(text, v))
        return fail("is not a 64 bit integer");
      numeric = static_cast<f64>(v);
      break;
    }
    case SettingType::kU32: {
      u32 v;
      if (!detail::ParseSetting(text, v))
        return fail("is not an unsigned 32 bit integer");
      numeric = v;
      break;
    }
    case SettingType::kU64: {
      u64 v;
      if (!detail::ParseSetting(text, v))
        return fail("is not an unsigned 64 bit integer");
      numeric = static_cast<f64>(v);
      break;
    }
    case SettingType::kF64: {
      f64 v;
      if (!detail::ParseSetting(text, v))
        return fail("is not a number");
      numeric = v;
      break;
    }
    default:
      is_numeric = false;
      break;
  }
  const auto& c = state_->constraints;
  if (is_numeric) {
    if (c.min && numeric < *c.min)
      return fail(std::format("is below {}", *c.min));
    if (c.max && numeric > *c.max)
      return fail(std::format("is above {}", *c.max));
  }
  if (!c.allowed.empty() && state_->type == SettingType::kString &&
      std::ranges::find(c.allowed, std::string(text)) == c.allowed.end()) {
    return fail("is not an accepted value");
  }
  return Ok();
}

Result<void> SettingBase::Admit(SettingLayer layer, CommitKind kind) const {
  if (kind == CommitKind::kEnd) {
    return Ok();
  }
  if (kind == CommitKind::kWrite && GetLayer() == SettingLayer::kLock) {
    return Err(ErrorCategory::kConfig, std::format("{} is locked", state_->name));
  }
  if (IsFrozen()) {
    return Err(ErrorCategory::kConfig, std::format("{} can no longer change", state_->name));
  }
  if (kind == CommitKind::kWrite && layer < GetLayer()) {
    return Err(ErrorCategory::kConfig, std::format("{} is held by a higher layer", state_->name));
  }
  return Ok();
}

bool SettingBase::FinishCommit(SettingLayer layer, CommitKind kind, bool changed) {
  state_->layer.store(kind == CommitKind::kWrite ? layer : SettingLayer::kDefault,
                      std::memory_order_release);
  if (kind == CommitKind::kEnd) {
    state_->restart_pending.store(false, std::memory_order_release);
    return true;
  }
  if (!changed) {
    return false;
  }
  if (kind == CommitKind::kWrite && state_->constraints.mode == SettingMode::kRequiresRestart &&
      g_startup_frozen.load()) {
    state_->restart_pending.store(true, std::memory_order_release);
    return false;
  }
  state_->restart_pending.store(false, std::memory_order_release);
  return true;
}

void SettingBase::AdvanceGeneration() {
  state_->generation.fetch_add(1, std::memory_order_acq_rel);
}

Result<void> SettingBase::StoreFailed(std::string_view text, CommitKind kind) const {
  if (kind == CommitKind::kWrite) {
    return Err(ErrorCategory::kConfig, std::format("{}: '{}' did not store", state_->name, text));
  }
  return Err(ErrorCategory::kConfig, std::format("{}: default did not store", state_->name));
}

void SettingBase::Notify() {
  std::shared_ptr<const State::WatchList> watches;
  {
    std::lock_guard lock(state_->watches_mutex);
    watches = state_->watches;
  }
  if (!watches) {
    return;
  }
  if (CanDeliver()) {
    RunPending(*watches);
    return;
  }
  for (const auto& watch : *watches) {
    Enqueue(watch);
  }
}

void SettingBase::LinkWatch(std::shared_ptr<detail::SettingWatch> watch) {
  std::lock_guard lock(state_->watches_mutex);
  auto next = state_->watches ? std::make_shared<State::WatchList>(*state_->watches)
                              : std::make_shared<State::WatchList>();
  next->push_back(std::move(watch));
  state_->watches = std::move(next);
}

void SettingBase::UnlinkWatch(const detail::SettingWatch* watch) {
  std::lock_guard lock(state_->watches_mutex);
  if (!state_->watches) {
    return;
  }
  auto next = std::make_shared<State::WatchList>(*state_->watches);
  std::erase_if(*next, [watch](const auto& entry) { return entry.get() == watch; });
  if (next->empty()) {
    state_->watches.reset();
  } else {
    state_->watches = std::move(next);
  }
}

Result<void> SettingBase::SetText(SettingLayer layer, std::string_view text) {
  REX_TRY_VOID(CheckSettingWrite(state_->name));
  REX_TRY_VOID(Admit(layer, CommitKind::kWrite));
  REX_TRY_VOID(Check(text));
  return Commit(layer, text, CommitKind::kWrite);
}

Result<void> SettingBase::SetFromCode(std::string_view text) {
  return SetText(g_launch_frozen.load() ? SettingLayer::kRuntime : SettingLayer::kTitle, text);
}

Result<void> SettingBase::Reset() {
  REX_TRY_VOID(CheckSettingWrite(state_->name));
  REX_TRY_VOID(Admit(SettingLayer::kDefault, CommitKind::kReset));
  return Commit(SettingLayer::kDefault, RenderDefault(), CommitKind::kReset);
}

SettingOwner::SettingOwner(std::string_view scope) : state_(std::make_unique<State>()) {
  state_->scope = scope;
}

SettingOwner::~SettingOwner() = default;

const std::string& SettingOwner::GetScope() const {
  return state_->scope;
}

std::span<SettingBase* const> SettingOwner::GetOwnedSettings() const {
  return state_->settings;
}

ConsoleCommand::ConsoleCommand(SettingOwner* owner, std::string_view name, std::string_view help,
                               MoveOnlyFunction<Result<void>(std::string_view)> run)
    : state_(std::make_unique<State>()) {
  state_->owner = owner;
  state_->name = std::format("{}.{}", owner->GetScope(), name);
  state_->help = help;
  state_->run = std::move(run);
  owner->state_->commands.push_back(this);
}

ConsoleCommand::~ConsoleCommand() {
  SettingOwner::State& owner = *state_->owner->state_;
  if (owner.registered) {
    REX_SETTINGS().Detach(*this);
  }
  std::erase(owner.commands, this);
}

const std::string& ConsoleCommand::GetName() const {
  return state_->name;
}

const std::string& ConsoleCommand::GetHelp() const {
  return state_->help;
}

Result<void> ConsoleCommand::Run(std::string_view args) {
  return state_->run(args);
}

}  // namespace rex
