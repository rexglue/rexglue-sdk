/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <condition_variable>
#include <forward_list>
#include <iterator>
#include <memory>
#include <mutex>
#include <vector>

#include <rex/assert.h>
#include <rex/platform/thread.h>
#include <rex/platform/timer_queue.h>

#include "core/timer_queue_state.h"

namespace rex::platform {

using WaitItem = TimerQueueWaitItem;

class TimerQueue {
 public:
  using clock = WaitItem::clock;
  static_assert(clock::is_steady);

  TimerQueue() {
    dispatch_thread_ =
        Spawn("rex::platform::TimerQueue",
              [this](const std::stop_token& stop_token) { TimerThreadMain(stop_token); });
  }

  void TimerThreadMain(const std::stop_token& stop_token) {
    const auto comp = [](const std::shared_ptr<WaitItem>& left,
                         const std::shared_ptr<WaitItem>& right) {
      return left->due_ < right->due_;
    };

    while (!stop_token.stop_requested()) {
      std::vector<std::shared_ptr<WaitItem>> arrived;
      {
        std::unique_lock lock(mutex_);
        const auto has_pending = [this] { return !pending_.empty(); };
        if (wait_queue_.empty()) {
          wake_.wait(lock, stop_token, has_pending);
        } else {
          wake_.wait_until(lock, stop_token, wait_queue_.front()->due_, has_pending);
        }
        arrived.swap(pending_);
      }

      if (!arrived.empty()) {
        std::forward_list<std::shared_ptr<WaitItem>> wait_items(
            std::make_move_iterator(arrived.begin()), std::make_move_iterator(arrived.end()));
        wait_items.sort(comp);
        wait_queue_.merge(wait_items, comp);
      }

      std::forward_list<std::shared_ptr<WaitItem>> rescheduled;
      while (!wait_queue_.empty() && wait_queue_.front()->due_ <= clock::now()) {
        auto wait_item = std::move(wait_queue_.front());
        wait_queue_.pop_front();

        auto state = WaitItem::State::kIdle;
        if (wait_item->state_.compare_exchange_strong(state, WaitItem::State::kInCallback,
                                                      std::memory_order_acq_rel)) {
          REX_ASSERT(wait_item->callback_ != nullptr);
          wait_item->callback_(wait_item->userdata_);

          if (wait_item->interval_ != clock::duration::zero() &&
              wait_item->state_.load(std::memory_order_acquire) !=
                  WaitItem::State::kInCallbackSelfDisarmed) {
            wait_item->due_ += wait_item->interval_;
            wait_item->state_.store(WaitItem::State::kIdle, std::memory_order_release);
            wait_item->state_.notify_all();
            rescheduled.push_front(std::move(wait_item));
          } else {
            wait_item->state_.store(WaitItem::State::kDisarmed, std::memory_order_release);
            wait_item->state_.notify_all();
          }
        } else {
          REX_ASSERT(WaitItem::State::kDisarmed == state);
        }
      }
      rescheduled.sort(comp);
      wait_queue_.merge(rescheduled, comp);
    }
  }

  std::weak_ptr<WaitItem> QueueTimer(std::shared_ptr<WaitItem> wait_item) {
    std::weak_ptr<WaitItem> wait_item_weak = wait_item;
    wait_item->due_ = std::max(clock::now() - wait_item->interval_, wait_item->due_);
    {
      std::lock_guard lock(mutex_);
      pending_.push_back(std::move(wait_item));
    }
    wake_.notify_one();
    return wait_item_weak;
  }

  std::jthread::id GetDispatchThreadId() const { return dispatch_thread_.get_id(); }

 private:
  std::mutex mutex_;
  std::condition_variable_any wake_;
  std::vector<std::shared_ptr<WaitItem>> pending_;
  std::forward_list<std::shared_ptr<WaitItem>> wait_queue_;
  std::jthread dispatch_thread_;
};

namespace {

std::mutex g_timer_queue_mutex;
std::unique_ptr<TimerQueue> g_timer_queue;

TimerQueue& Queue() {
  std::lock_guard lock(g_timer_queue_mutex);
  if (!g_timer_queue) {
    g_timer_queue = std::make_unique<TimerQueue>();
  }
  return *g_timer_queue;
}

}  // namespace

void detail::ShutdownTimerQueue() {
  std::unique_ptr<TimerQueue> queue;
  {
    std::lock_guard lock(g_timer_queue_mutex);
    queue = std::move(g_timer_queue);
  }
}

bool detail::IsTimerQueueRunning() {
  std::lock_guard lock(g_timer_queue_mutex);
  return g_timer_queue != nullptr;
}

void TimerQueueWaitItem::Disarm() {
  State state;

  if (std::this_thread::get_id() == parent_queue_->GetDispatchThreadId()) {
    state = State::kInCallback;
    if (state_.compare_exchange_strong(state, State::kInCallbackSelfDisarmed,
                                       std::memory_order_acq_rel)) {
      return;
    }
  }

  state = State::kIdle;
  // No callback may run after Disarm() returns, since owners free memory callbacks access.
  while (!state_.compare_exchange_weak(state, State::kDisarmed, std::memory_order_acq_rel)) {
    if (state == State::kDisarmed) {
      break;
    }
    if (state == State::kInCallback || state == State::kInCallbackSelfDisarmed) {
      state_.wait(state, std::memory_order_acquire);
    }
    state = State::kIdle;
  }
}

std::weak_ptr<WaitItem> QueueTimerOnce(std::function<void(void*)> callback, void* userdata,
                                       WaitItem::clock::time_point due) {
  TimerQueue& queue = Queue();
  return queue.QueueTimer(std::make_shared<WaitItem>(std::move(callback), userdata, &queue, due,
                                                     WaitItem::clock::duration::zero()));
}

std::weak_ptr<WaitItem> QueueTimerRecurring(std::function<void(void*)> callback, void* userdata,
                                            WaitItem::clock::time_point due,
                                            WaitItem::clock::duration interval) {
  TimerQueue& queue = Queue();
  return queue.QueueTimer(
      std::make_shared<WaitItem>(std::move(callback), userdata, &queue, due, interval));
}

}  // namespace rex::platform
