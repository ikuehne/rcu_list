#include "RCU.hh"

static thread_local std::list<PerThreadEntry *>::iterator spotInList;

static int membarrier(int cmd, int flags) {
    return syscall(__NR_membarrier, cmd, flags);
}

bool RcuManager::registerCurrentProcess(void) {
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
        perror("membarrier");
        return false;
    }

    return true;
}

void RcuManager::registerCurrentThread(void) {
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

void RcuManager::unregisterCurrentThread(void) {
    std::unique_lock lock(mutex);
    entries.erase(spotInList);
}

void RcuManager::synchronize(void) {
    std::unique_lock lock(mutex);
    membarrierAllThreads();
    toggleAndWaitForThreads();
    toggleAndWaitForThreads();
    membarrierAllThreads();
}

void RcuManager::membarrierAllThreads(void) {
    // According to the docs, we don't need to check for errors here: if the
    // call in registerCurrentProcess succeeds, all subsequent calls should
    // succeed.
    membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0);
}

void RcuManager::toggleAndWaitForThreads(void) {
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
