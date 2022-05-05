#include "u-lib.hh"

extern uint8_t end[];

mutex::mutex() {
    // map futex to shared memory segment so child has access to it when forking
    int* shared_addr = reinterpret_cast<int*>(
        round_up(reinterpret_cast<uintptr_t>(end), PAGESIZE)
    );
    shmid_ = shmget(IPC_PRIVATE);
    shmaddr_ = shmat(shmid_, reinterpret_cast<const void*>(shared_addr));
    assert_eq(shmaddr_, shared_addr);

    // set futex
    atom_ = reinterpret_cast<std::atomic<int>*>(shmaddr_);
    std::atomic_store(atom_, 0);
}

void mutex::destroy() {
    shmdt(shmaddr_);
}

void mutex::lock() {
    int c = compare_exchange_strong(atom_, 0, 1);

    // if we grabbed the lock, return without blocking
    if(c == 0) return;

    // othwerwise, block until lock is free
    do {
        // signal that we are waiting for the lock
        if(c == 2 || compare_exchange_strong(atom_, 1, 2) != 0) {
            // block until the lock is freed
            sys_futex(atom_, FUTEX_WAIT, 2);
        }
        // we get here if atom_ becomes 0. Hence, we try grabbing it again.
    } while((c = compare_exchange_strong(atom_, 0, 2)) != 0);
    
    // wet get here if we grabbed the lock in the while loop
}

void mutex::unlock() {
    if(std::atomic_fetch_sub(atom_, 1) != 1) {
        std::atomic_store(atom_, 0);
        sys_futex(atom_, FUTEX_WAKE, 1);
    }
}