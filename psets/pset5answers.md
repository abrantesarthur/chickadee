# CS 161 Problem Set 5 Answers

Leave your name out of this file. Put collaboration notes and credit in
`pset5collab.md`.

## Answers to written questions

##### Synchronization invariants

###### lock interleavings

- If `ptable_lock` and `pgtable_lock` are simultaneously locked, `ptable_lock` must always be locked first.

  This is so we avoid deadlocking issues due to `proc::syscall_fork` grabbing `ptable_lock` first.

- If `ptable_lock` and `proc_group::lock_` are simultaneously locked, `ptable_lock` must always be locked first.

  This is so we avoid deadlocking issues due to `proc::syscall_exit` grabbing `ptable_lock` first

###### proc_group::children\_

- Accessing `proc_group::children_` requires the `ptable_lock`

  Remember that `ptable_lock` protects everything related to process hierarchy.

###### proc_group::who_exited\_

- Accessing `proc_group::who_exited_` requires the `proc_group::lock_`

  This prevents racing conditions in `syscall_exit`, if two threads try to exit at the same time.

- It starts as `nullptr` and can only modified once by the thread who calls `sys_exit` first

  This ensures that the thread who called `sys_exit` sets its own state to `ps_exiting` after all other threads have exited, as opposed to havign its state set to `ps_exiting` in `cpustate::schedule`.

## Questions

1. How should we implement sleeping behavior? Which thread in the parent process should be awaken by the child? Any sleeping thread or a specific thread?
2. Why am I getting a protection fault when cpu 1 tries accessing a member of the pg\_ in line 117 of k-cpu.cc?

## To do

- sync access to pg\_->exiting\_ and write invariants about it
- pg\_->exiting\_ invariant: it can only be switched once by the first process who exits
- prevent forking and cloning if exiting\_ flag is set
- mark idle tasks and process groups in `refresh`
- write invariants for `proc*group::is_exiting`: ps_exiting can only move from false to true!
- fix handle make NCPU=1 run-testkalloc error (line 666 in kernel.cc)
- what is the synchronization plan for `proc_group::children_`
- protect access to `proc_group::pagetable_`
- remove invariant that I added for `pgtable_lock`. `kill_zombie` disrespects it
- Fix `syscall_exit`: handle sleeping correclty
- Fix `syscall_exit`: handle modifying other process states correctly
- Fix `syscall_exit`: handle memory freeing correclty (eg., free pagetable after exit all threads)
- In `syscall_clone`: only free pagetable and other resources if other threads don't exist
- Protect `proc_group::ppid` with `pgtable_lock` instead of `ptable_lock` and add to synchronization invariatns
- Determine what `proc_group::lock_` should protect (`pagetable_`, something else?)

## Grading notes
