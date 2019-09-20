#pragma once

#include <cassert>
#include "RCU.hh"

struct RcuListNode {
    std::atomic<RcuListNode *> next;
    std::uint64_t data;
};

class RcuList {
public:
    RcuList() : head(nullptr) {}

    bool remove(RcuManager &manager, std::uint64_t data) {
        while (true) {
            manager.readLock();

            std::atomic<RcuListNode *> *prev = &head;
            RcuListNode *cur = head.load(std::memory_order_relaxed);

            // Invariant: prev->load() == cur (modulo changes by other threads,
            // which we will detect with CAS).
            for (; cur != nullptr;
                 cur = cur->next.load(std::memory_order_relaxed)) {
                if (cur->data == data) {
                    break;
                }
                prev = &cur->next;
            }

            if (cur == nullptr) {
                manager.readUnlock();
                return false;
            }

            assert(cur->data == data);

            if (cur->data == data) {
                RcuListNode *next = cur->next.load(
                        std::memory_order_relaxed);
                bool success =
                    prev->compare_exchange_weak(cur, next,
                        std::memory_order_release);
                if (success) {
                    manager.readUnlock();
                    manager.synchronize();
                    delete cur;
                    return true;
                } else {
                    manager.readUnlock();
                }
            }
        }
    }

    void insert(RcuManager &manager, std::uint64_t data) {
        auto newNode = new RcuListNode {
            nullptr,
            data
        };

        RcuListNode *old;
        bool success;
        do {
            manager.readLock();
            old = head.load(std::memory_order_relaxed);
            newNode->next = old;
            success = head.compare_exchange_weak(old, newNode,
                std::memory_order_release);
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
};
