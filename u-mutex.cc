#include "u-lib.hh"

/**
 * mutex
 * init
 * trylock
 * destroy 
 */

void mutex::lock() {
    int c = compare_exchange_strong(&futex_, 0, 1);

    // if we grabbed the lock, return without blocking
    if(c == 0) return;

    // othwerwise, block until lock is free
    do {
        // signal that we are waiting for the lock
        if(c == 2 || compare_exchange_strong(&futex_, 1, 2) != 0) {
            // block until the lock is freed
            sys_futex(&futex_, FUTEX_WAIT, 2);
        }
        // we get here if futex_ becomes 0. Hence, we try grabbing it again.
    } while((c = compare_exchange_strong(&futex_, 0, 2)) != 0);
    
    // wet get here if we grabbed the lock in the while loop
}

// mutex::trylock()
        // try grabbing the lock without blocking. Returns true if success
bool mutex::trylock() {
    return compare_exchange_strong(&futex_, 0, 1) == 0;
}

void mutex::unlock() {
    if(futex_.fetch_sub(1) != 1) {
        futex_.store(0);
        sys_futex(&futex_, FUTEX_WAKE, 1);
    }
}

int mutex::compare_exchange_strong(std::atomic<int>* futex, int expected, int desired) {
    int* e = &expected;
    // if value at futex is 'expected', replace it with 'desired'
    // otherwise, load 'e' with contained value
    std::atomic_compare_exchange_strong(futex, e, desired);
    return *e;
}
