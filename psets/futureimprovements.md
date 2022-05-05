## `bufcache::evict_lru()`

- When the buffer cache is full and we want need to evict entries, instead of using LRU, use another policy that treats different kinds of blocks with different priorities. For example, treat inode blocks differently than data blocks.

- improve `mark_dirty` strategy. Understand difference between `ino->unlock_write` and `ino->entry()->put_write`. Moreover, It seems like we always mark dirty when we call `ino()->unlock_write`

## scheduler

- implement a better scheduline algorithm. Look at Linux's Completely Fair Algorithm (Lecture 17) for inspiration

## file system

- add concurrent disk writes by modifying the `ahcistate::read_or_write` function to use more than just command slot 0.

- support buffer cache prefetching (see pset4 part A)

- increase number of blocks in file system. This should impact multiple constants and functions such as `allocate_extent`

- what if a child seeks at the same time that its parent writes to a disk file? Is the f->wpos\* and f->rpos fields going to be synchronized?

- pset 4: add support to the `sys_unlink` (test with `make cleanfs run-testwritefs4`), `sys_rename`, `sys_mkdir`, and `sys_rmdir`

## Threads and processes

- add support for more than 16 processes and threads
- `make run-allocexit` dislays dots (i.e., memory) that can't be allocated. Fix that

## proc::syscall_execv

- It assumes that it is called by a single-threaded process. Remove that assumption

## u-shm.hh

- `shmget`
  add support for flags
- `shmat`
  add support for non-null `shmaddr`
