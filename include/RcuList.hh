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
    RcuList(RcuManager &manager) : head(nullptr), gc(manager) {}

    void joinGC(void) {
        gc.join();
    }

    uint64_t pop(RcuManager &manager) {
        RcuListNode *oldHead;
        bool success;

        do {
            manager.readLock();
            oldHead = head.load(std::memory_order_acquire);
            if (oldHead == nullptr) break;
            RcuListNode *newHead = oldHead->next.load(
                std::memory_order_acquire);
            success = head.compare_exchange_weak(oldHead, newHead, 
                std::memory_order_acq_rel);
            manager.readUnlock();
        } while (!success);

        if (oldHead == nullptr) {
            return 0xDEAD;
        }

        assert(!search(manager, oldHead->data));

        uint64_t result = oldHead->data;
        gc.discard(manager, oldHead);
        return result;
    }

    void push(RcuManager &manager, std::uint64_t data) {
        auto newNode = new RcuListNode {
            nullptr,
            data
        };

        bool success;
        do {
            manager.readLock();
            RcuListNode *old = head.load(std::memory_order_acquire);
            newNode->next = old;
            success = head.compare_exchange_weak(old, newNode,
                std::memory_order_acq_rel);
            manager.readUnlock();
        } while (!success);
    }

    bool search(RcuManager &manager, std::uint64_t data) {
        manager.readLock();

        for (RcuListNode *cur = head.load(std::memory_order_relaxed);
             cur != nullptr;
             cur = cur->next.load(std::memory_order_relaxed)) {
            if (cur->data == data) {
                manager.readUnlock();
                return true;
            }
        }

        manager.readUnlock();

        return false;
    }

private:
    std::atomic<RcuListNode *> head;
    char padding[CACHE_LINE_BYTES];
    GarbageCollector<RcuListNode> gc;
};
