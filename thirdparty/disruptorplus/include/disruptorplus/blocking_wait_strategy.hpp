#ifndef DISRUPTORPLUS_BLOCKING_WAIT_STRATEGY_HPP_INCLUDED
#define DISRUPTORPLUS_BLOCKING_WAIT_STRATEGY_HPP_INCLUDED

#include "sequence.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace disruptorplus
{
    /// \brief
    /// A wait_strategy that blocks waiting threads using a until the respective
    /// sequence barriers have reached the desired sequence number using a
    /// condition-variable.
    ///
    /// All currently blocked threads will be woken when any sequence
    /// barrier publishes a new sequence regardless of whether those
    /// threads are currently waiting on that sequence barrier or not.
    ///
    /// This strategy is CPU efficient for cases where there may
    /// be long periods of inactivity when either producer or consumer
    /// threads are starved, but has the downside of using kernel
    /// calls which can introduce uncertainty in processing latency.
    class blocking_wait_strategy
    {
    public:
    
        /// \brief
        /// Initialise the synchronisation resources used by the wait strategy.
        ///
        /// \throw std::system_error
        /// If unable to initialise the resources.
        blocking_wait_strategy()
        {}
        
        /// \brief
        /// Wait unconditionally until all of the specified sequences
        /// have at least published the specified sequence value.
        ///
        /// \param sequence
        /// The sequence number to wait for.
        ///
        /// \param count
        /// The number of elements in \p sequences.
        /// Must be greater than zero.
        ///
        /// \param sequences
        /// An array of \p count pointers to sequence values.
        /// The call will not return until all of these values have advanced
        /// beyond the specified \p sequence.
        ///
        /// \return
        /// The value of the least-advanced sequence.
        /// This value is guaranteed to be at least \p sequence.
        ///
        /// \throw std::system_error
        /// If the system does not have enough resources to perform this operation.
        sequence_t wait_until_published(
            sequence_t sequence,
            size_t count,
            const std::atomic<sequence_t>* const sequences[])
        {
            assert(count > 0);
            sequence_t result;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [&]() -> bool {
                    result = minimum_sequence_after(sequence, count, sequences);
                    return difference(result, sequence) >= 0;
                    });
            }
            return result;
        }
        
        /// \brief
        /// Wait until either all of the specified sequences have at least
        /// published the specified sequence value or a timeout is reached.
        ///
        /// \param sequence
        /// The sequence number to wait for.
        ///
        /// \param count
        /// The number of elements in \p sequences.
        /// Must be greater than zero.
        ///
        /// \param sequences
        /// An array of \p count pointers to sequence values.
        /// The call will wait until all of these sequence values have
        /// published a sequence number at or after the specified
        /// \p sequence number.
        ///
        /// \param timeout
        /// The maximum amount of time to wait for the \p sequences
        /// numbers to advance to \p sequence.
        ///
        /// \return
        /// If the operation timed out then returns some number such
        /// that <tt>difference(result, sequence) < 0</tt>, otherwise
        /// returns the least-advanced of all the sequence values read
        /// from \p sequences, which is guaranteed to satisfy
        /// <tt>difference(result, sequence) >= 0</tt>.
        ///
        /// \throw std::system_error
        /// If the system does not have enough resources to perform this operation.
        template<typename Rep, typename Period>
        sequence_t wait_until_published(
            sequence_t sequence,
            size_t count,
            const std::atomic<sequence_t>* const sequences[],
            const std::chrono::duration<Rep, Period>& timeout)
        {
            assert(count > 0);
            sequence_t result;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                // std::condition_variable takes the time before the predicate (the
                // original order did not compile once instantiated).
                m_cv.wait_for(
                    lock,
                    timeout,
                    [&]() -> bool {
                        result = minimum_sequence_after(sequence, count, sequences);
                        return difference(result, sequence) >= 0;
                    });
            }
            return result;
        }

        /// \brief
        /// Wait until either all of the specified sequences have at least
        /// published the specified sequence value or a timeout time is reached.
        ///
        /// \param sequence
        /// The sequence number to wait for.
        ///
        /// \param count
        /// The number of elements in \p sequences.
        /// Must be greater than zero.
        ///
        /// \param sequences
        /// An array of \p count pointers to sequence values.
        /// The call will wait until all of these sequence values have
        /// published a sequence number at or after the specified
        /// \p sequence number.
        ///
        /// \param timeoutTime
        /// The time to wait until for the \p sequences numbers to
        /// advance to \p sequence. The operation will timeout after
        /// this point in time.
        ///
        /// \return
        /// If the operation timed out then returns some number such
        /// that <tt>difference(result, sequence) < 0</tt>, otherwise
        /// returns the least-advanced of all the sequence values read
        /// from \p sequences, which is guaranteed to satisfy
        /// <tt>difference(result, sequence) >= 0</tt>.
        ///
        /// \throw std::system_error
        /// If the system does not have enough resources to perform this operation.
        template<typename Clock, typename Duration>
        sequence_t wait_until_published(
            sequence_t sequence,
            size_t count,
            const std::atomic<sequence_t>* const sequences[],
            const std::chrono::time_point<Clock, Duration>& timeoutTime)
        {
            assert(count > 0);
            sequence_t result;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                // std::condition_variable takes the time before the predicate (the
                // original order did not compile once instantiated).
                m_cv.wait_until(
                    lock,
                    timeoutTime,
                    [&]() -> bool {
                        result = minimum_sequence_after(sequence, count, sequences);
                        return difference(result, sequence) >= 0;
                    });
            }
            return result;
        }

        /// \brief
        /// Notify any waiting threads that one of the sequence values has changed.
        ///
        /// Wakes all waiting threads so they can re-check whether their target
        /// sequence numbers are now satisfied.
        void signal_all_when_blocking()
        {
            // Take out a lock here because we don't want to notify other threads
            // if they are between checking the sequence values and waiting on
            // the condition-variable.
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.notify_all();
        }
        
    private:
    
        std::mutex m_mutex;
        std::condition_variable m_cv;
    
    };
}

#endif
