#include "RCU.hh"

namespace rcu {

// Keep track of where the thread is in the registry.
//
// This lets us deregister threads in unregisterCurrentThreads.
static thread_local std::list<PerThreadEntry *>::iterator spotInList;

// Wrap the membarrier syscall.
//
// We use three membarrier commands:
//
//  - MEMBARRIER_CMD_QUERY: returns <0 for an error, or a bitmask of
//    supported commands otherwise.
//
//  - MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED: register the process's intent
//    to recieve expedited membarriers. Once this is called, future calls to
//    MEMBARRIER_CMD_PRIVATE_EXPEDITED will force all threads in this process
//    to execute a full memory barrier.
//
//  - MEMBARRIER_CMD_PRIVATE_EXPEDITED: force all threads in this process to
//    execute a full memory barrier.
//
static int membarrier(int cmd, int flags) {
    return syscall(__NR_membarrier, cmd, flags);
}

void membarrierAllThreads(void) {
    // According to the docs, we don't need to check for errors here: if the
    // call in registerCurrentProcess succeeds, all subsequent calls should
    // succeed.
    membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0);
}

bool registerCurrentProcess(void) {
    // Query membarrier for supported operations.
    auto ret = membarrier(MEMBARRIER_CMD_QUERY, 0);

    // Check whether query failed.
    if (ret < 0) {
        return false;
    }

    // Check whether the commands we'll use are supported.
    if (!(ret & MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED)) {
        return false;
    }
    
    if (!(ret & MEMBARRIER_CMD_PRIVATE_EXPEDITED)) {
        return false;
    }

    // Register our intent to receive expedited membarriers.
    ret = membarrier(MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED, 0);

    if (ret < 0) {
        return false;
    }

    // Try it out once to test that it works: the docs specify that if it
    // fails at all, it must fail the first time. This call means we don't
    // have to worry about errors on future calls.
    ret = membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0);

    if (ret < 0) {
        return false;
    }

    return true;
}

void registerCurrentThread(void) {
    std::unique_lock lock(mutex);
    // Our gracePeriodCounter starts at 0.
    threadLocalEntry.gracePeriodCounter.store(0,
            std::memory_order_relaxed);
    entries.push_back(&threadLocalEntry);
    // Remember where we are in the list. This is only used for
    // unregistration.
    spotInList = entries.end();
    spotInList--;
}

void unregisterCurrentThread(void) {
    std::unique_lock lock(mutex);
    entries.erase(spotInList);
}

static inline void toggleAndWaitForThreads(void);
void synchronize(void) {
    std::unique_lock lock(mutex);
    // Wait until all reader threads have run a full memory barrier. In effect
    // this synchronizes-with the notional "memory barriers" in readLock and
    // readUnlock.
    membarrierAllThreads();
    // Toggle the GP bit and wait until every thread has either entered a
    // quiescent state, or matches the new GP bit. If a thread has entered a
    // quiescent state, then we're good as far as that thread is concerned.
    // However, if a thread only has a matching GP bit, then one of two things
    // may have happened:
    //
    //  - They may have read the new GP bit. In that case, we're fine.
    //  - They may have read an old GP bit during the last call to
    //    synchronize.
    //
    // To rule out the latter case...
    toggleAndWaitForThreads();
    // we toggle the bit again and perform the same check.
    toggleAndWaitForThreads();
    // Similar to the membarrierAllThreads above. This one ensures that reader
    // threads' reads of shared data happen-before we return.
    membarrierAllThreads();
}

// Toggle the GP bit and wait until we observe one of two things for every
// thread:
//  - They are in a quiescent state.
//  - Their thread-local GP bit matcches the global one.
void toggleAndWaitForThreads(void) {
    using namespace std::literals;

    auto oldGracePeriod = globalGracePeriod.load(
            std::memory_order_relaxed);
    auto newGracePeriod = oldGracePeriod ^ GP_COUNTER_MASK;
    globalGracePeriod.store(newGracePeriod,
            std::memory_order_relaxed);

    for (const auto &entry: entries) {
        while (true) {
            auto entryGP = entry->gracePeriodCounter.load(
                    std::memory_order_relaxed);
            if (!(entryGP & NESTING_MASK))
                break;
            if ((entryGP & GP_COUNTER_MASK) == (newGracePeriod & GP_COUNTER_MASK))
                break;
            std::this_thread::sleep_for(1ms);
        }
    }
}

}
