# CS 161 Problem Set 4 Answers

Leave your name out of this file. Put collaboration notes and credit in
`pset4collab.md`.

## Answers to written questions

### Part A. Buffer Cache

##### invariants

- reading or writing to the `bufcache::lru_stack` requires the `bufcache::lock_`

- `bufcache::evict_lru(irqs)` must be passed the `irqstate` of the `bufchache::lock_` as an argument and the `lock_` must be locked.

##### eviction policy

- our buffer cache eviction policy follows a recently used design for all blocks, except the `superblock`, which is kept in the last entry and is never evicted.

- `bufcache::mark_mru(ei)` does not evict blocks. Hence, if the `bufcache::lru_stack` is full, a block must first be evicted with `bufcache::evict_lru()` before calling `bufcache::mark_mru(ei)`. Failing to do so will result in an assertion error

- `bufcache::evict_lru(irqs)` evicts the least recently used buffer cache entry that is both unreferenced and non-dirty.

  If no entry meets these conditions, the function blocks on the `bufcache::evict_wq_` until that is the case. Processes waiting on this wait queue are awake either when dirty entries are written back to disk (i.e., when `bufcache::sync` executes) or when an entry reference is released (i.e., when `bcentry::put()` is called).

  Allowing `bufcache::evict_lru(irqs)` to block makes our system more resilient to cases when the buffer cache is highly utilized. Instead or returning some error, the function gracefully waits until an entry is available to be evicted.

##### prefetching policy

TODO

## Grading notes
