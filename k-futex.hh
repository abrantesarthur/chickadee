#ifndef CHICKADEE_K_FUTEX_HH
#define CHICKADEE_K_FUTEX_HH

#include "k-waitstruct.hh"

// access a global futex hastable keyed by physical address.
// Each entry is a wait queue of processes waiting on that address

#define FUTEX_TABLE_SIZE 50

struct futex_entry {
    uintptr_t addr_ = 0;          // physical address
    wait_queue wq_;               // processes that care about the value at 'addr'
};

struct futex_table {
    spinlock lock_;                     // protects access to the table

private:
    // TODO: future improvement: use hashtable instead
    futex_entry table_[FUTEX_TABLE_SIZE]; 
};


#endif


