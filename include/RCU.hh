#pragma once

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>
#include <type_traits>
#include <list>

#include <linux/membarrier.h>
#include <unistd.h>
#include <sys/syscall.h>

// Probably basically right.
const size_t CACHE_LINE_BYTES = 64;

const std::uint64_t GP_COUNTER_MASK = 1ULL << 63;
const std::uint64_t NESTING_MASK    = ~GP_COUNTER_MASK;

struct PerThreadEntry  {
    std::atomic<std::uint64_t> gracePeriodCounter;
};

static thread_local PerThreadEntry threadLocalEntry;

// Manages userspace RCU synchronization.
class RcuManager {
public:
    RcuManager() : globalGracePeriod(1), entries(), mutex() {}

    // Register that the current process wants to use RCU.
    //
    // Must be called before any other thread in the process calls any other
    // methods. Only needs to be called once; subsequent calls will have the
    // same return value and no additional effect.
    //
    // Returns a boolean which is true on success, or false on failure.
    // Failures occur if the system doesn't support expedited Linux
    // `membarrier`. If this method fails RCU will not work.
    bool registerCurrentProcess(void);

    // Add the curren thread to the RCU registry.
    //
    // Must be called before this thread calls unregisterCurrentThread,
    // readLock, readUnlock, or synchronize.
    void registerCurrentThread(void);

    // Remove the current thread from the RCU registry.
    //
    // Must be called before thread destruction. After this is called,
    // registerCurrentThread may be safely called again to re-register the
    // thread. Until the thread is re-registered it cannot use RCU.
    void unregisterCurrentThread(void);

    // Delay reclamation of memory by other threads.
    //
    // Readers and writers should call readLock before starting a read
    // operation on shared data, and readUnlock once the operation is
    // finished.  Each readLock must be paired with a corresponding readUnlock.
    // The period between the readLock and the corresponding readUnlock is a
    // "read-side critical section". See synchronize for the semantics of a
    // read-side critical section.
    //
    // Whenever a reader thread is not in a read-side critical section, it is
    // in a "quiescent state".
    inline void readLock(void) const {
        auto tmp = threadLocalEntry.gracePeriodCounter.load(
                std::memory_order_relaxed);
        if (!(tmp & NESTING_MASK)) {
            auto global = globalGracePeriod.load(std::memory_order_relaxed);
            threadLocalEntry.gracePeriodCounter.store(global,
                    std::memory_order_relaxed);
        } else {
            threadLocalEntry.gracePeriodCounter.store(tmp + 1,
                    std::memory_order_relaxed);
        }
    }

    // Mark the end of a read-side critical section.
    inline void readUnlock(void) const {
        auto tmp = threadLocalEntry.gracePeriodCounter.load(
                std::memory_order_relaxed);
        threadLocalEntry.gracePeriodCounter.store(tmp - 1,
                std::memory_order_relaxed);
    }

    // Wait until it is safe to reclaim inaccessible previously-shared memory.
    //
    // Specifically, waits until every reader thread is known to have passed
    // through a quiescent state. 
    //
    // Used to reclaim memory once it is made inaccessible from shared data
    // structures. For example, say we have a shared, RCU-protected linked
    // list like the following:
    //
    // ------      ------     ------
    // | N1 | ---> | N2 | --> | N3 |
    // ------      ------     ------
    //
    // And say a thread has a pointer to N1, and wants to delete N2. It would
    // do the following:
    //
    // 1. Atomically change N1's next pointer to point to N3, using CAS, a
    //    mutex, or whatever other mechanism.
    //
    // ------                 ------
    // | N1 | --------------> | N3 |
    // ------                 ------
    //             ------      ^
    //             | N2 | -----/
    //             ------
    //
    // 2. Call synchronize(). After synchronize is done, we know that no
    //    reader threads have pointers to N2, because they have passed through
    //    a quiescent state since we made N2 inaccessible.
    //
    // 3. Delete N2. This is now safe, because we have the only remaining
    //    pointer to it.
    //
	void synchronize(void);

private:
    // Use the membarrier syscall to wait until all threads run a full fence.
	inline void membarrierAllThreads(void);

    inline void toggleAndWaitForThreads(void);

    std::atomic<std::uint64_t> globalGracePeriod;
    std::list<PerThreadEntry *> entries;
    std::mutex mutex;
};
