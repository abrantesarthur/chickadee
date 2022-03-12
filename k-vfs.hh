#ifndef CHICKADEE_K_VFS_HH
#define CHICKADEE_K_VFS_HH
#include "kernel.hh"

// TODO: include initfs_lock

struct vnode;
struct pipe_vnode;
struct memfile_vnode;
struct keyboard_console_vnode;
struct file_descriptor;

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
    spinlock lock_;
    enum {
        kbd_cons,
        memfile,
        pipe,
        disk
    } type_;
    std::atomic<int> ref_ = 0;       // number of processes referencing this
    std::atomic<off_t> rpos_ = 0;   // current read position
    std::atomic<off_t> wpos_ = 0;   // current write position
    bool readable_ = false;         // whether the file is readable
    bool writable_ = false;         // whether the file is writables
    vnode* vnode_ = nullptr;
};

#endif