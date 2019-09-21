#pragma once

#include <cassert>
#include "RCU.hh"

struct RcuListNode {
    std::atomic<RcuListNode *> next;
    std::uint64_t data;

    std::atomic<RcuListNode *> &getGcNext(void) {
        return next;
    }
};

class RcuList {
public:
    RcuList() : head(nullptr), gc() {}

    void joinGC(void) {
        gc.join();
    }

    uint64_t pop() {
        RcuListNode *oldHead;
        bool success;

        do {
            // Use RCU for ABA protection.
            rcu::readLock();
            // This load synchronizes-with committing CAS-es, so that we
            // always read the updated next pointer.
            oldHead = head.load(std::memory_order_acquire);
            if (oldHead == nullptr) break;
            RcuListNode *newHead = oldHead->next.load(
                std::memory_order_relaxed);
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
            success = head.compare_exchange_weak(oldHead, newHead, 
                std::memory_order_release);
            rcu::readUnlock();
        } while (!success);

        if (oldHead == nullptr) {
            return 0xDEAD;
        }

        assert(!search(oldHead->data));

        uint64_t result = oldHead->data;
        gc.discard(oldHead);
        return result;
    }

    void push(std::uint64_t data) {
        auto newNode = new RcuListNode {
            nullptr,
            data
        };

        bool success;
        do {
            // See the comments in pop for an explanation of how
            // this is synchronized.
            rcu::readLock();
            RcuListNode *old = head.load(std::memory_order_acquire);
            newNode->next.store(old, std::memory_order_relaxed);
            success = head.compare_exchange_weak(old, newNode,
                std::memory_order_release);
            rcu::readUnlock();
        } while (!success);
    }

    bool search(std::uint64_t data) {
        rcu::readLock();

        for (RcuListNode *cur = head.load(std::memory_order_acquire);
             cur != nullptr;
             cur = cur->next.load(std::memory_order_acquire)) {
            if (cur->data == data) {
                rcu::readUnlock();
                return true;
            }
        }

        rcu::readUnlock();

        return false;
    }

private:
    std::atomic<RcuListNode *> head;
    char padding[rcu::CACHE_LINE_BYTES];
    rcu::GarbageCollector<RcuListNode> gc;
};
