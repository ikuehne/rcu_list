#include "RCU.hh"

static thread_local std::list<PerThreadEntry *>::iterator spotInList;

static int membarrier(int cmd, int flags) {
    return syscall(__NR_membarrier, cmd, flags);
}

bool RcuManager::registerCurrentProcess(void) {
    auto ret = membarrier(MEMBARRIER_CMD_QUERY, 0);

    if (ret < 0) {
        return false;
    }

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
    // fails at all, it must fail the first time.
    ret = membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0);

    if (ret < 0) {
        perror("membarrier");
        return false;
    }

    return true;
}

void RcuManager::registerCurrentThread(void) {
    std::unique_lock lock(mutex);
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
    // This doesn't seem right to me, but it's there in the paper and
    // I'm not one to mess with barriers...
    std::atomic_thread_fence(std::memory_order_relaxed);
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
            bool ongoing = (entryGP & NESTING_MASK)
                        && ((entryGP ^ newGracePeriod) & GP_COUNTER_MASK);
            if (!ongoing) break;
            std::this_thread::sleep_for(1ms);
        }
    }
}
