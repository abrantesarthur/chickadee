# CS 161 Problem Set 2 Answers

Leave your name out of this file. Put collaboration notes and credit in
`pset2collab.md`.

## Answers to written questions

#### Part C. Parent Processes

##### Performance

- I added a `list_links children_links_` and a `children_` list to each `struct proc`. The former allows a process to be added as a child of another process. The latter allows a process to keep track of its children. This way, when a process is exiting, it can reparent its children in `O(C)` time, where `C` is its number of children, by iterating over `children_n`

##### Synchronization invariantes

###### proc::ppid\_

- Reading and writing to `proc::ppid_` requires the `ptable_lock`

This invariant syncrhonizes write access to the `ppid_` by an exiting parent and read access by the child. It also prevents a child from exiting at the same time as its parent.

###### proc::pstate\_

- TODO: specify the synchronization plan when start allowing processes to modify pstate\_.\

###### proc::children_links and proc::children\_

###### pageset pages

- access to `pageset pages` must be synchronized with the `page_lock`, except:
  - When initializing `pageset pages` with `kalloc_init`

## Grading notes
