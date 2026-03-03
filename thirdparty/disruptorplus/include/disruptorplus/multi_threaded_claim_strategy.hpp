#ifndef DISRUPTORPLUS_MULTI_THREADED_CLAIM_STRATEGY_HPP_INCLUDED
#define DISRUPTORPLUS_MULTI_THREADED_CLAIM_STRATEGY_HPP_INCLUDED

#include "config.hpp"
#include "sequence_barrier.hpp"
#include "sequence_barrier_group.hpp"
#include "sequence_range.hpp"

#include <atomic>
#include <chrono>
#include <memory>

namespace disruptorplus
{
    /// \brief
    /// A claim strategy for slots in a ring buffer where there are multiple
    /// producer threads all concurrently trying to write items to the ring
    /// buffer.
    ///
    /// When a writer wants to write to a slot in the queue it first atomically
    /// increments a counter by the number of slots it wishes to allocate.
    /// It then waits until all of those slots have become available and then
    /// returns the range of sequence numbers allocated back to the caller.
    /// The caller then writes to those slots and when done publishes them
    /// by writing the sequence numbers published to each of the slots to
    /// the corresponding element of an array of equal size to the ring buffer.
    /// When a reader wants to check if the next sequence number is available
    /// it them simply needs to read from the corresponding slot in this array
    /// to check if the value stored there is equal to the sequence number it
    /// is wanting to read.
    ///
    /// This means concurrent writers are wait-free when there is space available
    /// in the ring buffer, requiring a single atomic fetch-add operation as the
    /// only contended write operation. All other writes are to memory locations
    /// owned by a particular writer. Concurrent writers can publish items
    /// out-of-order so that one writer does not hold up other writers until the
    /// ring buffer fills up.
    ///
    /// \tparam WaitStrategy
    /// The type of wait-strategy object to use to block threads when waiting
    /// for sequence numbers to be published.
    ///
    /// \see single_threaded_claim_strategy
    /// For an alternative implementation of a claim strategy where only one
    /// thread is publishing items to the ring buffer.
    template<typename WaitStrategy>
    class multi_threaded_claim_strategy
    {
    public:
    
        /// \brief
        /// Initialise a new claim strategy.
        ///
        /// \param bufferSize
        /// The number of elements in the ring buffer.
        /// Must be a power of two.
        ///
        /// \param waitStrategy
        /// The wait strategy to use when waiting for sequence numbers to
        /// be published.
        multi_threaded_claim_strategy(
            size_t bufferSize,
            WaitStrategy& waitStrategy)
            : m_indexMask(bufferSize - 1)
            , m_bufferSize(bufferSize)
            , m_waitStrategy(waitStrategy)
            , m_claimBarrier(waitStrategy)
            , m_published(new std::atomic<sequence_t>[bufferSize])
            , m_nextClaimable(0)
        {
            // bufferSize must be power-of-two
            assert(m_bufferSize > 0 && (m_bufferSize & (m_bufferSize - 1)) == 0);
            
            // Initialise the buffer such that sequences before 0 have already
            // been published.
            for (sequence_t i = 0; i < bufferSize; ++i)
            {
                m_published[i].store(static_cast<sequence_t>(i - bufferSize), std::memory_order_relaxed);
            }
        }
        
        /// \brief
        /// The number of elements in the ring-buffer.
        ///
        /// This will always be a power-of-two.
        size_t buffer_size() const { return m_bufferSize; }
        
        /// \brief
        /// Add a sequence barrier for claiming slots in the ring buffer.
        ///
        /// Add a sequence barrier that prevents elements of the ring buffer from
        /// being claimed by writers until the reader has indicated the slot is
        /// available by publishing the sequence number they have finished reading.
        /// Claimed slots will never advance more than buffer_size() ahead
        /// of any of the registered claim barriers.
        ///
        /// \param barrier
        /// The sequence barrier to add.
        /// A reference to the sequence barrier is held by the claim strategy
        /// so the caller must ensure the lifetime of the sequence barrier
        /// exceeds that of the cliam strategy.
        /// This barrier must have been constructed with the same wait strategy object
        /// as the claim strategy was constructed with.
        /// \note
        /// This operation is not thread-safe and the caller must ensure that no other
        /// threads are accessing the claim strategy concurrently with this call.
        void add_claim_barrier(sequence_barrier<WaitStrategy>& barrier)
        {
            m_claimBarrier.add(barrier);
        }
        
        /// \brief
        /// Add a sequence barrier group for claiming slots in the ring buffer.
        ///
        /// Adds sequence barriers that prevent elements of the ring buffer from
        /// being claimed by writers until the readers have indicated the slot is
        /// available by publishing the sequence number they have finished reading.
        /// Claimed slots will never advance more than buffer_size() ahead
        /// of any of the registered claim barriers.
        ///
        /// \param barrier
        /// The sequence barriers to add.
        /// A reference to each sequence barrier in the group is held by the claim
        /// strategy, so the caller must ensure the lifetime of the sequence barriers
        /// exceed that of the cliam strategy.
        /// This barrier must have been constructed with the same wait strategy object
        /// as the claim strategy was constructed with.
        ///
        /// \note
        /// This operation is not thread-safe and the caller must ensure that no other
        /// threads are accessing the claim strategy concurrently with this call.
        void add_claim_barrier(sequence_barrier_group<WaitStrategy>& barrier)
        {
            m_claimBarrier.add(barrier);
        }
        
        /// \brief
        /// Claim a single slot in the ring buffer for writing to.
        ///
        /// Blocks the caller until a slot is available.
        ///
        /// The caller must call \ref publish(sequence_t) once it has
        /// finished writing to the slot to publish the item to readers.
        ///
        /// \return
        /// The sequence number of the slot claimed.
        sequence_t claim_one()
        {
            sequence_t sequence = m_nextClaimable.fetch_add(1, std::memory_order_relaxed);
            m_claimBarrier.wait_until_published(
                static_cast<sequence_t>(sequence - m_bufferSize));
            return sequence;
        }
        
        /// \brief
        /// Claim up to \p count consecutive slots in the ring buffer.
        ///
        /// Blocks the caller until items are available.
        /// Will allocate at most \ref buffer_size() slots in the ring buffer.
        ///
        /// This operation has 'acquire' memory semantics.
        ///
        /// \param count
        /// The maximum number of slots to claim.
        ///
        /// \return
        /// A sequence range indicating the sequence numbers that were claimed.
        /// This sequence range may contain less items than requested but will
        /// contain at least one slot if \p count is non-zero.
        sequence_range claim(size_t count)
        {
            count = std::min(count, m_bufferSize);
            sequence_t sequence = m_nextClaimable.fetch_add(count, std::memory_order_relaxed);
            sequence_range range(sequence, count);
            m_claimBarrier.wait_until_published(
                static_cast<sequence_t>(range.last() - m_bufferSize));
            return range;
        }
        
        /// \brief
        /// Attempt to claim up to \p count slots in the ring buffer without blocking.
        ///
        /// This operation has 'acquire' memory semantics.
        ///
        /// \param count
        /// The maximum number of slots to claim.
        ///
        /// \param range [out]
        /// If any slots were claimed then this variable is populated with the
        /// range of sequence numbers that were claimed. If no slots were claimed
        /// then the value is left unchanged.
        ///
        /// \return
        /// Returns \c true if any elements were claimed in which case the range of
        /// slots claimed is written to the \p range out-parameter. May claim less
        /// slots than requested if fewer slots were available.
        /// Returns \c false if no elements were claimed. The \p range out-parameter is
        /// not modified in this case.
        bool try_claim(size_t count, sequence_range& range)
        {
            sequence_t published =
                static_cast<sequence_t>(m_claimBarrier.last_published() + m_bufferSize);
            
            sequence_t sequence = m_nextClaimable.load(std::memory_order_relaxed);
            do
            {
                sequence_diff_t diff = difference(published, sequence);
                if (diff < 0)
                {
                    return false;
                }
                count = std::min(count, static_cast<size_t>(diff + 1));
            } while (!m_nextClaimable.compare_exchange_weak(
                sequence,
                static_cast<sequence_t>(sequence + count),
                std::memory_order_relaxed,
                std::memory_order_relaxed));
                
            range = sequence_range(sequence, count);
            return true;
        }
        
        /// \brief
        /// Attempt to claim up to \p count slots in the ring buffer.
        ///
        /// Blocks the caller until either at least one slot was claimed or until
        /// the specified timeout period has elapsed, whichever comes first.
        ///
        /// This operation has 'acquire' memory semantics.
        ///
        /// \param count
        /// The maximum number of slots to claim.
        ///
        /// \param range [out]
        /// Receives the range of sequence numbers claimed if the operation succeeds.
        /// Value is left unchanged if no slots were claimed.
        ///
        /// \param timeout
        /// The maximum time to wait for claiming any slots.
        ///
        /// \return
        /// Returns \c true if any slots were claimed in which case the range
        /// of slots claimed is written to the \p range out-parameter.
        /// Returns \c false if the timeout duration was exceeded without
        /// claiming any slots.
        ///
        /// \throw std::exception
        /// May throw any exception thrown by WaitStrategy::wait_until_published().
        template<class Rep, class Period>
        bool try_claim_for(
            size_t count,
            sequence_range& range,
            const std::chrono::duration<Rep, Period>& timeout)
        {
            return try_claim_until(
                count,
                range,
                std::chrono::high_resolution_clock::now() + timeout);
        }
        
        /// \brief
        /// Attempt to claim up to \p count slots in the ring buffer.
        ///
        /// Blocks the caller until either at least one slot was claimed or until
        /// the specified timeout period has elapsed, whichever comes first.
        ///
        /// This operation has 'acquire' memory semantics.
        ///
        /// \param count
        /// The maximum number of slots to claim.
        ///
        /// \param range [out]
        /// Receives the range of sequence numbers claimed if the operation succeeds.
        /// Value is left unchanged if no slots were claimed.
        ///
        /// \param timeoutTime
        /// The time after which the call should stop waiting for available slots.
        ///
        /// \return
        /// Returns \c true if any slots were claimed in which case the range
        /// of slots claimed is written to the \p range out-parameter.
        /// Returns \c false if the timeout duration was exceeded without
        /// claiming any slots.
        ///
        /// \throw std::exception
        /// May throw any exception thrown by \c Clock::now() or
        /// \c WaitStrategy::wait_until_published().
        template<class Clock, class Duration>
        bool try_claim_until(
            size_t count,
            sequence_range& range,
            const std::chrono::time_point<Clock, Duration>& timeoutTime)
        {
            sequence_t published =
                static_cast<sequence_t>(m_claimBarrier.last_published() + m_bufferSize);
            
            sequence_t sequence = m_nextClaimable.load(std::memory_order_relaxed);
            size_t reducedCount;
            do
            {
                sequence_diff_t diff = difference(published, sequence);
                if (diff < 0)
                {
                    published = static_cast<sequence_t>(
                        m_claimBarrier.wait_until_published(
                            static_cast<sequence_t>(sequence - m_bufferSize),
                            timeoutTime) + m_bufferSize);
                    diff = difference(published, sequence);
                    if (diff < 0)
                    {
                        // Timeout
                        return false;
                    }
                }
                reducedCount = std::min(count, static_cast<size_t>(diff + 1));
            } while (!m_nextClaimable.compare_exchange_weak(
                sequence,
                static_cast<sequence_t>(sequence + reducedCount),
                std::memory_order_relaxed,
                std::memory_order_relaxed));
                
            range = sequence_range(sequence, reducedCount);
            
            return true;
        }
        
        /// \brief
        /// Publish an element written to a slot in the ring buffer,
        /// signalling that the element is available to be accessed by
        /// reader threads.
        ///
        /// A reader will not be notified that this element is available
        /// until all other sequences prior to this element have also
        /// been published.
        ///
        /// A writer must call this method when they have finished
        /// writing data to the ring-buffer element they previously
        /// claimed by calling one of the 'claim' methods above.
        ///
        /// This operation has 'release' memory semantics.
        ///
        /// \note
        /// The meaning of this method is slightly different from that
        /// of the \ref single_threaded_claim_strategy::publish(sequence_t) method.
        /// As multiple threads may publish elements out of order, every
        /// sequence number must be published, whereas for the single-threaded
        /// claim strategy it is assumed that all prior sequences have already
        /// been published when publishing the next sequence.
        ///
        /// \param sequence
        /// The sequence number of the element being published.
        ///
        /// \throw std::exception
        /// Throws any exception thrown by WaitStrategy::signal_all_when_blocking().
        void publish(sequence_t sequence)
        {
            set_published(sequence);
            m_waitStrategy.signal_all_when_blocking();
        }
        
        /// \brief
        /// Publish a sequence of elements written to slots in the ring buffer,
        /// signalling that the elements are available to be accessed by
        /// reader threads.
        ///
        /// A reader will not be notified that these elements are available
        /// until all other sequences prior to these elements have also
        /// been published.
        ///
        /// A writer must call this method when they have finished
        /// writing data to the ring-buffer elements they previously
        /// claimed by calling one of the 'claim' methods above.
        ///
        /// This operation has 'release' memory semantics.
        ///
        /// \param range
        /// The range of sequence numbers to publish.
        ///
        /// \throw std::exception
        /// Throws any exception thrown by WaitStrategy::signal_all_when_blocking().
        void publish(const sequence_range& range)
        {
            for (size_t i = 0, j = range.size(); i < j; ++i)
            {
                set_published(range[i]);
            }
            m_waitStrategy.signal_all_when_blocking();
        }
        
        /// \brief
        /// Return the highest sequence number published after the specified
        /// last-known published sequence.
        ///
        /// \param lastKnownPublished
        /// This sequence number is assumed to have already been published.
        /// The initial value passed in here on first call should be sequence_t(-1).
        ///
        /// \return
        /// The last-published sequence number.
        /// This will be equal to \p lastKnownPublished if no additional sequences
        /// have been published.
        sequence_t last_published_after(sequence_t lastKnownPublished) const
        {
            sequence_t seq = lastKnownPublished + 1;
            while (is_published(seq))
            {
                lastKnownPublished = seq;
                ++seq;
            }
            return lastKnownPublished;
        }
        
        /// \brief
        /// Block the caller until the specified sequence number has been
        /// published.
        ///
        /// This method is called by reader threads waiting to consume
        /// items written to the ring buffer.
        ///
        /// \param sequence
        /// The sequence number to wait for.
        ///
        /// \param lastKnownPublished
        /// This sequence number is assumed to have already been published.
        /// The initial value passed in here on first call should be sequence_t(-1).
        ///
        /// \return
        /// Returns the sequence number of the latest available published
        /// sequence, guaranteed to be equal to or later than the specified
        /// \p sequence parameter.
        ///
        /// \throw std::exception
        /// Throws any exception thrown by \c WaitStrategy::wait_until_published().
        sequence_t wait_until_published(
            sequence_t sequence,
            sequence_t lastKnownPublished) const
        {
            assert(difference(sequence, lastKnownPublished) > 0);
            
            for (sequence_t seq = lastKnownPublished + 1;
                 difference(seq, sequence) <= 0;
                 ++seq)
            {
                if (!is_published(seq))
                {
                    const std::atomic<sequence_t>* const sequences[1] =
                        { &m_published[seq & m_indexMask] };
                    m_waitStrategy.wait_until_published(seq, 1, sequences);
                }
            }
            return last_published_after(sequence);
        }
        
        /// \brief
        /// Block the caller until the specified sequence number has been
        /// published or until a specified timeout has elapsed.
        ///
        /// This method is called by reader threads waiting to consume
        /// items written to the ring buffer.
        ///
        /// \param sequence
        /// The sequence number to wait for.
        ///
        /// \param lastKnownPublished
        /// This sequence number is assumed to have already been published.
        /// The initial value passed in here on first call should be sequence_t(-1).
        ///
        /// \param timeout
        /// The maximum time to wait for the sequence number to be published.
        ///
        /// \return
        /// Returns the sequence number of the latest available published
        /// element. If this value is prior to the requested \p sequence
        /// number then the operation timed-out before the sequence
        /// was published.
        template<typename Rep, typename Period>
        sequence_t wait_until_published(
            sequence_t sequence,
            sequence_t lastKnownPublished,
            const std::chrono::duration<Rep, Period>& timeout) const
        {
            return wait_until_published(
                sequence,
                lastKnownPublished,
                std::chrono::high_resolution_clock::now() + timeout);
        }
        
        /// \brief
        /// Block the caller until the specified sequence number has been
        /// published or until a specified timeout has elapsed.
        ///
        /// This method is called by reader threads waiting to consume
        /// items written to the ring buffer.
        ///
        /// \param sequence
        /// The sequence number to wait for.
        ///
        /// \param lastKnownPublished
        /// This sequence number is assumed to have already been published.
        /// The initial value passed in here on first call should be sequence_t(-1).
        ///
        /// \param timeoutTime
        /// The time after which the operation should cease blocking the caller
        /// if the sequence number has not yet been published.
        ///
        /// \return
        /// Returns the sequence number of the latest available published
        /// element. If this value is prior to the requested \p sequence
        /// number then the operation timed-out before the sequence
        /// was published.
        template<typename Clock, typename Duration>
        sequence_t wait_until_published(
            sequence_t sequence,
            sequence_t lastKnownPublished,
            const std::chrono::time_point<Clock, Duration>& timeoutTime) const
        {
            assert(difference(sequence, lastKnownPublished) > 0);
            
            for (sequence_t seq = lastKnownPublished + 1;
                 difference(seq, sequence) <= 0;
                 ++seq)
            {
                if (!is_published(seq))
                {
                    const std::atomic<sequence_t>* const sequences[1] =
                        { &m_published[seq & m_indexMask] };
                    sequence_t result =
                        m_waitStrategy.wait_until_published(seq, 1, sequences, timeoutTime);
                    if (difference(result, seq) < 0)
                    {
                        // Timeout. seq is the first non-published sequence
                        return seq - 1;
                    }
                }
            }
            return last_published_after(sequence);
        }
        
    private:
    
        bool is_published(sequence_t sequence) const
        {
            return m_published[sequence & m_indexMask].load(std::memory_order_acquire) == sequence;
        }
        
        void set_published(sequence_t sequence)
        {
            auto& entry = m_published[sequence & m_indexMask];
            assert(entry.load(std::memory_order_relaxed) == static_cast<sequence_t>(sequence - m_bufferSize));
            entry.store(sequence, std::memory_order_release);
        }
    
        const sequence_t m_indexMask;
        const size_t m_bufferSize;
        
        WaitStrategy& m_waitStrategy;
        
        sequence_barrier_group<WaitStrategy> m_claimBarrier;
        
        const std::unique_ptr<std::atomic<sequence_t>[]> m_published;

        // Since this m_nextClaimable is going to be written to by multiple
        // threads, we don't want false sharing with m_published or other
        // variables that occur after it in the heap/stack.
        uint8_t m_pad0[CacheLineSize - sizeof(sequence_t)];
        std::atomic<sequence_t> m_nextClaimable;
        uint8_t m_pad1[CacheLineSize - sizeof(sequence_t)];
        
    };
}

#endif
