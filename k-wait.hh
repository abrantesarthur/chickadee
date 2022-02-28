#ifndef CHICKADEE_K_WAIT_HH
#define CHICKADEE_K_WAIT_HH
#include "kernel.hh"
#include "k-waitstruct.hh"

// k-wait.hh
//    Defines `waiter` and `wait_queue` member functions.
//    `k-waitstruct.hh` defines the `waiter` and `wait_queue` types.
//    (Separating the structures and functions into different header files
//    avoids problems with circular dependencies.)


inline waiter::waiter() {
}

inline waiter::~waiter() {
    // optional error-checking code
}

// prepare(wq)
//      set waiter process to blocked and add it to the waitqueue
inline void waiter::prepare(wait_queue& wq) {
    spinlock_guard g(wq.lock_);
    p_ = current();
    p_->pstate_ = proc::ps_blocked;
    wq_ = &wq;
    // add this waiter (i.e., the process) to wait queue
    wq.q_.push_front(this);
}

// block()
//      yield if the current process is blocked and wake it otherwise.
inline void waiter::block() {
    assert(p_ == current());
    if(p_->pstate_ == proc::ps_blocked) {
        p_->yield();
    }
    // we will reach this only if process is not blocked
    clear();
}

// clear()
//      remove waiter from wait queue and wake its corresponding process
inline void waiter::clear() {
    spinlock_guard g(wq_->lock_);
    if(links_.is_linked()) {
        wq_->q_.erase(this);
    }
    // wake process after removing it from wait queue
    wake();
}

// wake()
//      set process to runnable and schedule it to run
inline void waiter::wake() {
    p_->wake();
}


// waiter::block_until(wq, predicate)
//    Block on `wq` until `predicate()` returns true.
template <typename F>
inline void waiter::block_until(wait_queue& wq, F predicate) {
    while (true) {
        prepare(wq);
        if (predicate()) {
            break;
        }
        block();
    }
    clear();
}

// waiter::block_until(wq, predicate, lock, irqs)
//    Block on `wq` until `predicate()` returns true. The `lock`
//    must be locked; it is unlocked before blocking (if blocking
//    is necessary). All calls to `predicate` have `lock` locked,
//    and `lock` is locked on return.
template <typename F>
inline void waiter::block_until(wait_queue& wq, F predicate,
                                spinlock& lock, irqstate& irqs) {
    while (true) {
        prepare(wq);
        if (predicate()) {
            break;
        }
        lock.unlock(irqs);
        block();
        irqs = lock.lock();
    }
    clear();
}

// waiter::block_until(wq, predicate, guard)
//    Block on `wq` until `predicate()` returns true. The `guard`
//    must be locked on entry; it is unlocked before blocking (if
//    blocking is necessary) and locked on return.
template <typename F>
inline void waiter::block_until(wait_queue& wq, F predicate,
                                spinlock_guard& guard) {
    block_until(wq, predicate, guard.lock_, guard.irqs_);
}

// wait_queue::wake_all()
//    Lock the wait queue, then clear it by waking all waiters.
inline void wait_queue::wake_all() {
    spinlock_guard guard(lock_);
    while (auto w = q_.pop_front()) {
        w->wake();
    }
}

// wait_queue::wake_proc(p)
//    look for waiter with process 'p' and wake it found
inline void wait_queue::wake_proc(proc* p) {
    spinlock_guard guard(lock_);
    waiter* w = q_.front();
    while(w) {
        if(w->p_ == p) {
            q_.erase(w);
            w->wake();
            return;
        }
        w = q_.next(w);
    }
}

#endif
