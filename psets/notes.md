## `bcentry::get_write()` vs. `inode::lock_write()`

- `bcentry::get_write()` synchronizes writes to `bcentry::buf_`, regardless of what is actually stored in the buffer. For example, the buffer could contain inode, dirent, superblock, data blocks, etc. This enforces buffer cache level invariants, regardless of what block is stored in it. For example, buffer contents must not be modified while the buffer is in flight to disk.

- `inode::lock_write()` synchronizes writes to the `inode` fields spcifically, such as its size and data references. This enforces file-system level invariantes.

- It is possible that we must call both methods simultaneously. For example, when writing to an inode, we first lock acess to the inode in `diskfile_vnode::write()`, and then we get a write reference to the entry in `chkfs_fileiter::insert()`. Locking access to the `inode` allows us to check and modify its `inode::size`, whereas getting the write reference to the buffer cache entry allows us to modify the contents of its buffer when inserting extents to it.

## `bcentry::put_write()` vs. `inode::unlock_write()`

- `bcentry::put_write()` releases the buffer cache level write reference.

- `inode::unlock_write()` releases the inode level's lock.

## `bcentry::put()`

- `bcentry::put()` releases the reference to the buffer cache entry. This means that the process will no longer need acces to that entry, so it can be evicted if necessary.

## `bcentry::get_write()` vs. `bcentry::lock_`

- The lock protects the entry's fields such as its `ref_` count, `buf_` underlying data, and the `bn_` buffer number that it stores. Modifying these fields requires the lock. On the other hand, `get_write` allows us to gain exclusive writing access to the `buf_` data. It doesn't, however, prevent other processes from accessing the `bcentry` fields protected by the `bcentry::lock_`.
