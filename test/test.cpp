#include <atomic>
#include <iostream>
#include <chrono>
#include <thread>
#include <vector>

#include "RCU.hh"
#include "RcuList.hh"

void die() {
    std::cerr << "Test failed!\n";
    exit(1);
}

void require(bool b) {
    if (!b) die();
}

void threadFunction(RcuManager &manager) {
    using namespace std::literals;
    manager.registerCurrentThread();
    std::this_thread::sleep_for(1ms);
    manager.unregisterCurrentThread();
}

void modify(std::atomic<bool> &go, RcuManager &manager, RcuList &list,
            std::uint64_t lower, std::uint64_t upper) {
    while (!go.load(std::memory_order_relaxed) ) {}

    manager.registerCurrentThread();

    for (std::uint64_t i = lower; i < upper; ++i) {
        list.pop(manager, i);
    }

    for (std::uint64_t i = lower; i < upper; ++i) {
        list.pop(manager);
    }

    manager.unregisterCurrentThread();
}

const std::uint64_t lower = 10000;
const std::uint64_t upper = 20000;

void search(std::atomic<bool> &go, RcuManager &manager, RcuList &list) {
    while (!go.load(std::memory_order_relaxed) ) {}

    manager.registerCurrentThread();

    std::uint64_t count = 0;

    for (std::uint64_t i = 0; i < upper; ++i) {
        count += list.search(manager, i);
    }

    std::cout << "fraction: " << (double)count / upper << ".\n";

    manager.unregisterCurrentThread();
}

int main(void) {
    RcuManager manager;

    require(manager.registerCurrentProcess());
    manager.registerCurrentThread();

    std::vector<std::thread> threads;

    for (int i = 0; i < 8; ++i) {
        threads.emplace_back(threadFunction, std::ref(manager));
    }

    for (auto &thread: threads) {
        thread.join();
    }

    RcuList list;

    list.pop(manager, 0);
    list.pop(manager, 1);
    list.pop(manager, 2);
    list.pop(manager, 3);

    require(list.search(manager, 0));
    require(list.search(manager, 1));
    require(list.search(manager, 2));
    require(list.search(manager, 3));

    require(!list.search(manager, 4));
    require(!list.search(manager, 5));
    require(!list.search(manager, 6));
    require(!list.search(manager, 7));

    require(list.pop(manager) == 3);
    require(list.pop(manager) == 2);
    require(list.pop(manager) == 1);
    require(list.pop(manager) == 0);

    // Now for the multithreaded test. I'll push the numbers upper through
    // upper + 10000, and make sure that they all stay there while other
    // threads modify and search the list:
    
    for (uint64_t i = upper; i < upper + 10000; ++i) {
        list.pop(manager, i);
    }

    std::atomic<bool> go(false);
    threads = std::vector<std::thread>();

    threads.emplace_back(modify, std::ref(go), std::ref(manager), std::ref(list),
                         0, lower);
    threads.emplace_back(modify, std::ref(go), std::ref(manager), std::ref(list),
                         lower, upper);

    for (int i = 0; i < 8; ++i) {
        threads.emplace_back(search, std::ref(go), std::ref(manager), std::ref(list));
    }

    // GO!
    go.store(true);

    // Check that everything's still there.
    for (uint64_t i = upper; i < upper + 10000; ++i) {
        require(list.search(manager, i));
    }

    for (auto &thread: threads) {
        thread.join();
    }


    manager.unregisterCurrentThread();

    return 0;
}
