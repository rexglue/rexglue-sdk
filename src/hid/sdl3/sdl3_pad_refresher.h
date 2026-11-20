/**
 * @file        hid/sdl3/sdl3_pad_refresher.h
 * @brief       A thread that refreshes the SDL3 pads on request (private to the module)
 *
 * @copyright   Copyright (c) 2026 Dwreck
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <thread>

#include <rex/types.h>

namespace rex::hid::sdl3 {

/**
 * Runs a pad refresh ("a round") on its own thread when a reader asks, and lets the reader wait
 * for that round, up to a limit.
 *
 * A pad read takes the state from the events a refresh delivered. With one refresh per read,
 * posted to another thread, the read sees the previous refresh's state, one poll old. Here the
 * read asks for a round and waits until a round that began after its request has ended.
 *
 * No SDL here, so the class builds into a unit test with a fake round.
 */
class PadRefresher {
 public:
  enum class Result : u8 {
    kFresh,    /**< A round that began after this call has ended. */
    kRecent,   /**< A round ended less than recent_us ago: no new round. */
    kTimedOut, /**< The round did not end within the limit. */
    kSkipped,  /**< Rounds are slow (a caller's wait ran out): a round was asked for, no wait. */
    kStopped,  /**< No thread (not started, no round function, or it could not start), or Stop(). */
  };

  struct Counts {
    u64 rounds; /**< Rounds ended. */
    u64 fresh;  /**< Results returned, each call counted once. */
    u64 recent;
    u64 timed_out;
    u64 skipped;
    u64 stopped;
    /** What rounds cost, on steady_clock in microseconds (0 before the first round). */
    i64 longest_round_us;       /**< The longest round. */
    i64 round_us_sum;           /**< All rounds together (mean = this / rounds). */
    i64 longest_start_delay_us; /**< The longest time from a request to the start of the round
                                     that served it (the newest request it served). */
  };

  /**
   * What the closing log line says about the cost of the rounds. A session whose MEAN round is
   * longer than kLongMeanRoundUs gets one warning, once at least kLongMeanMinRounds rounds ran:
   * every frame's first read waits for its round. A single long round is normal (a battery query
   * takes a few milliseconds now and then, device detection after an unplug a few hundred), so the
   * longest round is reported and never warned about.
   */
  static constexpr i64 kLongMeanRoundUs = 500;
  static constexpr u64 kLongMeanMinRounds = 1000;
  struct RoundCost {
    i64 mean_us; /**< round_us_sum / rounds, rounded down; 0 when no round ran. */
    bool warn;   /**< rounds >= kLongMeanMinRounds and mean_us > kLongMeanRoundUs. */
  };
  static RoundCost RoundCostOf(const Counts& counts);

  struct Options {
    std::function<void()> refresh;      /**< One round; runs on the refresher's thread only. */
    std::function<void()> thread_setup; /**< Optional; runs once on that thread first. */
    std::function<i64()> now_us;        /**< Optional; default steady_clock in microseconds. */
    i64 recent_us = 1000;
    /**
     * A waiting caller keeps its core this long before it starts to yield it between looks. A
     * round takes tens of microseconds; a yield on a busy machine can cost a whole scheduler slice.
     */
    i64 spin_us = 300;
    std::function<void()> yield; /**< Optional; default std::this_thread::yield. */
    /** Optional, for tests: runs on the caller's thread right after its request, before its wait.
     */
    std::function<void()> after_request;
    /**
     * Optional, for tests: runs on the refresher's thread right after it read the request count,
     * before it sleeps or runs a round.
     */
    std::function<void()> after_noted;
    /**
     * Optional, for tests: runs on the caller's thread when its wait has reached the limit, before
     * its last look at whether its round has ended.
     */
    std::function<void()> at_deadline;
    bool start_thread = true; /**< false: behaves as a refresher whose thread could not start. */
  };

  explicit PadRefresher(Options options);
  /** Calls Stop(). */
  ~PadRefresher();

  PadRefresher(const PadRefresher&) = delete;
  PadRefresher& operator=(const PadRefresher&) = delete;
  PadRefresher(PadRefresher&&) = delete;
  PadRefresher& operator=(PadRefresher&&) = delete;

  /** The thread started and Stop() was not called. */
  bool IsRunning() const;

  /**
   * Asks for a round and waits for it, `limit` at most. Never runs the round on the calling
   * thread. From any thread but the refresher's own.
   *
   * When a wait runs out, rounds count as slow: that caller gets kTimedOut, callers waiting beside
   * it and later callers get kSkipped without a wait (each still asks for a round), until a round
   * has ended less than half that limit after the request of a skipped call it served. So slow
   * rounds, and rounds that start late, cost one limit, not one per call.
   *
   * The object must outlive every call: destroy it only when no thread can still be inside
   * RefreshAndWait (Stop() makes calls return, it does not wait for them to have returned).
   */
  Result RefreshAndWait(std::chrono::microseconds limit);

  /**
   * Waiters leave at once and later calls return kStopped; then the thread is joined (a round in
   * flight is waited for). Idempotent; from any thread but the refresher's own.
   *
   * RefreshAndWait and GetCounts() take no lock, and Stop() takes only the one that orders two
   * Stop() calls. So a caller's thread that is killed inside RefreshAndWait leaves no lock of this
   * class held. Not covered: whatever the OS's own wake call holds for an instant, and any lock the
   * round takes (Stop() waits for a round in flight).
   */
  void Stop();

  Counts GetCounts() const;
  /** Counts::longest_round_us alone (one load). */
  i64 GetLongestRoundUs() const;
  /**
   * 0 before the first round; on the now_us clock. Two separate reads: while rounds run, a begin
   * and an end read one after the other can belong to different rounds.
   */
  i64 GetLastRoundBeginUs() const;
  i64 GetLastRoundEndUs() const;

 private:
  void ThreadMain();
  i64 NowUs() const;
  static i64 SteadyUs();  // steady_clock in microseconds, whatever now_us is

  Options options_;
  bool started_ = false;  // set in the constructor only
  std::atomic<bool> stopped_{false};

  // Request numbers start at 1. A request is done when a round that began after it was made has
  // ended. A round's end is published in this order: the two stamps, rounds_, then done_.
  std::atomic<u64> requested_{0};
  std::atomic<u64> done_{0};
  // The thread sleeps on this (std::atomic wait); every request and Stop() change it and notify.
  // 32 bits: the size the OS waits on directly.
  std::atomic<u32> wake_{0};
  // Set when a caller's wait runs out, with that caller's limit; while it is set nobody waits. A
  // call that finds it set stores its request time in slow_request_us_ before it makes the
  // request; the thread clears slow_ after a round that ended less than half that limit after the
  // stamp it read with the round's request count. The caller's clock, not the round's own
  // duration: the thread cannot see how late it was woken.
  std::atomic<bool> slow_{false};
  std::atomic<i64> slow_limit_us_{0};
  std::atomic<i64> slow_request_us_{0};  // steady_clock us; 0 = none yet

  // Every request's time (steady_clock us), stored before the request is made; the thread reads it
  // with the request count, for the start delay.
  std::atomic<i64> request_us_{0};

  std::atomic<u64> rounds_{0};
  std::atomic<i64> last_begin_us_{0};
  std::atomic<i64> last_end_us_{0};
  // Round costs; written by the thread only.
  std::atomic<i64> longest_round_us_{0};
  std::atomic<i64> round_us_sum_{0};
  std::atomic<i64> longest_start_delay_us_{0};

  std::atomic<u64> fresh_{0};
  std::atomic<u64> recent_{0};
  std::atomic<u64> timed_out_{0};
  std::atomic<u64> skipped_{0};
  std::atomic<u64> stopped_count_{0};

  std::mutex join_mutex_;  // two Stop() calls must not join at once
  std::thread thread_;
};

}  // namespace rex::hid::sdl3
