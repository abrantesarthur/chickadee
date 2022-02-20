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
    // TODO: synchronize access to state_
    p_->pstate_ = proc::ps_blocked;
    // make this waiter's and process' wq point to same wait_queue
    p_->wq_ = &wq;
    wq_ = &wq;
    // add this waiter (i.e., the process) to wait queue
    wq.q_.push_front(this);
}

// block()
//      yield if the current process is blocked and wake it otherwise.
inline void waiter::block() {
    assert(p_ == current());
    // TODO: synchronize access to pstate_
    if(p_->pstate_ == proc::ps_blocked) {
        p_->yield();
    }
    // we will reach this only if process is not blocked
    clear();
}

// clear()
//      wake process, clear its waiter, and remove it from wait queue
inline void waiter::clear() {
    spinlock_guard g(wq_->lock_);
    // TODO: should we not wake the process just at the end of clearing? What bad things could happen if process is scheduled again before we're done clearing?
    wake();
    p_->wq_ = nullptr;
    // TODO: is this necessary?
    p_ = nullptr;
    if(links_.is_linked()) {
        wq_->q_.erase(this);
    }
    // TODO: is this necessary?
    wq_ = nullptr;
}

// wake()
//      set process to runnable and schedule it to run
inline void waiter::wake() {
    assert(wq_->lock_.is_locked());
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

#endif
