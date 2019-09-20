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

thread_local PerThreadEntry threadLocalEntry;
thread_local std::list<PerThreadEntry *>::iterator spotInList;

static int membarrier(int cmd, int flags) {
    return syscall(__NR_membarrier, cmd, flags);
}


class RcuManager {
public:
    RcuManager()
        : globalGracePeriod(1), entries(), mutex() {}

    bool registerCurrentProcess(void) {
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

    void registerCurrentThread(void) {
        std::unique_lock lock(mutex);
        threadLocalEntry.gracePeriodCounter.store(0,
                std::memory_order_relaxed);
        entries.push_back(&threadLocalEntry);
        spotInList = entries.end();
        spotInList--;
    }

    void unregisterCurrentThread(void) {
        std::unique_lock lock(mutex);
        entries.erase(spotInList);
    }

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

    inline void readUnlock(void) const {
        auto tmp = threadLocalEntry.gracePeriodCounter.load(
                std::memory_order_relaxed);
        threadLocalEntry.gracePeriodCounter.store(tmp - 1,
                std::memory_order_relaxed);
    }

	inline void synchronize(void) {
        std::unique_lock lock(mutex);
        membarrierAllThreads();
        toggleAndWaitForThreads();
        // This doesn't seem right to me, but it's there in the paper and
        // I'm not one to mess with barriers...
        std::atomic_thread_fence(std::memory_order_relaxed);
        toggleAndWaitForThreads();
        membarrierAllThreads();
	}

private:
	inline void membarrierAllThreads(void) {
		membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0);
	}

    inline void toggleAndWaitForThreads(void) {
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

    std::atomic<std::uint64_t> globalGracePeriod;
    std::list<PerThreadEntry *> entries;
    std::mutex mutex;
};
