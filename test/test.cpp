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

void threadFunction() {
    using namespace std::literals;
    rcu::registerCurrentThread();
    std::this_thread::sleep_for(1ms);
    rcu::unregisterCurrentThread();
}

void modify(std::atomic<bool> &go, RcuList &list,
            std::uint64_t lower, std::uint64_t upper) {
    while (!go.load(std::memory_order_relaxed) ) {}

    rcu::registerCurrentThread();

    for (std::uint64_t i = lower; i < upper; ++i) {
        list.push(i);
    }

    for (std::uint64_t i = lower; i < upper; ++i) {
        list.pop();
    }

    rcu::unregisterCurrentThread();
}

const std::uint64_t lower = 10000;
const std::uint64_t upper = 20000;

void search(std::atomic<bool> &go, RcuList &list) {
    while (!go.load(std::memory_order_relaxed) ) {}

    rcu::registerCurrentThread();

    std::uint64_t count = 0;

    for (std::uint64_t i = 0; i < upper; ++i) {
        count += list.search(i);
    }

    std::cout << "fraction: " << (double)count / upper << ".\n";

    rcu::unregisterCurrentThread();
}

int main(void) {
    require(rcu::registerCurrentProcess());
    rcu::registerCurrentThread();

    std::vector<std::thread> threads;

    for (int i = 0; i < 8; ++i) {
        threads.emplace_back(threadFunction);
    }

    for (auto &thread: threads) {
        thread.join();
    }

    RcuList list;

    list.push(0);
    list.push(1);
    list.push(2);
    list.push(3);

    require(list.search(0));
    require(list.search(1));
    require(list.search(2));
    require(list.search(3));

    require(!list.search(4));
    require(!list.search(5));
    require(!list.search(6));
    require(!list.search(7));

    require(list.pop() == 3);
    require(list.pop() == 2);
    require(list.pop() == 1);
    require(list.pop() == 0);

    // Now for the multithreaded test. I'll push the numbers upper through
    // upper + 10000, and make sure that they all stay there while other
    // threads modify and search the list:
    
    for (uint64_t i = upper; i < upper + 10000; ++i) {
        list.push(i);
    }

    std::atomic<bool> go(false);
    threads = std::vector<std::thread>();

    threads.emplace_back(modify, std::ref(go), std::ref(list),
                         0, lower);
    threads.emplace_back(modify, std::ref(go), std::ref(list),
                         lower, upper);

    for (int i = 0; i < 8; ++i) {
        threads.emplace_back(search, std::ref(go), std::ref(list));
    }

    // GO!
    go.store(true);

    // Check that everything's still there.
    for (uint64_t i = upper; i < upper + 10000; ++i) {
        require(list.search(i));
    }

    for (auto &thread: threads) {
        thread.join();
    }

    rcu::unregisterCurrentThread();

    list.joinGC();

    return 0;
}
