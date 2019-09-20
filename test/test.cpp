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

    list.insert(manager, 0);
    list.insert(manager, 1);
    list.insert(manager, 2);
    list.insert(manager, 3);

    require(list.search(manager, 0));
    require(list.search(manager, 1));
    require(list.search(manager, 2));
    require(list.search(manager, 3));

    require(!list.search(manager, 4));
    require(!list.search(manager, 5));
    require(!list.search(manager, 6));
    require(!list.search(manager, 7));

    require(!list.remove(manager, 4));
    require(!list.remove(manager, 5));
    require(!list.remove(manager, 6));
    require(!list.remove(manager, 7));

    require(list.remove(manager, 0));
    require(list.remove(manager, 1));
    require(list.remove(manager, 2));
    require(list.remove(manager, 3));

    require(!list.remove(manager, 0));
    require(!list.remove(manager, 1));
    require(!list.remove(manager, 2));
    require(!list.remove(manager, 3));

    manager.unregisterCurrentThread();

    return 0;
}
