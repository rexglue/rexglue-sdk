/**
 * @file        tests/unit/hid/sdl3_pad_refresher_test.cpp
 * @brief       Tests of the SDL3 driver's pad refresh thread
 *
 * @copyright   Copyright (c) 2026 Dwreck
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * The round is a fake that records the thread of each round, counts rounds begun and ended, and
 * can hold one chosen round at a gate until the test opens it. The clock is an atomic the test
 * sets. Every CHECK runs on the test's thread: a call that may block runs on a helper thread and
 * the test waits a bounded time for it ("at once" = ready within 1 s).
 */

#include "rex_test.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "hid/sdl3/sdl3_pad_refresher.h"

namespace {

using rex::hid::sdl3::PadRefresher;
using Result = PadRefresher::Result;
using std::chrono::microseconds;
using std::chrono::milliseconds;
using std::chrono::seconds;

constexpr auto kNoAnswer = static_cast<Result>(255);  // a helper call that did not return in time

struct Fake {
  std::atomic<i64> clock{5000};  // not 0: "0 before the first round" must differ from a stamp
  std::atomic<int> begun{0};
  std::atomic<int> ended{0};
  std::atomic<bool> inside{false};
  std::atomic<int> hold_round{0};  // the round (1 = first) that waits at the gate; 0 = none
  std::atomic<bool> gate_open{false};
  std::atomic<bool> gave_up{
      false};  // a held round left by its 30 s limit (the test forgot the gate)
  std::atomic<i64> advance_in_round{0};  // the clock moves by this much inside each round
  std::atomic<int> round_ms{0};          // each round takes this long (real time)
  std::atomic<int> setups{0};
  std::mutex mutex;
  std::vector<std::thread::id> round_threads;
  std::thread::id setup_thread;

  void Refresh() {
    const int n = ++begun;
    {
      const std::lock_guard<std::mutex> lock(mutex);
      round_threads.push_back(std::this_thread::get_id());
    }
    inside = true;
    if (n == hold_round.load()) {
      const auto give_up = std::chrono::steady_clock::now() + seconds(30);
      while (!gate_open.load()) {
        if (std::chrono::steady_clock::now() > give_up) {
          gave_up = true;
          break;
        }
        std::this_thread::sleep_for(milliseconds(1));
      }
    }
    if (round_ms.load() > 0) {
      std::this_thread::sleep_for(milliseconds(round_ms.load()));
    }
    clock.fetch_add(advance_in_round.load());
    inside = false;
    ++ended;
  }

  void Setup() {
    const std::lock_guard<std::mutex> lock(mutex);
    setup_thread = std::this_thread::get_id();
    ++setups;
  }

  PadRefresher::Options MakeOptions() {
    PadRefresher::Options o;
    o.refresh = [this] { Refresh(); };
    o.thread_setup = [this] { Setup(); };
    o.now_us = [this] { return clock.load(); };
    return o;
  }

  // Every round ran on one thread, and that thread is not `caller`.
  bool OneThreadNot(std::thread::id caller) {
    const std::lock_guard<std::mutex> lock(mutex);
    if (round_threads.empty()) {
      return false;
    }
    return std::ranges::all_of(
        round_threads, [&](std::thread::id id) { return id == round_threads[0] && id != caller; });
  }

  bool SetupOnRoundThread(std::thread::id caller) {
    const std::lock_guard<std::mutex> lock(mutex);
    return !round_threads.empty() && setup_thread == round_threads[0] && setup_thread != caller;
  }
};

template <class Pred>
bool WaitUntil(Pred pred, int ms = 5000) {
  const auto end = std::chrono::steady_clock::now() + milliseconds(ms);
  while (!pred()) {
    if (std::chrono::steady_clock::now() > end) {
      return false;
    }
    std::this_thread::sleep_for(milliseconds(1));
  }
  return true;
}

std::future<Result> Call(PadRefresher& r, microseconds limit) {
  return std::async(std::launch::async, [&r, limit] { return r.RefreshAndWait(limit); });
}

template <class T>
bool AtOnce(std::future<T>& f, int ms = 1000) {
  return f.wait_for(milliseconds(ms)) == std::future_status::ready;
}

// The answer of a helper call, or kNoAnswer when it has not returned.
Result Answer(std::future<Result>& f, bool ready) {
  return ready ? f.get() : kNoAnswer;
}

// Ends a test: no round stays held, no waiter stays waiting. Futures declared after the refresher
// are destroyed (and so joined) before it.
void Release(Fake& fake, PadRefresher& r) {
  fake.gate_open = true;
  r.Stop();
}

const microseconds kLong = seconds(10);  // a wait the test ends itself
const microseconds kPlain =
    seconds(2);  // a call made with no round held: a round takes microseconds

}  // namespace

REX_TEST_CASE("pad refresher never runs a round on the caller's thread", "[hid][sdl3]") {
  Fake fake;
  PadRefresher r(fake.MakeOptions());
  CHECK(r.RefreshAndWait(kPlain) == Result::kFresh);
  fake.clock += 2000;
  CHECK(r.RefreshAndWait(kPlain) == Result::kFresh);
  fake.clock += 2000;
  CHECK(r.RefreshAndWait(kPlain) == Result::kFresh);
  CHECK((fake.begun == 3 && fake.ended == 3));
  CHECK(r.GetCounts().rounds == 3);
  CHECK(fake.OneThreadNot(std::this_thread::get_id()));
  Release(fake, r);
}

REX_TEST_CASE("pad refresher runs its thread setup once on its thread", "[hid][sdl3]") {
  Fake fake;
  PadRefresher r(fake.MakeOptions());
  CHECK(r.RefreshAndWait(kPlain) == Result::kFresh);
  fake.clock += 2000;
  CHECK(r.RefreshAndWait(kPlain) == Result::kFresh);
  CHECK(fake.setups == 1);
  CHECK(fake.SetupOnRoundThread(std::this_thread::get_id()));
  Release(fake, r);
}

REX_TEST_CASE("pad refresher gives a burst of reads one round", "[hid][sdl3]") {
  Fake fake;
  PadRefresher r(fake.MakeOptions());
  CHECK(r.RefreshAndWait(kPlain) == Result::kFresh);
  fake.clock += 999;
  CHECK(r.RefreshAndWait(kPlain) == Result::kRecent);
  CHECK(r.RefreshAndWait(kPlain) == Result::kRecent);
  CHECK(r.RefreshAndWait(kPlain) == Result::kRecent);
  // A recent answer asks for no round: none starts in the next 100 ms either.
  std::this_thread::sleep_for(milliseconds(100));
  CHECK((fake.begun == 1 && r.GetCounts().rounds == 1));
  Release(fake, r);
}

// The first call is never "recent": no round has ended, whatever the clock says (here it is below
// recent_us, so "now - 0 < recent_us" would hold).
REX_TEST_CASE("pad refresher's first call is not recent", "[hid][sdl3]") {
  Fake fake;
  fake.clock = 500;
  PadRefresher r(fake.MakeOptions());
  CHECK(r.RefreshAndWait(kPlain) == Result::kFresh);
  CHECK(fake.begun == 1);
  Release(fake, r);
}

// The clock moves 7 inside each round, so a round's begin and end stamps differ and "recent" is
// seen to count from the END of the round (from its begin, 6006 would already be 1006 later).
REX_TEST_CASE("pad refresher counts recent from the end of the last round", "[hid][sdl3]") {
  Fake fake;
  fake.advance_in_round = 7;
  PadRefresher r(fake.MakeOptions());
  CHECK((r.GetLastRoundBeginUs() == 0 && r.GetLastRoundEndUs() == 0));
  CHECK(r.RefreshAndWait(kPlain) == Result::kFresh);
  CHECK((r.GetLastRoundBeginUs() == 5000 && r.GetLastRoundEndUs() == 5007));
  fake.clock = 6006;  // 999 after the end
  CHECK(r.RefreshAndWait(kPlain) == Result::kRecent);
  fake.clock = 6007;  // exactly 1000 after the end: not recent
  CHECK(r.RefreshAndWait(kPlain) == Result::kFresh);
  CHECK(r.GetCounts().rounds == 2);
  CHECK((r.GetLastRoundBeginUs() == 6007 && r.GetLastRoundEndUs() == 6014));
  Release(fake, r);
}

// Y asks while round 1 runs: round 1 may hold events from before Y's request only, so Y must get
// round 2. Y's request has been made when the gate opens (the after_request hook counts requests:
// X's is the first, Y's the second). Y's own thread counts the rounds ended at the moment its call
// returns, and every round takes 100 ms from the gate on, so a Y that was answered by round 1 is
// seen with one round ended.
REX_TEST_CASE("pad refresher answers a request with a round that starts after it", "[hid][sdl3]") {
  Fake fake;
  fake.hold_round = 1;
  std::atomic<int> requests{0};
  PadRefresher::Options o = fake.MakeOptions();
  o.after_request = [&] { ++requests; };
  PadRefresher r(std::move(o));
  std::future<Result> x = Call(r, kLong);
  CHECK(WaitUntil([&] { return fake.inside.load() && fake.begun == 1; }));
  fake.clock += 2000;
  std::atomic<int> ended_when_y_returned{-1};
  std::future<Result> y = std::async(std::launch::async, [&] {
    const Result result = r.RefreshAndWait(kLong);
    ended_when_y_returned = fake.ended.load();
    return result;
  });
  CHECK(WaitUntil([&] { return requests.load() == 2; }));
  CHECK((fake.inside.load() && fake.ended == 0));  // round 1 is still held with both waiting
  fake.round_ms = 100;
  fake.gate_open = true;
  const bool y_ready = AtOnce(y, 5000);
  const u64 rounds_after_y = r.GetCounts().rounds;
  CHECK(Answer(y, y_ready) == Result::kFresh);
  CHECK((ended_when_y_returned == 2 && rounds_after_y == 2));
  const bool x_ready = AtOnce(x, 5000);
  CHECK(Answer(x, x_ready) == Result::kFresh);
  Release(fake, r);
}

// A round that does not come back. One caller waits its limit once; nobody waits again until that
// round has ended; then waits resume.
REX_TEST_CASE("pad refresher waits for a stuck round once", "[hid][sdl3]") {
  Fake fake;
  fake.hold_round = 1;
  PadRefresher r(fake.MakeOptions());

  // The wait is bounded.
  std::future<std::pair<Result, i64>> timed = std::async(std::launch::async, [&] {
    const auto t0 = std::chrono::steady_clock::now();
    const Result res = r.RefreshAndWait(milliseconds(50));
    const i64 took_us =
        std::chrono::duration_cast<microseconds>(std::chrono::steady_clock::now() - t0).count();
    return std::pair<Result, i64>{res, took_us};
  });
  const bool timed_ready = AtOnce(timed);
  const std::pair<Result, i64> timed_answer =
      timed_ready ? timed.get() : std::pair<Result, i64>{kNoAnswer, 0};
  CHECK(timed_ready);
  CHECK(timed_answer.first == Result::kTimedOut);
  CHECK(timed_answer.second >= 50000);
  CHECK(timed_answer.second < 300000);  // the limit from above: 50 ms, not a multiple of it
  CHECK((fake.inside.load() && fake.begun == 1 && fake.ended == 0));

  // No second wait while the round is out.
  fake.clock += 2000;
  std::future<Result> second = Call(r, kLong);
  const bool second_ready = AtOnce(second);
  CHECK(second_ready);
  CHECK(Answer(second, second_ready) == Result::kSkipped);
  CHECK((fake.begun == 1 && fake.ended == 0));

  // The stuck round ends (it was slow: waits stay off), then the round the skipped call asked for
  // runs and is quick: waits are back on.
  fake.gate_open = true;
  CHECK(WaitUntil([&] { return r.GetCounts().rounds >= 2; }));
  CHECK(fake.begun == 2);
  fake.clock += 2000;
  std::future<Result> third = Call(r, kLong);
  const bool third_ready = AtOnce(third);
  CHECK(Answer(third, third_ready) == Result::kFresh);
  CHECK((fake.ended == 3 && r.GetCounts().rounds == 3));
  Release(fake, r);
}

// Two callers wait on one stuck round: when the first gives up, the other leaves too, so the round
// costs one limit, not one limit a caller.
REX_TEST_CASE("pad refresher's waiters leave when one times out", "[hid][sdl3]") {
  Fake fake;
  fake.hold_round = 1;
  PadRefresher r(fake.MakeOptions());
  std::future<Result> patient = Call(r, kLong);
  CHECK(WaitUntil([&] { return fake.inside.load(); }));
  std::future<Result> brief = Call(r, milliseconds(50));
  const bool brief_ready = AtOnce(brief);
  CHECK(Answer(brief, brief_ready) == Result::kTimedOut);
  const bool patient_ready = AtOnce(patient);
  const bool held = fake.inside.load() && fake.ended == 0;
  CHECK(patient_ready);
  CHECK(Answer(patient, patient_ready) == Result::kSkipped);
  CHECK(held);
  Release(fake, r);
}

// A caller that reaches its deadline looks at its round once more before it times out. The
// at_deadline hook stands for a reader that lost its core between its last look and its clock
// read: the held round is let go there, and the hook returns once the round's end is published
// (the thread's next look sees the round ended, and it comes after done_ is stored). Without that
// look the call times out on a round that has ended, and the next call is skipped.
REX_TEST_CASE("pad refresher answers fresh for a round that ends at the deadline", "[hid][sdl3]") {
  Fake fake;
  fake.hold_round = 1;
  std::atomic<bool> published{false};
  std::atomic<bool> hook_saw_it{false};
  std::atomic<int> deadlines{0};
  PadRefresher::Options o = fake.MakeOptions();
  o.after_noted = [&] {
    if (fake.ended.load() >= 1) {
      published = true;
    }
  };
  o.at_deadline = [&] {
    if (deadlines.fetch_add(1) != 0) {
      return;
    }
    fake.gate_open = true;
    hook_saw_it = WaitUntil([&] { return published.load(); });
  };
  PadRefresher r(std::move(o));
  const Result at_deadline = r.RefreshAndWait(milliseconds(50));
  fake.clock += 2000;
  const Result next = r.RefreshAndWait(kPlain);
  CHECK((hook_saw_it && deadlines == 1));
  CHECK(at_deadline == Result::kFresh);  // NOT kTimedOut
  CHECK(r.GetCounts().timed_out == 0);
  CHECK(r.GetCounts().fresh == 2);  // the second look counts its answer too
  CHECK(next == Result::kFresh);    // NOT kSkipped: rounds were not marked slow
  Release(fake, r);
}

// Rounds that are slow every time (100 ms each, limit 40 ms): one call pays the limit, the calls
// after it do not wait but still ask for a round, so the state keeps being refreshed; when rounds
// are quick again the waits come back.
REX_TEST_CASE("pad refresher's slow rounds cost one limit", "[hid][sdl3]") {
  Fake fake;
  fake.round_ms = 100;
  PadRefresher r(fake.MakeOptions());
  const Result first = r.RefreshAndWait(milliseconds(40));
  bool rounds_kept_coming = true;
  int skipped = 0;
  for (int i = 1; i <= 8; ++i) {
    // Each call below is made after the round the call before it asked for has ended.
    rounds_kept_coming = WaitUntil([&] { return r.GetCounts().rounds >= static_cast<u64>(i); }) &&
                         rounds_kept_coming;
    fake.clock += 2000;
    if (r.RefreshAndWait(milliseconds(40)) == Result::kSkipped) {
      ++skipped;
    }
  }
  CHECK(first == Result::kTimedOut);
  CHECK(rounds_kept_coming);
  CHECK(skipped == 8);
  CHECK(r.GetCounts().timed_out == 1);
  // Quick rounds again: the next call still does not wait, its round is quick, the one after waits.
  const bool ninth = WaitUntil([&] { return r.GetCounts().rounds >= 9; });
  fake.round_ms = 0;
  fake.clock += 2000;
  const Result still_off = r.RefreshAndWait(milliseconds(40));
  const bool tenth = WaitUntil([&] { return r.GetCounts().rounds >= 10; });
  fake.clock += 2000;
  const Result back_on = r.RefreshAndWait(kPlain);
  CHECK((ninth && tenth));
  CHECK(still_off == Result::kSkipped);
  CHECK(back_on == Result::kFresh);
  CHECK(r.GetCounts().rounds == 11);
  Release(fake, r);
}

// The factor one half, from both sides. Limit 200 ms. A round that ends 150 ms after the skipped
// call's request (more than half the limit, less than the limit): the next call still does not
// wait. A round that ends about 60 ms after it (less than half; a coarse timer adds up to 16 ms):
// the next call waits again. A factor of 1 or more (the limit itself, twice it) fails the first
// leg, one of 1/4 or less the second; factors in between are not told apart here.
REX_TEST_CASE("pad refresher's waits come back after a round quicker than half the limit",
              "[hid][sdl3]") {
  Fake fake;
  fake.round_ms = 250;
  PadRefresher r(fake.MakeOptions());
  const Result over = r.RefreshAndWait(milliseconds(200));  // times out: rounds are slow
  const bool one = WaitUntil([&] { return r.GetCounts().rounds >= 1; });
  fake.round_ms = 150;
  fake.clock += 2000;
  const Result asks_150 = r.RefreshAndWait(milliseconds(200));  // skipped; asks for a 150 ms round
  const bool two = WaitUntil([&] { return r.GetCounts().rounds >= 2; });
  fake.round_ms = 60;
  fake.clock += 2000;
  const Result after_150 = r.RefreshAndWait(milliseconds(200));  // asks for a 60 ms round
  const bool three = WaitUntil([&] { return r.GetCounts().rounds >= 3; });
  fake.clock += 2000;
  const Result after_60 = r.RefreshAndWait(milliseconds(200));
  CHECK((over == Result::kTimedOut && asks_150 == Result::kSkipped));
  CHECK((one && two && three));
  CHECK(after_150 == Result::kSkipped);  // NOT kFresh (a factor of 1 or more)
  CHECK(after_60 == Result::kFresh);     // NOT kSkipped (a factor of 1/4 or less)
  CHECK(r.GetCounts().timed_out == 1);
  Release(fake, r);
}

// Rounds that START late: the refresher's thread is held 5 ms after each look at the request
// count, so no round can end within a read's 2 ms limit, though the round itself takes nothing.
// Judged by the round's own duration such a round "was quick", so the next frame's first read
// would wait the limit again. Judged by the time from a skipped call's request to the round's end,
// one read pays the limit once. Then the delay goes and the waits come back. Real clock: frames
// are real time apart.
REX_TEST_CASE("pad refresher's late started rounds cost one limit", "[hid][sdl3]") {
  Fake fake;
  std::atomic<bool> late{true};
  PadRefresher::Options o = fake.MakeOptions();
  o.now_us = nullptr;
  o.after_noted = [&] {
    if (late.load()) {
      std::this_thread::sleep_for(milliseconds(5));
    }
  };
  PadRefresher r(std::move(o));
  int fresh_late = 0;
  for (int frame = 0; frame < 30; ++frame) {
    for (int read = 0; read < 4; ++read) {
      if (r.RefreshAndWait(milliseconds(2)) == Result::kFresh) {
        ++fresh_late;
      }
    }
    std::this_thread::sleep_for(milliseconds(14));
  }
  const PadRefresher::Counts during = r.GetCounts();
  late = false;
  int fresh_after = 0;
  for (int frame = 0; frame < 15; ++frame) {
    for (int read = 0; read < 4; ++read) {
      if (r.RefreshAndWait(milliseconds(2)) == Result::kFresh) {
        ++fresh_after;
      }
    }
    std::this_thread::sleep_for(milliseconds(14));
  }
  const PadRefresher::Counts after = r.GetCounts();
  INFO("late: timed out " << during.timed_out << ", skipped " << during.skipped << ", fresh "
                          << fresh_late << "; after: timed out " << after.timed_out << ", fresh "
                          << fresh_after);
  CHECK(during.timed_out == 1);  // NOT one a frame or so
  CHECK(fresh_late == 0);
  CHECK(during.skipped >= 29);  // every later frame still asked for a round
  CHECK(after.timed_out == 1);
  CHECK(fresh_after >= 10);  // waits came back once rounds started on time
  Release(fake, r);
}

// Stop while a round is in flight and a caller waits.
REX_TEST_CASE("pad refresher's Stop releases a waiter", "[hid][sdl3]") {
  Fake fake;
  fake.hold_round = 1;
  PadRefresher r(fake.MakeOptions());
  std::future<Result> a = Call(r, kLong);
  CHECK(WaitUntil([&] { return fake.inside.load(); }));
  std::future<void> b = std::async(std::launch::async, [&] { r.Stop(); });
  const bool a_ready = AtOnce(a);
  const bool held_when_a_returned = fake.inside.load() && fake.ended == 0;
  CHECK(a_ready);
  CHECK(Answer(a, a_ready) == Result::kStopped);
  CHECK(held_when_a_returned);
  fake.gate_open = true;
  CHECK(AtOnce(b));
  CHECK(!fake.gave_up);
  Release(fake, r);
}

// Stop() waits for the round in flight: it has not returned while the round is held, and when it
// returns the round is not running and does not run again.
REX_TEST_CASE("pad refresher's Stop joins the round in flight", "[hid][sdl3]") {
  Fake fake;
  fake.hold_round = 1;
  PadRefresher r(fake.MakeOptions());
  [[maybe_unused]] const std::future<Result> a = Call(r, kLong);  // waits until Stop()
  CHECK(WaitUntil([&] { return fake.inside.load(); }));
  std::atomic<bool> stop_entered{false};
  std::future<bool> b = std::async(std::launch::async, [&] {
    stop_entered = true;
    r.Stop();
    return fake.inside.load();
  });
  CHECK(WaitUntil([&] { return stop_entered.load(); }));
  CHECK(!AtOnce(b, 200));  // Stop() is waiting for the held round
  fake.gate_open = true;
  const bool b_ready = AtOnce(b);
  const bool inside_when_stop_returned = b_ready ? b.get() : true;
  CHECK(b_ready);
  CHECK(!inside_when_stop_returned);
  const int begun = fake.begun.load();
  const int ended = fake.ended.load();
  std::this_thread::sleep_for(milliseconds(50));
  CHECK((begun == 1 && ended == 1 && fake.begun == 1 && fake.ended == 1 && !fake.inside));
  Release(fake, r);
}

// Stop() while the refresher's thread is between its look ("nothing to do") and its sleep: Stop()
// has not returned while the thread is held there, and returns once it is let go (the thread sees
// the stop instead of sleeping through it).
REX_TEST_CASE("pad refresher's Stop between a look and the sleep returns", "[hid][sdl3]") {
  Fake fake;
  std::atomic<int> looks{0};
  std::atomic<bool> at_hook{false};
  std::atomic<bool> go{false};
  std::atomic<bool> hook_gave_up{false};
  PadRefresher::Options o = fake.MakeOptions();
  o.after_noted = [&] {
    if (looks.fetch_add(1) != 0) {
      return;
    }
    at_hook = true;
    if (!WaitUntil([&] { return go.load(); }, 30000)) {
      hook_gave_up = true;
    }
  };
  PadRefresher r(std::move(o));
  CHECK(WaitUntil([&] { return at_hook.load(); }));
  std::atomic<bool> stop_entered{false};
  std::future<void> s = std::async(std::launch::async, [&] {
    stop_entered = true;
    r.Stop();
  });
  CHECK(WaitUntil([&] { return stop_entered.load(); }));
  CHECK(!AtOnce(s, 200));  // the join waits for the held thread
  go = true;
  CHECK(AtOnce(s));
  CHECK((!hook_gave_up && fake.begun == 0));
  Release(fake, r);
}

// Refreshers destroyed at once (the thread may not have reached its first look) and after one
// call: every destructor returns, every call is fresh.
REX_TEST_CASE("pad refresher can be destroyed at once and after one call", "[hid][sdl3]") {
  std::future<int> whole = std::async(std::launch::async, [] {
    int not_fresh = 0;
    for (int i = 0; i < 1000; ++i) {
      PadRefresher::Options o;
      o.refresh = [] {};
      const PadRefresher r(std::move(o));
    }
    for (int i = 0; i < 1000; ++i) {
      PadRefresher::Options o;
      o.refresh = [] {};
      PadRefresher r(std::move(o));
      if (r.RefreshAndWait(seconds(2)) != Result::kFresh) {
        ++not_fresh;
      }
    }
    return not_fresh;
  });
  const bool ready = AtOnce(whole, 30000);
  const int not_fresh = ready ? whole.get() : -1;
  CHECK(ready);
  CHECK(not_fresh == 0);
}

// The clock does not move after the round, so a call after Stop() would be "recent" if the stopped
// rule did not come first.
REX_TEST_CASE("pad refresher returns at once after Stop", "[hid][sdl3]") {
  Fake fake;
  PadRefresher r(fake.MakeOptions());
  CHECK(r.RefreshAndWait(kPlain) == Result::kFresh);
  CHECK(r.IsRunning());
  r.Stop();
  CHECK(!r.IsRunning());
  std::future<Result> late = Call(r, kLong);
  const bool late_ready = AtOnce(late);
  CHECK(late_ready);
  CHECK(Answer(late, late_ready) == Result::kStopped);
  fake.clock += 2000;
  std::future<Result> later = Call(r, kLong);
  const bool later_ready = AtOnce(later);
  CHECK(Answer(later, later_ready) == Result::kStopped);
  CHECK((fake.begun == 1 && r.GetCounts().rounds == 1));
  std::future<void> again = std::async(std::launch::async, [&] { r.Stop(); });
  CHECK(AtOnce(again));
  Release(fake, r);
}

REX_TEST_CASE("pad refresher without a thread returns at once", "[hid][sdl3]") {
  Fake fake;
  std::future<unsigned> whole = std::async(std::launch::async, [&] {
    PadRefresher::Options o = fake.MakeOptions();
    o.start_thread = false;
    PadRefresher r(std::move(o));
    unsigned bits = 0;
    if (!r.IsRunning()) {
      bits |= 1u;
    }
    if (r.RefreshAndWait(kLong) == Result::kStopped) {
      bits |= 2u;
    }
    if (r.GetCounts().rounds == 0 && r.GetCounts().stopped == 1) {
      bits |= 4u;
    }
    return bits;  // the destructor runs here
  });
  const bool ready = AtOnce(whole);
  const unsigned bits = ready ? whole.get() : 0u;
  CHECK(ready);
  CHECK((bits & 1u) != 0u);
  CHECK((bits & 2u) != 0u);
  CHECK((bits & 4u) != 0u);
  CHECK((fake.begun == 0 && fake.setups == 0));
}

// The driver gives no clock: the default one stamps rounds (recent_us is a minute here, so the
// second call is recent whatever the machine's load).
REX_TEST_CASE("pad refresher's default clock stamps rounds", "[hid][sdl3]") {
  Fake fake;
  PadRefresher::Options o = fake.MakeOptions();
  o.now_us = nullptr;
  o.recent_us = 60'000'000;
  PadRefresher r(std::move(o));
  CHECK(r.RefreshAndWait(kPlain) == Result::kFresh);
  CHECK((r.GetLastRoundBeginUs() > 0 && r.GetLastRoundEndUs() >= r.GetLastRoundBeginUs()));
  CHECK(r.RefreshAndWait(kPlain) == Result::kRecent);
  Release(fake, r);
}

REX_TEST_CASE("pad refresher's counts match its answers", "[hid][sdl3]") {
  Fake fake;
  PadRefresher r(fake.MakeOptions());
  std::array<Result, 8> seq{};
  seq[0] = r.RefreshAndWait(kPlain);  // fresh
  seq[1] = r.RefreshAndWait(kPlain);  // recent (the clock has not moved)
  seq[2] = r.RefreshAndWait(kPlain);  // recent
  seq[3] = r.RefreshAndWait(kPlain);  // recent
  fake.clock += 2000;
  seq[4] = r.RefreshAndWait(kPlain);  // fresh
  fake.clock += 2000;
  fake.hold_round = 3;
  std::future<Result> t = Call(r, milliseconds(50));
  const bool t_ready = AtOnce(t);
  seq[5] = Answer(t, t_ready);  // timed out, round 3 held
  std::future<Result> s1 = Call(r, kLong);
  const bool s1_ready = AtOnce(s1);
  seq[6] = Answer(s1, s1_ready);  // skipped
  std::future<Result> s2 = Call(r, kLong);
  const bool s2_ready = AtOnce(s2);
  seq[7] = Answer(s2, s2_ready);  // skipped
  fake.gate_open = true;
  // Round 3 was the held one; round 4 is the one the skipped calls asked for.
  const bool round4_ended = WaitUntil([&] { return r.GetCounts().rounds >= 4; });
  r.Stop();
  const Result last = r.RefreshAndWait(kPlain);  // stopped
  CHECK((seq[0] == Result::kFresh && seq[4] == Result::kFresh));
  CHECK((seq[1] == Result::kRecent && seq[2] == Result::kRecent && seq[3] == Result::kRecent));
  CHECK(seq[5] == Result::kTimedOut);
  CHECK((seq[6] == Result::kSkipped && seq[7] == Result::kSkipped));
  CHECK(round4_ended);
  CHECK(last == Result::kStopped);
  const PadRefresher::Counts c = r.GetCounts();
  CHECK(c.rounds == 4);
  CHECK(c.fresh == 2);
  CHECK(c.recent == 3);
  CHECK(c.timed_out == 1);
  CHECK(c.skipped == 2);
  CHECK(c.stopped == 1);
  Release(fake, r);
}

// What rounds cost, one check or more for each statement that writes a cost: each starts at 0
// (looked at while the thread is held before its first round); the longest round is the longest,
// not the last (100 ms, then 30 ms); the sum adds every round (130 ms or more, which the longest
// alone is not); the start delay runs from the request to the round's start, not to its end (the
// thread is held 25 ms after round 1's request, and round 1 takes 100 ms, so to its end would be
// 125 ms or more), and is the longest, not the last (round 2 starts at once); a request time that
// was never stored would make it the clock's whole count since the machine started.
REX_TEST_CASE("pad refresher counts what rounds cost", "[hid][sdl3]") {
  Fake fake;
  std::atomic<int> looks{0};
  std::atomic<bool> at_hook{false};
  std::atomic<bool> requested{false};
  std::atomic<bool> hook_gave_up{false};
  PadRefresher::Options o = fake.MakeOptions();
  o.after_noted = [&] {
    if (looks.fetch_add(1) != 0) {
      return;  // the first look only (it found nothing to do)
    }
    at_hook = true;
    if (!WaitUntil([&] { return requested.load(); }, 30000)) {
      hook_gave_up = true;
    }
    std::this_thread::sleep_for(milliseconds(25));
  };
  o.after_request = [&] { requested = true; };
  PadRefresher r(std::move(o));
  const bool held = WaitUntil([&] { return at_hook.load(); });
  const PadRefresher::Counts before = r.GetCounts();
  const i64 longest_before = r.GetLongestRoundUs();
  fake.round_ms = 100;
  const Result first = r.RefreshAndWait(kPlain);
  fake.round_ms = 30;
  fake.clock += 2000;
  const Result second = r.RefreshAndWait(kPlain);
  const PadRefresher::Counts c = r.GetCounts();
  INFO("longest round " << c.longest_round_us << " us, sum " << c.round_us_sum
                        << " us, longest start delay " << c.longest_start_delay_us << " us");
  CHECK((held && first == Result::kFresh && second == Result::kFresh && c.rounds == 2));
  CHECK((before.longest_round_us == 0 && before.round_us_sum == 0 &&
         before.longest_start_delay_us == 0 && longest_before == 0));
  CHECK(c.longest_round_us >= 100000);  // NOT the last round's (30 ms)
  CHECK(r.GetLongestRoundUs() == c.longest_round_us);
  CHECK(c.round_us_sum >= 130000);           // NOT the longest alone
  CHECK(c.longest_start_delay_us >= 25000);  // NOT the last round's (it started at once)
  CHECK(c.longest_start_delay_us < 100000);  // NOT to the round's end
  CHECK(!hook_gave_up);
  Release(fake, r);
}

// A waiting caller keeps its core while the spin lasts: a yield on a busy machine can cost a whole
// scheduler slice. The spin is 2 s here and the round 20 ms, so the call never reaches a yield
// whatever the machine's load.
REX_TEST_CASE("pad refresher's spin keeps the core", "[hid][sdl3]") {
  Fake fake;
  fake.round_ms = 20;
  std::atomic<int> yields{0};
  PadRefresher::Options o = fake.MakeOptions();
  o.spin_us = 2'000'000;
  o.yield = [&] {
    ++yields;
    std::this_thread::yield();
  };
  PadRefresher r(std::move(o));
  CHECK(r.RefreshAndWait(kLong) == Result::kFresh);
  CHECK(yields == 0);
  CHECK(fake.ended == 1);
  Release(fake, r);
}

// After the spin the caller gives the core away each turn (a long round must not burn a core for
// the whole limit), and not before: the first yield comes no earlier than the spin's length.
REX_TEST_CASE("pad refresher yields after the spin", "[hid][sdl3]") {
  Fake fake;
  fake.round_ms = 100;
  std::atomic<int> yields{0};
  std::atomic<i64> first_yield_us{-1};
  std::chrono::steady_clock::time_point start;
  PadRefresher::Options o = fake.MakeOptions();
  o.spin_us = 5000;
  o.yield = [&] {
    if (yields.fetch_add(1) == 0) {
      first_yield_us =
          std::chrono::duration_cast<microseconds>(std::chrono::steady_clock::now() - start)
              .count();
    }
    std::this_thread::yield();
  };
  PadRefresher r(std::move(o));
  start = std::chrono::steady_clock::now();
  CHECK(r.RefreshAndWait(kLong) == Result::kFresh);
  CHECK(yields > 0);
  CHECK(first_yield_us >= 5000);
  Release(fake, r);
}

// A spin of 0 is the plain yield loop: the first turn that finds the round unfinished yields.
// Twenty calls, each with a 5 ms round; the earliest first yield of the twenty is taken, so a busy
// machine must delay every one of them to fail this, while a spin of any length (0 read as a
// default, say) keeps every first yield at or above that length.
REX_TEST_CASE("pad refresher without a spin yields at once", "[hid][sdl3]") {
  Fake fake;
  fake.round_ms = 5;
  int yields = 0;  // the hook runs on the calling thread, which is this one
  i64 first_yield_us = -1;
  std::chrono::steady_clock::time_point start;
  PadRefresher::Options o = fake.MakeOptions();
  o.spin_us = 0;
  o.yield = [&] {
    ++yields;
    if (first_yield_us < 0) {
      first_yield_us =
          std::chrono::duration_cast<microseconds>(std::chrono::steady_clock::now() - start)
              .count();
    }
    std::this_thread::yield();
  };
  PadRefresher r(std::move(o));
  int fresh = 0;
  i64 earliest = -1;
  for (int i = 0; i < 20; ++i) {
    fake.clock += 2000;
    first_yield_us = -1;
    start = std::chrono::steady_clock::now();
    if (r.RefreshAndWait(kLong) == Result::kFresh) {
      ++fresh;
    }
    if (first_yield_us >= 0 && (earliest < 0 || first_yield_us < earliest)) {
      earliest = first_yield_us;
    }
  }
  INFO("earliest first yield " << earliest << " us, yields " << yields);
  CHECK(fresh == 20);
  CHECK(yields > 0);
  CHECK((earliest >= 0 && earliest < 150));
  Release(fake, r);
}

// The spin is a spin: nothing in it gives the core away or sleeps. The yield hook cannot see a
// sleep, so this one measures: 51 calls, the refresher's thread asleep before each (1 ms pause),
// the spin 2 s. A call then lasts as long as the thread's wake and a round (tens of microseconds);
// a sleep in the spin makes every call that finds its round unfinished at the first look last the
// sleep's length. The median is judged, so a busy machine must delay half the calls to fail this.
// Not seen here: a bare yield in the spin on a quiet machine (it returns at once).
REX_TEST_CASE("pad refresher's spin does not sleep", "[hid][sdl3]") {
  Fake fake;
  std::atomic<int> yields{0};
  PadRefresher::Options o = fake.MakeOptions();
  o.recent_us = 0;
  o.spin_us = 2'000'000;
  o.yield = [&] {
    ++yields;
    std::this_thread::yield();
  };
  PadRefresher r(std::move(o));
  std::vector<i64> us;
  int fresh = 0;
  for (int i = 0; i < 51; ++i) {
    std::this_thread::sleep_for(milliseconds(1));
    const auto t0 = std::chrono::steady_clock::now();
    if (r.RefreshAndWait(kPlain) == Result::kFresh) {
      ++fresh;
    }
    us.push_back(
        std::chrono::duration_cast<microseconds>(std::chrono::steady_clock::now() - t0).count());
  }
  std::ranges::sort(us);
  INFO("call us min " << us[0] << ", median " << us[25] << ", max " << us[50]);
  CHECK(fresh == 51);
  CHECK(yields == 0);
  CHECK(us[25] < 500);
  Release(fake, r);
}

// Whether rounds are slow is read BEFORE a call makes its request. Rounds are slow (one call ran
// out); then rounds are quick, and the call's own round has ended, and cleared "slow", before the
// call goes on (the after_request hook waits for round 2). The call must not wait for a round
// because that round was quick: it answers kSkipped. Read after the request, "slow" is already
// clear and the answer is kFresh.
REX_TEST_CASE("pad refresher reads whether rounds are slow before the request", "[hid][sdl3]") {
  Fake fake;
  fake.round_ms = 100;
  std::atomic<bool> armed{false};
  std::atomic<bool> saw_round_2{false};
  std::atomic<PadRefresher*> rp{nullptr};
  PadRefresher::Options o = fake.MakeOptions();
  // Round 2 is seen by its end stamp (7000 on the test's clock; round 1's is 5000), which the
  // thread stores after it cleared "slow". Not by GetCounts(): the hook must use nothing a caller
  // could be holding.
  o.after_request = [&] {
    if (!armed.load()) {
      return;
    }
    saw_round_2 = WaitUntil([&] { return rp.load()->GetLastRoundEndUs() == 7000; });
  };
  PadRefresher r(std::move(o));
  rp = &r;
  CHECK(r.RefreshAndWait(milliseconds(40)) == Result::kTimedOut);
  CHECK(WaitUntil([&] { return r.GetCounts().rounds >= 1; }));
  fake.round_ms = 0;
  fake.clock += 2000;
  armed = true;
  const Result result = r.RefreshAndWait(kPlain);
  armed = false;
  CHECK(saw_round_2);
  CHECK(result == Result::kSkipped);
  Release(fake, r);
}

// A request made while the refresher's thread is between "nothing to do" and going to sleep is not
// slept through. The after_noted hook holds the thread right after its first read of the request
// count (0: nothing to do) until the caller's request has been made. The thread read its wake word
// before that, so its sleep returns at once and the round runs; had it read the wake word after
// the request count, it would sleep on the new value and the caller would pay its whole limit.
REX_TEST_CASE("pad refresher sees a request made just before its sleep", "[hid][sdl3]") {
  Fake fake;
  std::atomic<int> noted_calls{0};
  std::atomic<bool> at_hook{false};
  std::atomic<bool> requested{false};
  std::atomic<bool> hook_gave_up{false};
  PadRefresher::Options o = fake.MakeOptions();
  o.after_noted = [&] {
    if (noted_calls.fetch_add(1) != 0) {
      return;  // the first look only
    }
    at_hook = true;
    const auto give_up = std::chrono::steady_clock::now() + seconds(30);
    while (!requested.load()) {
      if (std::chrono::steady_clock::now() > give_up) {
        hook_gave_up = true;
        break;
      }
      std::this_thread::sleep_for(milliseconds(1));
    }
  };
  o.after_request = [&] { requested = true; };
  PadRefresher r(std::move(o));
  CHECK(WaitUntil([&] { return at_hook.load(); }));
  CHECK(fake.begun == 0);
  CHECK(r.RefreshAndWait(kPlain) == Result::kFresh);
  CHECK((!hook_gave_up && fake.ended == 1));
  Release(fake, r);
}

// A caller that stops for good right after it made its request (a thread killed wherever it is)
// holds nothing the others need: GetCounts() answers, the round it asked for runs and Stop()
// returns. A guard, not a proof: only the one place where `after_request` runs is covered; the
// mutant that puts a lock around the request shows the test can fail.
REX_TEST_CASE("pad refresher's stuck caller blocks nobody", "[hid][sdl3]") {
  Fake fake;
  std::atomic<bool> entered{false};
  std::atomic<bool> release{false};
  PadRefresher::Options o = fake.MakeOptions();
  o.after_request = [&] {
    entered = true;
    const auto give_up = std::chrono::steady_clock::now() + seconds(30);
    while (!release.load() && std::chrono::steady_clock::now() < give_up) {
      std::this_thread::sleep_for(milliseconds(1));
    }
  };
  PadRefresher r(std::move(o));
  std::future<Result> a = Call(r, kLong);
  const bool a_entered = WaitUntil([&] { return entered.load(); });
  CHECK(a_entered);
  std::future<u64> c = std::async(std::launch::async, [&] { return r.GetCounts().fresh; });
  const bool c_ready = AtOnce(c);
  CHECK(c_ready);
  CHECK(WaitUntil([&] { return fake.ended.load() >= 1; }));
  std::future<void> s = std::async(std::launch::async, [&] { r.Stop(); });
  const bool s_ready = AtOnce(s);
  CHECK(s_ready);
  const bool stuck_until_now = a.wait_for(milliseconds(0)) != std::future_status::ready;
  CHECK(stuck_until_now);
  release = true;
  const bool a_ready = AtOnce(a);
  CHECK(a_ready);
  CHECK(Answer(a, a_ready) == Result::kFresh);  // its round had ended: done comes before stopped
  Release(fake, r);
}

// The closing line's round cost and its warning: a pure function of the counts, so the driver's
// one comparison is pinned here. The limits are written as literals (1000 rounds, 500 us), not
// taken from the constants under test. In each fixture with rounds the longest round differs from
// the mean and the sum, so a function that judged it gives another answer.
REX_TEST_CASE("pad refresher's round cost summary", "[hid][sdl3]") {
  auto of = [](u64 rounds, i64 sum_us, i64 longest_us) {
    PadRefresher::Counts c{};
    c.rounds = rounds;
    c.round_us_sum = sum_us;
    c.longest_round_us = longest_us;
    return PadRefresher::RoundCostOf(c);
  };
  // 1000 rounds with a mean of 501 us: the first session that warns.
  const PadRefresher::RoundCost at_both = of(1000, 501000, 7);
  CHECK((at_both.mean_us == 501 && at_both.warn));
  // 999 rounds with the same mean (999 * 501 = 500499): one round short, no warning.
  const PadRefresher::RoundCost short_session = of(999, 500499, 7);
  CHECK((short_session.mean_us == 501 && !short_session.warn));
  // 1000 rounds with a mean of exactly 500 us: at the limit, NOT over it.
  const PadRefresher::RoundCost at_limit = of(1000, 500000, 7);
  CHECK((at_limit.mean_us == 500 && !at_limit.warn));
  // The mean rounds down: 500.999 us is 500, no warning.
  const PadRefresher::RoundCost rounds_down = of(1000, 500999, 7);
  CHECK((rounds_down.mean_us == 500 && !rounds_down.warn));
  // One very long round in a quick session (the shape of a real run with pads being added: 4463
  // rounds, 194 us mean, longest 207116 us): reported by the caller, never warned about.
  const PadRefresher::RoundCost one_long = of(4463, 865822, 207116);
  CHECK((one_long.mean_us == 194 && !one_long.warn));
  // No round at all: no division, mean 0, no warning.
  const PadRefresher::RoundCost none = of(0, 0, 0);
  CHECK((none.mean_us == 0 && !none.warn));
  // A long session of slow rounds: the sum is past 32 bits (3,000,000 rounds of 4000 us).
  const PadRefresher::RoundCost long_session = of(3000000, 12000000000LL, 9000);
  CHECK((long_session.mean_us == 4000 && long_session.warn));
}
