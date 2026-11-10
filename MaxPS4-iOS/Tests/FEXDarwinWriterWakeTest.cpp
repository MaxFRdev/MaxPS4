// Exercise the pinned mutex with its real Darwin wait/wake implementation.
#include <FEXCore/Utils/WritePriorityMutex.h>
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

int main() {
    FEXCore::Utils::WritePriorityMutex::Mutex mutex;
    std::atomic<bool> start {false}, valid {true};
    unsigned value = 0;
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < 12; ++i) {
        workers.emplace_back([&, i] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            unsigned previous = 0;
            for (unsigned n = 0; n < 10000; ++n) {
                if (i < 4) {
                    mutex.lock();
                    ++value;
                    if ((n & 63) == 0) std::this_thread::yield();
                    mutex.unlock();
                } else {
                    mutex.lock_shared();
                    if (value < previous) valid = false;
                    previous = value;
                    if ((n & 63) == 0) std::this_thread::yield();
                    mutex.unlock_shared();
                }
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& worker : workers) worker.join();
    if (value != 40000 || !valid.load()) return 1;
    std::puts("PASS Darwin writer wake: 40000 writes, 80000 reads, all threads joined");
}
