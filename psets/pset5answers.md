# CS 161 Problem Set 5 Answers

Leave your name out of this file. Put collaboration notes and credit in
`pset5collab.md`.

## Answers to written questions

### Synchronization invariants

#### lock interleavings

- If `ptable_lock` and `pgtable_lock` are simultaneously locked, `ptable_lock` must always be locked first.

  This is so we avoid deadlocking issues due to `proc::syscall_fork` grabbing `ptable_lock` first.

- If `ptable_lock` and `proc_group::lock_` are simultaneously locked, `ptable_lock` must always be locked first.

  This is so we avoid deadlocking issues due to `proc::syscall_exit` grabbing `ptable_lock` first

#### proc_group::children\_

- Accessing `proc_group::children_` requires the `ptable_lock`

  Remember that `ptable_lock` protects everything related to process hierarchy.

#### proc_group::who_exited\_

- Accessing `proc_group::who_exited_` requires the `proc_group::lock_`

  This prevents racing conditions in `syscall_exit`, if two threads try to exit at the same time.

- It starts as `nullptr` and can only modified once by the thread who calls `sys_exit` first

  This ensures that the thread who called `sys_exit` sets its own state to `ps_exiting` after all other threads have exited, as opposed to havign its state set to `ps_exiting` in `cpustate::schedule`.

## Project

### Goal

The goal of my project was to enable userspace code to synchronize threads and processes efficiently and with minimum kernel involvement. I accomplished this by using mutexes, futexes, and shared memory segments.

### Design

The entrypoint for an userspace code trying to accomplish synchronization is the `u-mutex.hh` file. It defines a `struct mutex` definition that users can use to establish critical areas. This mutex implements locking and unlocking primitives with the help of a futex and of shared memory segments. More specifically, the lock (i.e., `mutex::atom_`) is a pointer to a shared memory region which children have access to after forks so the locking semantics work. If a proc tries grabbing the lock when it's already held by another process, it uses a futex to sleep. Then, when the process having the lock decides to release it, it wakes up the sleeping child through the futex. This works because, although the two processes have separate address spaces, they both have mappings to the same underlying `mutex::atom_`.

### Code

- `u-mutex.hh` and `u-mutex.cc`

  contains user-accessible mutex based on futex

- `k-futex.hh` and `k-futex.cc`

  contain definitions of a futex table with mappings from a futex (i.e., physical address) and a wait queue of processes that care about it

- `kernel.hh` and `k-proc.cc`

  `proc_group`'s shared memory segment definition and implementations

- `u-lib.hh`, `u-shm.hh`, and `u-shm.cc`

  contains entry points to the `syscall_futex`, `syscall_shmget`, `syscall_shmat`, and `syscall_shmdt` system calls

- `kernel.cc`

  contains implementations of the `syscall_futex`, `syscall_shmget`, `syscall_shmat`, and `syscall_shmdt` system calls.

  It also contains modifications in `syscall_fork` and `syscall_exit` to support shared memory segments.

- `p-testmutex.cc`
  mutex testing code
- `p-testfutex.cc`
  futex testing code
- `p-testshm.cc`
  shared memory testing code

### Challenges

- modifying `syscall_fork` and `syscall_exit` to accomodate shared memory segments was the most challengingg part. One cool thing is that this really stressed my buddy-allocator's assertions. For example, I faced double frees all the time. Overall, synchronizing the allocation, copying, and freeing of shared memory segments across different children was difficult.

### How can we test?

- run `make run-testmutex` to test mutexes
- run `make run-testshm` to test shared memory segments
- run `make run-testfutex` to test futexes

## Grading notes
