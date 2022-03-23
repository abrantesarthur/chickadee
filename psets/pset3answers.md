# CS 161 Problem Set 3 Answers

Leave your name out of this file. Put collaboration notes and credit in
`pset3collab.md`.

## Answers to written questions

### Part C: Pipes

##### VFS changes

I added a `pipe_vnode` to my vfs and implemented its `read` and `write`

### Part D: Memfs

##### `memfile::initfs` synchronization plan

Many processes can have concurrent access to the `initfs` array. To synchronize their access, I created a global spinlock `initfs_lock` in `k-devices.cc`. It is illegal to access or modify `initfs` without having acquired this lock.

Similarly, I also added a spinlock `memfile::lock_` to synchronize access to `memfile`'s member variables.

##### VFS changes

I have added a new type of vnode (i.e., struct memfile_vnode) into my initial VFS, as well as implemented its read and write methods in order to accomodate memfiles. I have also added offset variables (i.e., rpos* and wpos*) into the struct file_descriptor. These variables will allow us to keep track of where in a memfile the next read and write operations should execute.

### Part E: Pipes

##### VFS changes

I added a `pipe_vnode` to my vfs and implemented its `read` and `write`

---

## Grading notes

---
