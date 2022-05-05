#ifndef CHICKADEE_U_MUTEX_HH
#define CHICKADEE_U_MUTEX_HH
#include <atomic>

// TODO: add this to its own file
// reference: https://eli.thegreenplace.net/2018/basics-of-futexes
struct mutex {
    inline mutex() : futex_(0) {
        // TODO: map memory
    }
    void lock();
    void unlock();
    bool trylock();

private:
    // 0 means unlocked
    // 1 means locked, no waiters
    // 2 means locked, with waiters
    std::atomic<int> futex_;

    int compare_exchange_strong(std::atomic<int>* futex, int expected, int desired);
};

#endif
