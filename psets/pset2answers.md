# CS 161 Problem Set 2 Answers

Leave your name out of this file. Put collaboration notes and credit in
`pset2collab.md`.

## Answers to written questions

#### Part C. Parent Processes

##### Performance

- I added a `list_links children_links_` and a `children_` list to each `struct proc`. The former allows a process to be added as a child of another process. The latter allows a process to keep track of its own children. This way, when a process is exiting, it can reparent its children in `O(C)` time, where `C` is its number of children, by iterating over `children_`

##### Synchronization invariantes

###### proc::ppid\_

- reading and writing to `proc::ppid_` requires the `ptable_lock`

  This invariant syncrhonizes write access to the `ppid_` by an exiting parent and read access to the `ppid_` by the child. This handles the case where a process is exiting at the same time that one of its children calls `sys_getppid()`. It also prevents a child from exiting at the same time as its parent.

###### proc::pstate\_

- `proc::pstate_` may be modified only by the corresponding kernel task, except that other contexts may perform an atomic compare-and-swap operation that changes `ps_blocked` state to `ps_runnable`.

  This guarantees that `sys_waitpid` does not free a zombie while another CPU is running on the zombie's kernel task stack. After all, `cpustate::schedule(p)` will only schedule a process to run if its status is `ps_runnable`. Moreover, it is an invariant that an executing process is not on the `runq` for any other CPU. Hence if a process exits, hence setting its status to `ps_nonrunnable` and becoming a zombie, our invariant prevents any other cpu from executing on its kernel stack.

  In the future, allowing different contexts to transition a process' state from `ps_blocked` to something other than `ps_runnable` may affect this invariant's correctness.

###### proc::sleeping\_

- `p->sleeping_` can be modified only by the kernel task for `p`.

  Because `p->sleeping_` is declared as `std::atomic<bool>`, if `p` writes to it at the same time that another kernel task reads its value, the behavior is well-defined.

###### proc::interrupted\_

- only a process `p` exiting child may set `p->interrupted_` to true.

  This prevents a process from being interrupted for a reason other than a child is exiting or the timer interrupt fired up. In the future, if we want to support other types of interruptions, this invariant should be updated.

###### process hiearchy

- reading and writing to `proc::children_links` and `proc::children` requires the `ptable_lock`

  Although this isn't as scalable as using a `process_hierarchy_lock`, it also works and is easier to implement. We may change this strategy to something more scalable in the future.

###### pageset pages

- access to `pageset pages` must be synchronized with the `page_lock`, except:
  - When initializing `pageset pages` with `kalloc_init`

## Grading notes
