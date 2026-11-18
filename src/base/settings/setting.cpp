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
#include <deque>
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

namespace detail {

struct ChangeThread {
  std::atomic<SettingWatch*> running{nullptr};
  std::atomic<SettingWatch*> waiting_on{nullptr};
};

}  // namespace detail

namespace {

/**
 * Rounds of change calls a thread runs for one outermost write before the last round. A round is
 * the calls made for the writes of the round before, so a chain of settings each derived from the
 * one before takes one round per link. No chain in the SDK or its consumers is longer than two, and
 * a cycle with changing values stops after these rounds.
 */
constexpr u32 kMaxChangeRounds = 16;

struct ChangeQueue {
  u32 depth = 0;
  u32 batches = 0;
  bool running = false;
  bool last_round = false;
  std::vector<std::shared_ptr<detail::SettingWatch>> pending;
  std::vector<std::shared_ptr<detail::SettingWatch>> round; /**< Kept so its capacity is reused. */
};

thread_local ChangeQueue t_changes;

void ReportSettingOrder() {
  platform::detail::PushRankChecked(platform::LockRank::kLeaf);
  platform::detail::PushRankChecked(platform::lock_rank::kSettingsWatch);
  platform::detail::PopRankChecked(platform::lock_rank::kSettingsWatch);
  platform::detail::PopRankChecked(platform::LockRank::kLeaf);
}

void Enqueue(const std::shared_ptr<detail::SettingWatch>& watch) {
  if (std::ranges::find(t_changes.pending, watch) == t_changes.pending.end()) {
    t_changes.pending.push_back(watch);
  }
}

std::string NameRound(std::span<const std::shared_ptr<detail::SettingWatch>> round) {
  std::vector<std::string_view> names;
  for (const auto& watch : round) {
    for (const SettingBase* setting : watch->GetSettings()) {
      if (std::ranges::find(names, std::string_view(setting->GetName())) == names.end()) {
        names.emplace_back(setting->GetName());
      }
    }
  }
  std::string out;
  for (const std::string_view name : names) {
    if (!out.empty()) {
      out += ", ";
    }
    out += name;
  }
  return out;
}

/** True when the calling thread delivers immediately instead of queueing. */
bool CanDeliver() {
  const ChangeQueue& queue = t_changes;
  return queue.depth == 0 && queue.batches == 0 && !queue.running;
}

/**
 * Runs the queued change calls of the thread in rounds.
 *
 * @param first The watches of the first round, delivered before the queued ones. Empty when the
 * queue holds the first round.
 */
void RunPending(std::span<const std::shared_ptr<detail::SettingWatch>> first = {}) {
  ChangeQueue& queue = t_changes;
  if (!CanDeliver()) {
    return;
  }
  queue.running = true;
  const ScopeGuard finish([&queue] {
    queue.running = false;
    queue.last_round = false;
    queue.pending.clear();
    queue.round.clear();
  });
  u32 rounds = 0;
  if (!first.empty()) {
    for (const auto& watch : first) {
      watch->Deliver();
    }
    ++rounds;
  }
  while (!queue.pending.empty()) {
    if (rounds == kMaxChangeRounds) {
      queue.last_round = true;
      REXSYS_ERROR("setting change calls stopped after {} rounds, the last round calls {}",
                   kMaxChangeRounds, NameRound(queue.pending));
    }
    queue.round.clear();
    std::swap(queue.round, queue.pending);
    for (const auto& watch : queue.round) {
      watch->Deliver();
    }
    ++rounds;
  }
}

/**
 * Steps of the wait-for walk before it gives up looking for a cycle. Each step crosses one thread
 * that runs a watch and waits to remove another, so this bounds the threads in a cycle.
 */
constexpr u32 kMaxWaitSteps = 64;

/**
 * Hands out the entries of the threads that run change calls. An entry goes back to the free list
 * when its thread ends and is never freed, so another thread can read one at any time.
 */
class ChangeThreadPool {
 public:
  detail::ChangeThread* Take();
  void Give(detail::ChangeThread* entry);

 private:
  std::mutex mutex_; /**< Unranked, it takes no other lock and calls no other code. */
  std::deque<detail::ChangeThread> entries_;
  std::vector<detail::ChangeThread*> free_;
};

ChangeThreadPool& GetChangeThreadPool() {
  static auto* pool = new ChangeThreadPool();
  return *pool;
}

struct ChangeThreadHolder {
  constexpr ChangeThreadHolder() = default;
  ~ChangeThreadHolder() {
    if (entry) {
      GetChangeThreadPool().Give(entry);
    }
  }
  ChangeThreadHolder(const ChangeThreadHolder&) = delete;
  ChangeThreadHolder& operator=(const ChangeThreadHolder&) = delete;

  detail::ChangeThread* entry = nullptr;
};

thread_local ChangeThreadHolder t_thread;

/** Returns the entry of the calling thread, taking one from the pool on the first call. */
detail::ChangeThread& GetChangeThread() {
  if (!t_thread.entry) {
    t_thread.entry = GetChangeThreadPool().Take();
  }
  return *t_thread.entry;
}

detail::ChangeThread* ChangeThreadPool::Take() {
  std::lock_guard lock(mutex_);
  if (!free_.empty()) {
    detail::ChangeThread* entry = free_.back();
    free_.pop_back();
    return entry;
  }
  return &entries_.emplace_back();
}

void ChangeThreadPool::Give(detail::ChangeThread* entry) {
  entry->running.store(nullptr, std::memory_order_seq_cst);
  entry->waiting_on.store(nullptr, std::memory_order_seq_cst);
  std::lock_guard lock(mutex_);
  free_.push_back(entry);
}

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

}  // namespace

Result<void> CheckSettingWrite(std::string_view name) {
  if (log::IsInSinks()) {
    ReportSettingOrder();
    return Err(ErrorCategory::kConfig, std::format("{} cannot change inside a log sink", name));
  }
  if (t_changes.last_round && t_changes.depth > 0) {
    ReportSettingOrder();
    return Err(ErrorCategory::kConfig,
               std::format("{} cannot change, the change calls stopped after {} rounds", name,
                           kMaxChangeRounds));
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
  ChangeThread& self = GetChangeThread();
  runner_.store(&self, std::memory_order_seq_cst);
  const u64 covering = requests_;
  const bool call = !removed_.load(std::memory_order_seq_cst) && HasNewer();
  lock.unlock();
  /** Ends the call held by this thread, also when the function throws. */
  struct End {
    SettingWatch& watch;
    std::unique_lock<std::mutex>& state;
    ChangeThread& thread;
    u64 own;    /**< The ticket of the thread. */
    u64 served; /**< The last ticket the call serves. */
    ~End() {
      watch.runner_.store(nullptr, std::memory_order_seq_cst);
      if (watch.removed_.load(std::memory_order_seq_cst)) {
        watch.runner_.notify_all();
      }
      thread.running.store(nullptr, std::memory_order_relaxed);
      state.lock();
      if (watch.requests_ > served && !watch.removed_.load(std::memory_order_relaxed)) {
        // The handoff is written before served_, whose release store the waiter reads with
        // acquire.
        watch.handoff_.store(served + 1, std::memory_order_relaxed);
        watch.served_.store(served, std::memory_order_release);
      } else {
        watch.running_ = false;
        watch.served_.store(watch.requests_, std::memory_order_release);
      }
      const bool waiters = watch.requests_ > own;
      state.unlock();
      if (waiters) {
        watch.served_.notify_all();
      }
    }
  };
  const End end{*this, lock, self, ticket, covering};
  if (call) {
    self.running.store(this, std::memory_order_relaxed);
    const ChangeCall depth;
    Call();
  }
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

bool SettingWatch::WaitLeadsBack(const ChangeThread& self) const {
  const SettingWatch* watch = this;
  for (u32 step = 0; step < kMaxWaitSteps && watch; ++step) {
    const ChangeThread* runner = watch->runner_.load(std::memory_order_seq_cst);
    if (!runner) {
      return false;
    }
    if (runner == &self) {
      return true;
    }
    watch = runner->waiting_on.load(std::memory_order_seq_cst);
  }
  return false;
}

void SettingWatch::Remove() {
  removed_.store(true, std::memory_order_seq_cst);
  Unlink(nullptr);
  ChangeThread& self = GetChangeThread();
  // Each thread in a cycle stores its waiting_on and then reads the runner_ and waiting_on of the
  // others. All of these are seq_cst, so in their single total order the last thread to store its
  // waiting_on reads every other store of the cycle and finds it, and no cycle goes unseen.
  self.waiting_on.store(this, std::memory_order_seq_cst);
  for (;;) {
    ChangeThread* runner = runner_.load(std::memory_order_seq_cst);
    if (!runner || runner == &self) {
      break;
    }
    if (WaitLeadsBack(self)) {
      // Only this thread writes its running field, so a relaxed read sees its own last store.
      const SettingWatch* running = self.running.load(std::memory_order_relaxed);
      REXSYS_ERROR(
          "the change call of {} removes the watch of {}, whose call waits to remove it, so the "
          "removal does not wait",
          running ? NameWatch(running) : std::string("no setting"), NameWatch(this));
      ReportSettingOrder();
      break;
    }
    runner_.wait(runner, std::memory_order_acquire);
  }
  self.waiting_on.store(nullptr, std::memory_order_seq_cst);
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

ScopeGuard AddWatch(std::shared_ptr<SettingWatch> watch) {
  SettingWatch::Link(watch);
  if (IsInSettingChange()) {
    Enqueue(watch);
  } else {
    watch->Deliver();
    RunPending();
  }
  return ScopeGuard([watch = std::move(watch)] { watch->Remove(); });
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
