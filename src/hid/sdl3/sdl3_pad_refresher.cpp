/**
 * @file        hid/sdl3/sdl3_pad_refresher.cpp
 * @brief       A thread that refreshes the SDL3 pads on request
 *
 * @copyright   Copyright (c) 2026 Dwreck
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "hid/sdl3/sdl3_pad_refresher.h"

#include <system_error>
#include <utility>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#elif defined(_M_ARM64)
#include <intrin.h>
#endif

namespace rex::hid::sdl3 {

namespace {

// One turn of a spin: tells the core the thread is waiting, where the CPU has an instruction for
// it.
inline void CpuRelax() {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
  _mm_pause();
#elif defined(_M_ARM64)
  __yield();
#elif defined(__aarch64__) || defined(__arm__)
  __asm__ __volatile__("yield");
#endif
}

}  // namespace

PadRefresher::PadRefresher(Options options) : options_(std::move(options)) {
  if (!options_.start_thread || !options_.refresh) {
    return;
  }
  try {
    thread_ = std::thread([this] { ThreadMain(); });
    started_ = true;
  } catch (const std::system_error&) {
    // No thread: IsRunning() stays false and every call returns kStopped.
    started_ = false;
  }
}

PadRefresher::~PadRefresher() {
  Stop();
}

bool PadRefresher::IsRunning() const {
  return started_ && !stopped_.load(std::memory_order_acquire);
}

i64 PadRefresher::SteadyUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

i64 PadRefresher::NowUs() const {
  if (options_.now_us) {
    return options_.now_us();
  }
  return SteadyUs();
}

PadRefresher::Result PadRefresher::RefreshAndWait(std::chrono::microseconds limit) {
  if (!IsRunning()) {
    ++stopped_count_;
    return Result::kStopped;
  }
  // A burst of reads (a title reads four users in a row) shares one round.
  if (rounds_.load(std::memory_order_acquire) != 0 &&
      NowUs() - last_end_us_.load(std::memory_order_acquire) < options_.recent_us) {
    ++recent_;
    return Result::kRecent;
  }
  // No lock here or anywhere below: this thread may be killed at any point (see Stop()).
  // Whether rounds are slow is read before the request is made: the round this call asks for must
  // not decide whether this call waits for it.
  const bool slow = slow_.load(std::memory_order_acquire);
  // The request's time, stored before the request is made, so a round that counts this request
  // reads this time or a later one (see ThreadMain).
  const i64 asked_us = SteadyUs();
  request_us_.store(asked_us);
  if (slow) {
    slow_request_us_.store(asked_us);
  }
  const u64 mine = requested_.fetch_add(1) + 1;
  wake_.fetch_add(1);
  wake_.notify_one();
  if (options_.after_request) {
    options_.after_request();
  }

  // Rounds are slow (a pad being plugged in can hold a round for a long time, or every round
  // takes long on this machine): one caller has paid the limit, nobody waits again until a round
  // has been quick. The request above still stands, so the state goes on being refreshed and a
  // quick round can be seen.
  if (slow) {
    ++skipped_;
    return Result::kSkipped;
  }

  // Not a timed OS wait: on Windows one can take a whole timer tick (15.6 ms) where a round takes
  // tens of microseconds. And no yield while the spin lasts: on a busy machine a yield hands the
  // core to another thread for up to a scheduler slice, and the round this call waits for is over
  // long before.
  const auto start = std::chrono::steady_clock::now();
  const auto deadline = start + limit;
  const auto spin_end = start + std::chrono::microseconds(options_.spin_us);
  for (;;) {
    if (done_.load(std::memory_order_acquire) >= mine) {
      ++fresh_;
      return Result::kFresh;
    }
    if (stopped_.load(std::memory_order_acquire)) {
      ++stopped_count_;
      return Result::kStopped;
    }
    if (slow_.load(std::memory_order_acquire)) {
      // Another caller gave up on the round this one is waiting for.
      ++skipped_;
      return Result::kSkipped;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      if (options_.at_deadline) {
        options_.at_deadline();
      }
      // One more look: this thread can lose its core between its look above and the clock read,
      // and its round can end meanwhile.
      if (done_.load(std::memory_order_acquire) >= mine) {
        ++fresh_;
        return Result::kFresh;
      }
      slow_limit_us_.store(limit.count(), std::memory_order_release);
      slow_.store(true, std::memory_order_release);
      ++timed_out_;
      return Result::kTimedOut;
    }
    if (now < spin_end) {
      CpuRelax();
    } else if (options_.yield) {
      options_.yield();
    } else {
      std::this_thread::yield();
    }
  }
}

void PadRefresher::ThreadMain() {
  if (options_.thread_setup) {
    options_.thread_setup();
  }
  u64 served = 0;  // done_, as this thread last stored it
  for (;;) {
    // Read wake_ first: a request made after this read changes it, so the wait below returns at
    // once and the request is not slept through.
    const u32 woken = wake_.load();
    if (stopped_.load(std::memory_order_acquire)) {
      return;
    }
    // Requests made from here on are not served by this round: it may already have read the
    // device when they arrive.
    const u64 noted = requested_.load();
    // Request times, read after the count: each request counted in `noted` stored its time before
    // it was counted, so these are the newest such request's or a later one's.
    const i64 asked_us = request_us_.load();
    const i64 asked_slow_us = slow_request_us_.load();
    if (options_.after_noted) {
      options_.after_noted();
    }
    if (noted == served) {
      wake_.wait(woken);
      continue;
    }
    const i64 begin = NowUs();
    const i64 t0 = SteadyUs();
    options_.refresh();
    const i64 t1 = SteadyUs();
    const i64 end = NowUs();
    const i64 took_us = t1 - t0;
    // Round costs. This thread is their only writer.
    if (took_us > longest_round_us_.load(std::memory_order_relaxed)) {
      longest_round_us_.store(took_us, std::memory_order_release);
    }
    round_us_sum_.store(round_us_sum_.load(std::memory_order_relaxed) + took_us,
                        std::memory_order_release);
    const i64 start_delay_us = t0 - asked_us;
    if (start_delay_us > longest_start_delay_us_.load(std::memory_order_relaxed)) {
      longest_start_delay_us_.store(start_delay_us, std::memory_order_release);
    }
    // Waits come back after a round that ended less than half the limit a caller gave up on after
    // the request of a skipped call it served: what a caller waits, not the round's own time (a
    // round that starts late is quick by its own clock). No such request yet (the stamp is 0, or
    // from an earlier slow spell): not quick.
    if (slow_.load(std::memory_order_acquire) &&
        (t1 - asked_slow_us) * 2 < slow_limit_us_.load(std::memory_order_acquire)) {
      slow_.store(false, std::memory_order_release);
    }
    last_begin_us_.store(begin);
    last_end_us_.store(end);
    rounds_.fetch_add(1);
    done_.store(noted);
    served = noted;
  }
}

void PadRefresher::Stop() {
  stopped_.store(true);
  wake_.fetch_add(1);
  wake_.notify_all();
  const std::lock_guard<std::mutex> lock(join_mutex_);
  if (thread_.joinable()) {
    thread_.join();
  }
}

PadRefresher::Counts PadRefresher::GetCounts() const {
  Counts c{};
  c.rounds = rounds_.load(std::memory_order_acquire);
  c.fresh = fresh_.load(std::memory_order_acquire);
  c.recent = recent_.load(std::memory_order_acquire);
  c.timed_out = timed_out_.load(std::memory_order_acquire);
  c.skipped = skipped_.load(std::memory_order_acquire);
  c.stopped = stopped_count_.load(std::memory_order_acquire);
  c.longest_round_us = longest_round_us_.load(std::memory_order_acquire);
  c.round_us_sum = round_us_sum_.load(std::memory_order_acquire);
  c.longest_start_delay_us = longest_start_delay_us_.load(std::memory_order_acquire);
  return c;
}

PadRefresher::RoundCost PadRefresher::RoundCostOf(const Counts& counts) {
  RoundCost cost{.mean_us = 0, .warn = false};
  if (counts.rounds != 0) {
    cost.mean_us = counts.round_us_sum / static_cast<i64>(counts.rounds);
  }
  cost.warn = counts.rounds >= kLongMeanMinRounds && cost.mean_us > kLongMeanRoundUs;
  return cost;
}

i64 PadRefresher::GetLongestRoundUs() const {
  return longest_round_us_.load(std::memory_order_acquire);
}

i64 PadRefresher::GetLastRoundBeginUs() const {
  return last_begin_us_.load(std::memory_order_acquire);
}

i64 PadRefresher::GetLastRoundEndUs() const {
  return last_end_us_.load(std::memory_order_acquire);
}

}  // namespace rex::hid::sdl3
