#ifndef CHICKADEE_K_FUTEX_HH
#define CHICKADEE_K_FUTEX_HH

#include "k-waitstruct.hh"

// access a global futex hastable keyed by physical address.
// Each entry is a wait queue of processes waiting on that address

#define FUTEX_TABLE_SIZE 50

struct futex_entry {
    inline futex_entry(int* kptr) : kptr_(kptr) {
    }

    int* kptr_;                                 // physical address
    wait_queue wq_;                             // processes that care about the value at 'addr'
    list_links link_;
};

struct futex_table {
    spinlock lock_;                             // protects access to the table

    wait_queue* get_wait_queue(int* kptr);
    wait_queue* create_wait_queue(int* ktpr);
    int wake_processes(int* kptr, int count);

private:
    // TODO: future improvement: use hashtable instead
    list<futex_entry, &futex_entry::link_> entries_;
};


#endif


