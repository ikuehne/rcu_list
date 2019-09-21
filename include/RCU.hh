// An implementation of userspace RCU (Read-Copy-Update).
//
// This algorithm is based on [1].
//
// Allows deferring reclamation of memory until after readers leave their
// critical sections, while imposing minimal overhead on readers. Also
// provides a garbage-collection facility for doing the reclamation
// asynchronously.
//
// [1] M. Desnoyers et al., "User-Level Implementations of Read-Copy Update,"
//     IEEE Transactions on Parallel and Distributed Systems, 2011.
//     [Online serial] Available:
//     https://www.efficios.com/pub/rcu/urcu-main.pdf. [Accessed Sept, 2019].
//

#pragma once

#include <atomic>
#include <cassert>
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
    RcuManager() : mutex(), globalGracePeriod(1), entries() {}

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

    // Add the current thread to the RCU registry.
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
    // "read-side critical section". Read-side critical sections can be
    // arbitrarily nested on a single thread; nested critical sections are
    // functionally equivalent to a single critical section of the same
    // duration as the widest nesting. That is, the following series of
    // operations:
    //
    //     readLock();
    //     A();
    //     readLock();
    //     B();
    //     readUnlock();
    //     C();
    //     readUnlock();
    //
    // Is functionally equivalent to:
    //
    //     readLock();
    //     A();
    //     B();
    //     C();
    //     readUnlock();
    //
    // Whenever a reader thread is not in a read-side critical section, it is
    // in a "quiescent state".
    //
    // See synchronize for the semantics of read-side critical sections and
    // quiescent states.
    inline void readLock(void) const {
        auto tmp = threadLocalEntry.gracePeriodCounter.load(
                std::memory_order_relaxed);
        // If our nesting is currently 0,
        if (!(tmp & NESTING_MASK)) {
            // Simultaneously set nesting to 1 and read the current grace
            // period.
            auto global = globalGracePeriod.load(std::memory_order_relaxed);
            threadLocalEntry.gracePeriodCounter.store(global,
                    std::memory_order_relaxed);
            // This "memory barrier" should compile to nothing.
            //
            // It's here because the first membarrierAllThreads call in
            // synchronize effectively synchronizes-with this "memory barrier"
            // to ensure that the start of our read-side critical section
            // happens-before any reads of shared data.
            std::atomic_thread_fence(std::memory_order_relaxed);
        } else {
            // Increment our nesting.
            threadLocalEntry.gracePeriodCounter.store(tmp + 1,
                    std::memory_order_relaxed);
        }
    }

    // Mark the end of a read-side critical section.
    inline void readUnlock(void) const {
        // Like the barrier in read-lock, this is essentially here for
        // documentation.
        //
        // This barrier synchronizes-with the barrier at the start of
        // `synchronize` to ensure that all of our reads happen-before we
        // enter a quiescent state.
        std::atomic_thread_fence(std::memory_order_relaxed);

        // Subtract one from our nesting.
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

    // Toggle the globalGracePeriod
    inline void toggleAndWaitForThreads(void);

    // The registry mutex.
    std::mutex mutex;

    // Contains the grace period in a single bit. Also contains a 1 in the low
    // bit, so that reader threads can simultaneously read the grace period
    // and set their nesting to 1.
    //
    // CAN ONLY BE MODIFIED WHILE HOLDING mutex. Reader threads atomically
    // read this without holding mutex.
    std::atomic<std::uint64_t> globalGracePeriod;

    // The thread registry.
    //
    // CAN ONLY BE READ OR MODIFIED WHILE HOLDING mutex.
    std::list<PerThreadEntry *> entries;
};

// Asynchronously deletes RCU-protected objects of type T.
template<typename T>
class GarbageCollector {
public:
    GarbageCollector(RcuManager &manager)
        : gcThread(gcLoop, this, std::ref(manager)), head(nullptr), done(false) {}

    // Wait until the GC thread is done, and then join it.
    void join(void) {
        done.store(true, std::memory_order_relaxed);
        gcThread.join();
    }

    // Asynchronously delete the given object.
    //
    // A call to manager.synchronize() is guaranteed before the memory is
    // deleted.
    //
    // Non-blocking. Places the given pointer in a shared collection, which is
    // periodically cleared by the GC thread.
    void discard(RcuManager &manager, T *t) {
        bool success;

        do {
            // We use RCU protection to prevent the ABA problem.
            manager.readLock();
            // Synchronizes-with committing CAS-es, to make sure that we read
            // the updated next pointer.
            T *oldHead = head.load(std::memory_order_acquire);
            t->getGcNext().store(oldHead, std::memory_order_relaxed);
            // Synchronizes-with reading next pointers, so that they always
            // get the new value.
            //
            // This CAS is not subject to the ABA problem due to RCU
            // protection. If the head at this CAS is equal to oldHead, either:
            //
            //   1. It has not been modified since we read it. This is fine.
            //   2. Someone popped it off the stack, and then it eventually
            //      got pushed back on.
            //
            // We always delete nodes after popping them, so in case (2) we
            // must at some point have deleted oldHead. But this explicitly
            // violates our RCU guarantees, so case (2) is impossible.
            success = head.compare_exchange_weak(oldHead, t,
                    std::memory_order_release);
            manager.readUnlock();
        } while (!success);
    }

private:
    static void gcLoop(GarbageCollector *gc, RcuManager &manager) {
        using namespace std::literals;

        manager.registerCurrentThread();

        // Loop until someone tells us to stop.
        //
        // We don't need any particular memory ordering guarantees on
        // gc->done; we just need to eventually read any updates to it.
        while (!gc->done.load(std::memory_order_relaxed)) {
            T *oldHead;
            bool success;

            {
                do {
                    // See the comments in discard for an explanation of how
                    // this is synchronized.
                    manager.readLock();
                    oldHead = gc->head.load(std::memory_order_acquire);

                    if (oldHead == nullptr) break;

                    success = gc->head.compare_exchange_weak(oldHead, nullptr,
                            std::memory_order_release);
                    manager.readUnlock();
                } while(!success);
            }

            // If the stack was empty, sleep for a while and then poll it
            // again.
            if (oldHead == nullptr) {
                std::this_thread::sleep_for(1ms);
                continue;
            }

            // We've acquired the entire list! Now we need to synchronize,
            // then free it.

            manager.synchronize();

            T *cur = oldHead;
            while (cur != nullptr) {
                T *next = cur->getGcNext().load(std::memory_order_relaxed);
                cur->getGcNext().store(nullptr, std::memory_order_relaxed);
                delete cur;
                cur = next;
            }
        }

        manager.unregisterCurrentThread();
    }

    std::thread gcThread;
    // Put padding around head to prevent false sharing.
    std::byte padding1[CACHE_LINE_BYTES];
    std::atomic<T *> head;
    std::byte padding2[CACHE_LINE_BYTES];
    std::atomic<bool> done;
};
