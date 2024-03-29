#ifndef CHICKADEE_K_CHKFS_HH
#define CHICKADEE_K_CHKFS_HH
#include "kernel.hh"
#include "chickadeefs.hh"
#include "k-lock.hh"
#include "k-wait.hh"

// buffer cache

using bcentry_clean_function = void (*)(bcentry*);

struct bcentry {
    using blocknum_t = chkfs::blocknum_t;

    enum estate_t {
        es_empty, es_allocated, es_loading, es_clean, es_dirty
    };

    std::atomic<int> estate_ = es_empty;

    spinlock lock_;                      // protects ref_ and most `estate_` changes
    blocknum_t bn_;                      // disk block number (unless empty)
    unsigned ref_ = 0;                   // reference count
    unsigned char* buf_ = nullptr;       // memory buffer used for entry
    std::atomic<int> write_ref_ = 0;     // write reference
    static wait_queue write_ref_wq_;     // write reference wait queue
    list_links link_;
    static list<bcentry, &bcentry::link_> dirty_list_;


    // return the index of this entry in the buffer cache
    inline size_t index() const;

    // test if this entry is empty (`estate_ == es_empty`)
    inline bool empty() const;

    // test if this entry's memory buffer contains a pointer
    inline bool contains(const void* ptr) const;

    // release the caller's reference
    void put();

    // obtain/release a write reference to this entry
    void get_write();
    void put_write(bool mark_dirty = true);
    void mark_dirty();


    // internal functions
    void clear();
    bool load(irqstate& irqs, bcentry_clean_function cleaner);
};

struct bufcache {
    using blocknum_t = bcentry::blocknum_t;

    // TODO: increase number of entries
    static constexpr size_t ne = 100;

    spinlock lock_;                  // protects lru_stack and all entries' bn_ and ref_
    wait_queue read_wq_;
    static wait_queue evict_wq_;
    bcentry e_[ne + 1];             // add extra entry for superblock
    int lru_stack[ne];              // least recently used entries


    static inline bufcache& get();

    bcentry* get_disk_entry(blocknum_t bn,
                            bcentry_clean_function cleaner = nullptr);

    int sync(int drop);
    void mark_mru(int index);       // mark most recently used entry
    int evict_lru(irqstate& irqs);   // evict least recently used entry

 private:
    static bufcache bc;

    bufcache();
    NO_COPY_OR_ASSIGN(bufcache);
};


// chickadeefs state: a Chickadee file system on a specific disk
// (Our implementation only speaks to `sata_disk`.)

struct chkfsstate {
    using blocknum_t = chkfs::blocknum_t;
    using inum_t = chkfs::inum_t;
    using inode = chkfs::inode;
    static constexpr size_t blocksize = chkfs::blocksize;

    static inline chkfsstate& get();

    // obtain an inode by number
    inode* get_inode(inum_t inum);

    // directory lookup in `dirino`
    inode* lookup_inode(inode* dirino, const char* name);
    // directory lookup starting at root directory
    inode* lookup_inode(const char* name);

    blocknum_t allocate_extent(unsigned count = 1);
    chkfs::inode* create_file(const char* pathname, uint32_t type = chkfs::type_regular);

    int link_inode(chkfs::inum_t inum, const char* pathname);



  private:
    static chkfsstate fs;

    chkfsstate();
    NO_COPY_OR_ASSIGN(chkfsstate);
};


inline bufcache& bufcache::get() {
    return bc;
}

inline chkfsstate& chkfsstate::get() {
    return fs;
}

inline size_t bcentry::index() const {
    auto& bc = bufcache::get();
    assert(this >= bc.e_ && this < bc.e_ + bc.ne);
    return this - bc.e_;
}

inline bool bcentry::empty() const {
    return estate_.load(std::memory_order_relaxed) == es_empty;
}

inline bool bcentry::contains(const void* ptr) const {
    return estate_.load(std::memory_order_relaxed) >= es_clean
        && reinterpret_cast<uintptr_t>(ptr) - reinterpret_cast<uintptr_t>(buf_)
               < chkfs::blocksize;
}

inline void bcentry::clear() {
    assert(ref_ == 0);
    assert(lock_.is_locked());
    estate_ = es_empty;
    if (buf_) {
        kfree(buf_);
        buf_ = nullptr;
    }
}

#endif
