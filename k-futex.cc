#include "kernel.hh"
#include "k-wait.hh"

// global futex table
futex_table ftable;

// futex_table::wake_processes(kptr, count)
//      wakes 'count' processes sleeping on the wait queue associated
//      with 'kptr' addresses. Returns number of processes awaken.
//      If wait_queue is non existent, or has no processes, return 0.
int futex_table::wake_processes(int* kptr, int count) {
    assert(lock_.is_locked());

    // look for wait_queue of processess
    wait_queue* wq = get_wait_queue(kptr);

    // if not found, tell caller that 0 processes were awoken
    if(!wq) return 0;
    
    // otherwise, try waking 'count' processes;
    return wq->wake_some(count);
}

wait_queue* futex_table::get_wait_queue(int* kptr) {
    assert(lock_.is_locked());

    // look for entry whose containing 'paddr'
    futex_entry* entry = entries_.front();
    while(entry) {
        if(entry->kptr_ == kptr) {
            return &entry->wq_;
        }
        entry = entries_.next(entry);
    }

    return nullptr;
}

wait_queue* futex_table::create_wait_queue(int* kptr) {
    assert(lock_.is_locked());
    
    // try allocating a new entry
    futex_entry* new_entry = knew<futex_entry>(kptr);
    if(!new_entry) return nullptr;

    // add it to the list
    entries_.push_front(new_entry);

    // return its wait_queue
    return &new_entry->wq_;
}