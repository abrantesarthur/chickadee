#ifndef CHICKADEE_K_VFS_HH
#define CHICKADEE_K_VFS_HH
#include "kernel.hh"
#include "chickadeefs.hh"

extern spinlock initfs_lock;

struct vnode;
struct pipe_vnode;
struct memfile_vnode;
struct keyboard_console_vnode;
struct diskfile_vnode;
struct file_descriptor;
struct bounded_buffer;
struct memfile;

struct vnode {
    spinlock lock_;
    std::atomic<int> ref_;

    vnode(int ref = 0) : ref_(ref) {
    }

    virtual uintptr_t read(file_descriptor* f, uintptr_t addr, size_t sz) = 0;
    virtual uintptr_t write(file_descriptor* f, uintptr_t addr, size_t sz) = 0;
};

struct pipe_vnode : public vnode {
    bounded_buffer* buf_;

    pipe_vnode(bounded_buffer* buf, int ref) : vnode(ref), buf_(buf) {
        assert(buf_);
    }

    uintptr_t read(file_descriptor* f, uintptr_t addr, size_t sz) override;
    uintptr_t write(file_descriptor* f, uintptr_t addr, size_t sz) override;
};

struct memfile_vnode : public vnode {
    memfile* mf_;

    memfile_vnode(memfile* mf, int ref) : vnode(ref), mf_(mf) {
        assert(mf);
    }

    uintptr_t read(file_descriptor* f, uintptr_t addr, size_t sz) override;
    uintptr_t write(file_descriptor* f, uintptr_t addr, size_t sz) override;
};

struct keyboard_console_vnode : public vnode {
    uintptr_t read(file_descriptor* f, uintptr_t addr, size_t sz) override;
    uintptr_t write(file_descriptor* f, uintptr_t addr, size_t sz) override;
};

struct diskfile_vnode : public vnode {
    chkfs::inode* ino_;

    diskfile_vnode(chkfs::inode* ino, int ref) :  vnode(ref), ino_(ino) {
        assert(ino_);
    }

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

#endif