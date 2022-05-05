#ifndef CHICKADEE_U_MUTEX_HH
#define CHICKADEE_U_MUTEX_HH
#include <atomic>

// TODO: add this to its own file
// reference: https://eli.thegreenplace.net/2018/basics-of-futexes
struct mutex {
    mutex();
    void destroy();
    void lock();
    void unlock();
    inline bool trylock();

private:
    // 0 means unlocked
    // 1 means locked, no waiters
    // 2 means locked, with waiters
    std::atomic<int>* futex_;

    int shmid_;             // shared memory id
    void* shmaddr_;         // shared memory address


    inline int compare_exchange_strong(std::atomic<int>* futex, int expected, int desired);
};


// mutex::trylock()
        // try grabbing the lock without blocking. Returns true if success
inline bool mutex::trylock() {
    return compare_exchange_strong(futex_, 0, 1) == 0;
}

inline int mutex::compare_exchange_strong(std::atomic<int>* futex, int expected, int desired) {
    int* e = &expected;
    // if value at futex is 'expected', replace it with 'desired'
    // otherwise, load 'e' with contained value
    std::atomic_compare_exchange_strong(futex, e, desired);
    return *e;
}


#endif
