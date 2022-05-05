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

- `syscall_futex`

- `sys_futex`

  This is the user side of things. It only calls syscall_futex if necessary. In other words, it actually checks the address before calling syscall_futex. Syscall_futex checks the address again.

### Goal

### Design

### Code

- `u-mutex.hh` and `u-mutex.cc` contains user-accessible mutex based on futex
- `u-lib.hh` contains user accessible `sys_futex`
- `kernel.cc` contains `syscall_futex` function
- `p-testfutex.cc` contains test code
- `kernel.hh` and `k-proc.cc`shared memory segment implementations

### Challenges

- modifying sys_exit and sys_fork to accomodate shm
- unamping shared_memory was a challenging. I had this assertion failure when freeing the physical range.
- maintaining shared data acorss forks and exit was challenging

### How can we test?

- run `make run-testfutex`
- run `make run-testshm`

## Grading notes

## Questions

1. How should we implement sleeping behavior? Which thread in the parent process should be awaken by the child? Any sleeping thread or a specific thread?
2. Why am I getting a protection fault when cpu 1 tries accessing a member of the pg\_ in line 117 of k-cpu.cc?

## To do

- update fork to handle shared memory segments
- update kill_zombie to handle shared memory segments
- mark idle tasks and process groups in `refresh`
- protect `proct_group::pagetable_` and `fd_table`
- Protect `proc_group::ppid` with `pgtable_lock` instead of `ptable_lock` and add to
  synchronization invariatns
