## `bufcache::evict_lru()`

- When the buffer cache is full and we want need to evict entries, instead of using LRU, use another policy that treats different kinds of blocks with different priorities. For example, treat inode blocks differently than data blocks.
