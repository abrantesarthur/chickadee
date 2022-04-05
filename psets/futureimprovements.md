## `bufcache::evict_lru()`

- When the buffer cache is full and we want need to evict entries, instead of using LRU, use another policy that treats different kinds of blocks with different priorities. For example, treat inode blocks differently than data blocks.

## scheuduler

- implement a better scheduline algorithm. Look at Linux's Completely Fair Algorithm (Lecture 17) for inspiration

## file system

- increase number of blocks in file system. This should impact multiple constants and functions such as `allocate_extent`
