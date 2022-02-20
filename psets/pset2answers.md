# CS 161 Problem Set 2 Answers

Leave your name out of this file. Put collaboration notes and credit in
`pset2collab.md`.

## Answers to written questions

#### Part C. Parent Processes

##### Performance

- I added a `list_links children_links_` and a `children_` list to each `struct proc`. The former allows a process to be added as a child of another process. The latter allows a process to keep track of its children. This way, when a process is exiting, it can reparent its children in `O(C)` time, where `C` is its number of children, by iterating over `children_n`

##### Synchronization invariantes

###### proc::ppid\_

- reading and writing to `proc::ppid_` requires the `ptable_lock`

  This invariant syncrhonizes write access to the `ppid_` by an exiting parent and read access by the child. It also prevents a child from exiting at the same time as its parent.

###### proc::pstate\_

- TODO: specify the synchronization plan when start allowing processes to modify pstate\_
  are we usign ptable\*lock?
  How do we guarantee that waitpid does not free a zombie while another CPU is running on the corresponding kernel task stack? Wll, schedule will only add a process to the runq if its status is runnable. Moreover, it is an invariant that if a child is running on a CPU, then it is not on the runq for any other CPU. These two facts combined mean that if a process exits, and hence, sets its status to non-runnable, it won't be scheduled to run again in any cpu. Hence, it is safe for waitpid to delete the resources of a process with status nonrunnable.

###### proc::sleeping\_

- TODO: are we using ptable_lock? Is there a better strategy?

###### process hiearchy

- reading and writing to `proc::children_links` and `proc::children` requires the `ptable_lock`

  Although this isn't as scalable as using a `process_hierarchy_lock`, it also works and is easier to implement. We may change this strategy to something more scalable in the future.

###### pageset pages

- access to `pageset pages` must be synchronized with the `page_lock`, except:
  - When initializing `pageset pages` with `kalloc_init`

## Grading notes
