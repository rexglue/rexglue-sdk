/**
 * @file        rex/platform/macos_compat.h
 * @brief       macOS compatibility shims for Linux-specific APIs
 *
 * Provides pipe-based eventfd, gettid, cpu_set_t stubs, etc.
 * Only included on Apple platforms.
 */

#pragma once

#ifdef __APPLE__

#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <cstdint>

// ============================================================================
// eventfd shim using pipes
// ============================================================================

#define EFD_CLOEXEC 0
#define EFD_NONBLOCK 0
#define EFD_SEMAPHORE 0

// eventfd compatibility: returns a pipe read fd, stores write fd in a global map
// For simplicity, we use a pipe pair. The "eventfd" fd is the read end.
// We store the write end by convention at fd+1 (not ideal, but works for our case).
// Better approach: use a side table.

#include <unordered_map>
#include <mutex>

namespace rex::platform::detail {
inline std::unordered_map<int, int>& eventfd_write_map() {
    static std::unordered_map<int, int> map;
    return map;
}
inline std::mutex& eventfd_mutex() {
    static std::mutex m;
    return m;
}
}  // namespace rex::platform::detail

inline int eventfd(unsigned int initval, int flags) {
    (void)flags;
    int pipefd[2];
    if (pipe(pipefd) != 0) return -1;

    // Set non-blocking on both ends
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    fcntl(pipefd[1], F_SETFL, O_NONBLOCK);
    fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);

    // Store the write end
    {
        std::lock_guard<std::mutex> lock(rex::platform::detail::eventfd_mutex());
        rex::platform::detail::eventfd_write_map()[pipefd[0]] = pipefd[1];
    }

    // Write initial value if non-zero
    if (initval > 0) {
        uint64_t val = initval;
        write(pipefd[1], &val, sizeof(val));
    }

    return pipefd[0];
}

inline int eventfd_write(int fd, uint64_t val) {
    int write_fd;
    {
        std::lock_guard<std::mutex> lock(rex::platform::detail::eventfd_mutex());
        auto it = rex::platform::detail::eventfd_write_map().find(fd);
        if (it == rex::platform::detail::eventfd_write_map().end()) return -1;
        write_fd = it->second;
    }
    return (write(write_fd, &val, sizeof(val)) == sizeof(val)) ? 0 : -1;
}

inline int eventfd_read(int fd, uint64_t* val) {
    return (read(fd, val, sizeof(*val)) == sizeof(*val)) ? 0 : -1;
}

inline void eventfd_close(int fd) {
    int write_fd = -1;
    {
        std::lock_guard<std::mutex> lock(rex::platform::detail::eventfd_mutex());
        auto it = rex::platform::detail::eventfd_write_map().find(fd);
        if (it != rex::platform::detail::eventfd_write_map().end()) {
            write_fd = it->second;
            rex::platform::detail::eventfd_write_map().erase(it);
        }
    }
    close(fd);
    if (write_fd >= 0) close(write_fd);
}

// ============================================================================
// gettid shim
// ============================================================================

inline uint64_t rex_gettid() {
    uint64_t tid;
    pthread_threadid_np(nullptr, &tid);
    return tid;
}

#ifndef gettid
#define gettid() ((pid_t)rex_gettid())
#endif

// ============================================================================
// cpu_set_t stubs (macOS doesn't support CPU affinity)
// ============================================================================

#ifndef CPU_SETSIZE
#define CPU_SETSIZE 1024

typedef struct {
    uint64_t __bits[CPU_SETSIZE / 64];
} cpu_set_t;

#define CPU_ZERO(set) memset((set), 0, sizeof(cpu_set_t))
#define CPU_SET(cpu, set) ((set)->__bits[(cpu) / 64] |= (1ULL << ((cpu) % 64)))
#define CPU_CLR(cpu, set) ((set)->__bits[(cpu) / 64] &= ~(1ULL << ((cpu) % 64)))
#define CPU_ISSET(cpu, set) (((set)->__bits[(cpu) / 64] & (1ULL << ((cpu) % 64))) != 0)
#define CPU_COUNT(set) ({                         \
    int _count = 0;                               \
    for (int _i = 0; _i < CPU_SETSIZE / 64; _i++) \
        _count += __builtin_popcountll((set)->__bits[_i]); \
    _count;                                       \
})
#endif

// sched_getaffinity/sched_setaffinity stubs (no-op on macOS)
inline int sched_getaffinity(pid_t pid, size_t cpusetsize, cpu_set_t* mask) {
    (void)pid; (void)cpusetsize;
    CPU_ZERO(mask);
    // Report all CPUs as available
    int ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
    for (int i = 0; i < ncpu; i++) CPU_SET(i, mask);
    return 0;
}

inline int sched_setaffinity(pid_t pid, size_t cpusetsize, const cpu_set_t* mask) {
    (void)pid; (void)cpusetsize; (void)mask;
    return 0;  // No-op on macOS
}

// ============================================================================
// pthread_gettid_np shim (Linux version returns pid_t tid)
// ============================================================================

// On Linux, pthread_gettid_np(pthread_t) returns pid_t
// On macOS, pthread_threadid_np(pthread_t, uint64_t*) fills a uint64_t
// We redefine to match the Linux API
inline pid_t pthread_gettid_np_compat(pthread_t thread) {
    uint64_t tid;
    pthread_threadid_np(thread, &tid);
    return (pid_t)tid;
}
#define pthread_gettid_np(t) pthread_gettid_np_compat(t)

// ============================================================================
// syscall shim
// ============================================================================

#include <sys/syscall.h>

#ifndef SYS_gettid
#define SYS_gettid 0  // Unused, gettid() macro handles it
#endif

// ============================================================================
// SIGRTMIN/SIGRTMAX shim (macOS doesn't have real-time signals)
//
// macOS provides exactly 2 user-defined signals: SIGUSR1 (30) and SIGUSR2 (31).
// We need 2 slots (kThreadSuspend=0, kThreadUserCallback=1):
//   slot 0 -> SIGUSR1 (30)  ->  assert 30 < 32  OK
//   slot 1 -> SIGUSR2 (31)  ->  assert 31 < 32  OK
//
// SIGRTMAX must be SIGUSR2+1 so that "result < SIGRTMAX" passes for both slots.
// SIGRTMAX=SIGUSR2 only passes for slot 0 (30 < 31) and CRASHES for slot 1
// (31 < 31 is false) -> assert_true fires -> abort() -> SIGABRT.
// ============================================================================

#ifndef SIGRTMIN
#define SIGRTMIN SIGUSR1        // 30: base of our fake RT range
#endif

#ifndef SIGRTMAX
#define SIGRTMAX (SIGUSR2 + 1)  // 32: exclusive upper bound; both slots fit
#endif

// ============================================================================
// pthread_setname_np shim
// On Linux: pthread_setname_np(pthread_t, const char*)
// On macOS: pthread_setname_np(const char*) - only for current thread
// Use rex_pthread_setname_np to avoid conflicts with system declaration
// ============================================================================

inline int rex_pthread_setname_np(pthread_t thread, const char* name) {
    if (pthread_equal(thread, pthread_self())) {
        return pthread_setname_np(name);
    }
    return 0;  // Can't set name of other threads on macOS
}

// ============================================================================
// pthread_getaffinity_np / pthread_setaffinity_np shims
// macOS doesn't support these - use no-op stubs
// ============================================================================

inline int pthread_getaffinity_np(pthread_t thread, size_t cpusetsize, cpu_set_t* cpuset) {
    (void)thread;
    return sched_getaffinity(0, cpusetsize, cpuset);
}

inline int pthread_setaffinity_np(pthread_t thread, size_t cpusetsize, const cpu_set_t* cpuset) {
    (void)thread;
    return sched_setaffinity(0, cpusetsize, cpuset);
}

// ============================================================================
// pthread_sigqueue / sigqueue shim (macOS has no real sigqueue)
//
// Linux's sigqueue() and pthread_sigqueue() carry a 'sigval' payload that is
// delivered in siginfo_t::si_value.  macOS has no equivalent - the only option
// is pthread_kill() which drops the payload entirely.
//
// To preserve the sival_ptr pointer that threading.cpp needs for
// kThreadUserCallback, we maintain a per-thread side table.
// threading.cpp calls rex_macos_sigqueue_store() before sending the signal
// and rex_macos_sigqueue_retrieve() inside the signal handler.
// ============================================================================

#include <unordered_map>
#include <mutex>
#include <signal.h>

namespace rex::platform::detail {
inline std::mutex& sigqueue_ptr_mutex() {
    static std::mutex m;
    return m;
}
inline std::unordered_map<pthread_t, void*>& sigqueue_ptr_map() {
    static std::unordered_map<pthread_t, void*> m;
    return m;
}
}  // namespace rex::platform::detail

// Store a sival_ptr for a target thread before sending the signal.
inline void rex_macos_sigqueue_store(pthread_t thread, void* ptr) {
    std::lock_guard<std::mutex> lk(rex::platform::detail::sigqueue_ptr_mutex());
    rex::platform::detail::sigqueue_ptr_map()[thread] = ptr;
}

// Retrieve (and erase) the stored pointer for the calling thread.
inline void* rex_macos_sigqueue_retrieve() {
    std::lock_guard<std::mutex> lk(rex::platform::detail::sigqueue_ptr_mutex());
    auto& map = rex::platform::detail::sigqueue_ptr_map();
    auto it = map.find(pthread_self());
    if (it == map.end()) return nullptr;
    void* ptr = it->second;
    map.erase(it);
    return ptr;
}

// pthread_sigqueue: store the payload then deliver via pthread_kill.
inline int pthread_sigqueue(pthread_t thread, int sig, const union sigval value) {
    if (value.sival_ptr) {
        rex_macos_sigqueue_store(thread, value.sival_ptr);
    }
    return pthread_kill(thread, sig);
}

// sigqueue: same idea but keyed on pid -> use pthread_self as approximation.
#ifndef sigqueue
inline int sigqueue(pid_t /*pid*/, int sig, const union sigval value) {
    // pid-targeted sigqueue is only used for kThreadUserCallback on Android;
    // on macOS this path should never be taken, but provide a safe stub.
    (void)value;
    return kill(getpid(), sig);
}
#endif

#endif  // __APPLE__
