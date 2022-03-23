# CS 161 Problem Set 3 VFS Design Document

## Interface

```cpp
struct vnode {
    spinlock lock_;
    std::atomic<int> ref_ = 0;
    void* data_ = nullptr;
    virtual uintptr_t read(file_descriptor* f, uintptr_t addr, size_t sz) = 0;
    virtual uintptr_t write(file_descriptor* f, uintptr_t addr, size_t sz) = 0;
};

struct pipe_vnode : public vnode {
    uintptr_t read(file_descriptor* f, uintptr_t addr, size_t sz) override;
    uintptr_t write(file_descriptor* f, uintptr_t addr, size_t sz) override;
};

struct memfile_vnode : public vnode {
    uintptr_t read(file_descriptor* f, uintptr_t addr, size_t sz) override;
    uintptr_t write(file_descriptor* f, uintptr_t addr, size_t sz) override;
};

struct keyboard_console_vnode : public vnode {
    uintptr_t read(file_descriptor* f, uintptr_t addr, size_t sz) override;
    uintptr_t write(file_descriptor* f, uintptr_t addr, size_t sz) override;
};


struct file_descriptor {
    enum fd_t {
        kbd_cons_t,
        memfile_t,
        pipe_t,
        disk_t
    };
    spinlock lock_;
    std::atomic<int> ref_ = 0;       // number of processes referencing this
    std::atomic<off_t> rpos_ = 0;   // current read position
    std::atomic<off_t> wpos_ = 0;   // current write position
    bool readable_ = false;         // whether the file is readable
    bool writable_ = false;         // whether the file is writables
    int type_;                      // the fd_t of this file descriptor
    vnode* vnode_ = nullptr;
};

struct bounded_buffer {
    spinlock lock_;
    wait_queue wq_;
    const unsigned cap_ = 128;
    char buf_[128];
    size_t pos_ = 0;
    size_t len_ = 0;    // number of characters in buffer
    bool write_closed_ = false;
    bool read_closed_ = false;

    uintptr_t read(char* buf, size_t sz);
    uintptr_t write(const char* buf, size_t sz);
};

```

## Functionality

- Each process has a file descriptor table, which is an array of 32 `file_descriptor` pointers
- Each `file_descriptor` has a pointer to a `struct vnode`.
- A `vnode` can be have one of four types: `kbd_cons`, `pipe`, `memfile`, or `disk`. The difference between these types of vnodes lies in the implementation of their `vnode::read()` and `vnode::write ()` methods.
- After the `dup2` system call executes, multiple entries in the file descriptor table of a given process can point to the same `file_descriptor`.
- When different processes access the same file, multiple file descriptors across different processes may point to the same `vnode`.

## Memory Allocation

- `vnode`
  - `kbd_cons`
    - allocated at boot time when the boot_process is allocated.
    - deallocated when all processesses have exited
  - `pipe`
    - allocated whenever a process makes a pipe syscall
    - deallocated when both the read and write ends of the pipe are closed
  - `memfile`
    - allocated whenever their corresponding files are opened
    - deallocated whenever their reference count variable is 0 (i.e., no processes are accessing their corresponding files)
- `file_descriptor`
  - `kbd_cons`
    - allocated at boot time when the boot_process is allocated.
    - deallocated when all processesses have exited
  - `pipe`
    - allocated whenever a process makes a pipe syscall
    - deallocated when the pipe end to which it refers (i.e., read or write) is closed
  - `memfile`
    - allocated when their corresponding files are opened
    - deallocated when their reference count hits 0 (i.e., no file descriptor entries reference them).
- `bbuffer`
  - allocated whenever a process makes a pipe syscall
  - deallocated when both the read and write ends of the pipe are closed

## Synchronization invariants

##### VFS

- a process A may not access or modify the VFS objects (i.e., file descriptors, vnodes, and bbuffers) of a process B, unless B is a direct or indirect descendant of A and vice-versa.

##### File Descriptor and Vnode

- reading or modifying any properties of a `file_descriptor` and `vnode` requires that structure's lock.
  This is the case even though they some of these properties are atomic. This, for instance, guarantees consistent reading or writing to a pipe's buffer.

- adding a reference to a `file_descriptor` or `vnode` and updating the reference count must be done in one atomic step. Use `file_descriptor` and `vnode` locks to accomplish this.

  For example, when a child is forked, adding a new reference to a file descriptor and incrementing its `ref_` must be done in one atomic step. This ensures tht, at any given time, the real reference count of an object and the value of the `ref_` variable are the same.

##### Memfile

- Reading or writing to a `memfile` property requires the `memfile::lock_`

  This will guarantees that the `memfile::read` and `memfile::write` methods behave consistently.

- a process that holds the `memfile::lock_` must not try to acquire the `file_descriptor::lock_` of the `file_descriptor` referencing that `memfile`. The opposite, however, is allowed.

  Because in `memfile::read` and `memfile::write` we acquire the `file_descriptor::lock_` and then the `memfile::lock_`, this invariant will prevent deadlock issues from happening.

##### Initfs

- `initfs_lock` is a global, static lock that protects the `initfs` array. It is illegal to modify or access the array without having acquired this lock.

##### Bounded Buffer

7. the `bounded_buffer::lock_` protects the `bounded_buffer` properties. It is illegal to access or modify those objects without acquiring this lock.

   We acquire this lock when executing the `bounded_buffer:read` and `bounded_buffer:write` methods. Hence, respecting this invariant will prevent inconsistencies in these methods.

## Future Directions

- Adding support to files other than memory backed files, pipes, and keyboard/console will require implementing `read` and `write` methods to new types of `vnode`.
