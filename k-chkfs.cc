#include "k-chkfs.hh"
#include "k-ahci.hh"
#include "k-chkfsiter.hh"

bufcache bufcache::bc;

wait_queue bcentry::write_ref_wq_;
list<bcentry, &bcentry::link_> bcentry::dirty_list_;
wait_queue bufcache::evict_wq_;

bufcache::bufcache() {
    for(size_t i = 0; i < ne; ++i) {
        lru_stack[i] = -1;
    }
}

// bufcache::mark_mru(ei)
//    Marsk the buffer cache entry with entry index 'ei' as the most 
//    recently used one. It is wrong to call mark_mru when the
//    lru_stack is full and 'ei' is not in the lru_stack. Instad,
//    call bufcache::evict_lru() first to make a slot available.
void bufcache::mark_mru(int ei) {
    assert(lock_.is_locked());
    size_t i, slot = -1;
    // loof for slot with 'ei' of that is empty
    for(i = 0; i < ne; ++i) {
        if(lru_stack[i] == ei) {
            slot = i;
            break;
        } else if(lru_stack[i] < 0) {
            slot = i;
        }
    }

    // a slot must have been found
    assert(slot != size_t(-1));

    // mark 'ei' the most recently used one
    for(int j = slot; j > 0; j--) {
        lru_stack[j] = lru_stack[j - 1];
    }
    lru_stack[0] = ei;
}

// bufcache::get_lru()
//    returns the buffer cache index of the least recently
//    used entry that is unreferenced and not dirty, or -1
//    if not found. If found an entry, it locks it and stores
//    the irqstate in '&eirqs'
int bufcache::get_lru(irqstate& eirqs) {
    assert(lock_.is_locked());

    int ei;
    // look for lru, unreferenced, and non-dirty entry
    for(int i = ne - 1; i >= 0; i--) {
        ei = lru_stack[i];
        eirqs = e_[ei].lock_.lock();
        // if found, return entry index
        if(!e_[ei].ref_ && e_[ei].estate_ != bcentry::es_dirty) {
            return ei;
        }
        e_[ei].lock_.unlock(eirqs);
    }

    return -1;
}

// TODO: test the sh*t out of this
// bufcache::evict_lru(irqs)
//    returns the entry of the evicted entry. Its `irqs`
//    argument must be the irqstate of the bufcache::lock_,
//    which must be locked. This can block if there isn't any
//    entry available (i.e., unreferenced and non-dirty entries).
size_t bufcache::evict_lru(irqstate& irqs) {
    assert(lock_.is_locked());

    // only evict when buffer cache is full
    assert(lru_stack[ne - 1] >= 0);

    int ei;
    irqstate eirqs;
    // This won't actually block if an entry is found right away.
    // Otherwise, it will block until an entry is available.
    waiter().block_until(evict_wq_, [&] () {
        ei = get_lru(eirqs);
        if(ei != -1) {
            // get_lru locked the entry, so this is safe
            e_[ei].clear();
            lru_stack[ei] = -1;
            e_[ei].lock_.unlock(eirqs);
            return true; 
        }
        return false;
    }, lock_, irqs);

    // ei should be a valid entry by now
    return ei;
}

// bufcache::get_disk_entry(bn, cleaner)
//    Reads disk block `bn` into the buffer cache, obtains a reference to it,
//    and returns a pointer to its bcentry. The returned bcentry has
//    `buf_ != nullptr` and `estate_ >= es_clean`. The function may block.
//
//    If this function reads the disk block from disk, and `cleaner != nullptr`,
//    then `cleaner` is called on the entry to clean the block data.
//
//    Returns `nullptr` if there's no room for the block.

bcentry* bufcache::get_disk_entry(chkfs::blocknum_t bn,
                                  bcentry_clean_function cleaner) {
    assert(chkfs::blocksize == PAGESIZE);
    auto irqs = lock_.lock();
    
    size_t i;
    if(bn == 0) {
        // superblock is always the last entry
        i = ne;
    } else {
        // look for slot containing `bn`
        size_t empty_slot = -1;
        for (i = 0; i != ne; ++i) {
            if (e_[i].empty()) {
                if (empty_slot == size_t(-1)) {
                    empty_slot = i;
                }
            } else if (e_[i].bn_ == bn) {
                break;
            }
        }

        // if not found, use a free slot
        if (i == ne) {
            // if cache is full, evict lru entry
            if (empty_slot == size_t(-1)) {
                // this may block
                empty_slot = evict_lru(irqs);
                assert(empty_slot != size_t(-1));
            }
            i = empty_slot;
        }

        // mark most recently used slot
        mark_mru(i);
    }

    // obtain entry lock
    e_[i].lock_.lock_noirq();

    // mark allocated if empty
    if (e_[i].empty()) {
        e_[i].estate_ = bcentry::es_allocated;
        e_[i].bn_ = bn;
    }

    // no longer need cache lock
    lock_.unlock_noirq();

    // mark reference
    ++e_[i].ref_;

    // load block
    bool ok = e_[i].load(irqs, cleaner);

    // unlock and return entry
    if (!ok) {
        --e_[i].ref_;
    }
    e_[i].lock_.unlock(irqs);
    return ok ? &e_[i] : nullptr;
}


// bcentry::load(irqs, cleaner)
//    Completes the loading process for a block. Requires that `lock_` is
//    locked, that `estate_ >= es_allocated`, and that `bn_` is set to the
//    desired block number.

bool bcentry::load(irqstate& irqs, bcentry_clean_function cleaner) {
    bufcache& bc = bufcache::get();

    // load block, or wait for concurrent reader to load it
    while (true) {
        assert(estate_ != es_empty);
        if (estate_ == es_allocated) {
            if (!buf_) {
                buf_ = reinterpret_cast<unsigned char*>
                    (kalloc(chkfs::blocksize));
                if (!buf_) {
                    return false;
                }
            }
            estate_ = es_loading;
            lock_.unlock(irqs);

            sata_disk->read(buf_, chkfs::blocksize,
                            bn_ * chkfs::blocksize);
            
            irqs = lock_.lock();
            estate_ = es_clean;
            if (cleaner) {
                cleaner(this);
            }
            bc.read_wq_.wake_all();
        } else if (estate_ == es_loading) {
            waiter().block_until(bc.read_wq_, [&] () {
                    return estate_ != es_loading;
                }, lock_, irqs);
        } else {
            return true;
        }
    }
}


// bcentry::put()
//    Releases a reference to this buffer cache entry. Does not
//    call clear() (i.e., free underlying buffer cache entry) if
//    reference count hits zero. Instead, delay the freeing of
//    memory for later under a LRU policy. The caller must not
//    use the entry after this call.

void bcentry::put() {
    spinlock_guard guard(lock_);
    assert(ref_ > 0);
    --ref_;
    // if possible, wake processes waiting for avaialable bufcache entry to evict
    if(!ref_ && estate_ != bcentry::es_dirty) {
        bufcache::evict_wq_.wake_all();
    }
}


// bcentry::get_write()
//    Obtains a write reference for this entry.
//    Prevents concurrent writes to this entry.
void bcentry::get_write() {
    assert(estate_ != es_empty);
    waiter().block_until(write_ref_wq_, [&] () {
        return write_ref_.exchange(1) == 0;
    });
}


// bcentry::put_write()
//    Releases a write reference for this entry, and 
//    mark it as dirty, if requested.

void bcentry::put_write(bool mark_dirty) {
    if(mark_dirty) {
        spinlock_guard g(lock_);
        if(estate_ != es_dirty) {
            estate_ = es_dirty;
            dirty_list_.push_front(this);
        }   
    }
    write_ref_.store(0);
    write_ref_wq_.wake_all();
}


// bufcache::sync(drop)
//    Writes all dirty buffers to disk, blocking until complete.
//    If `drop > 0`, then additionally free all buffer cache contents,
//    except referenced blocks. If `drop > 1`, then assert that all inode
//    and data blocks are unreferenced.

int bufcache::sync(int drop) {
    if(!sata_disk) return E_IO;

    // save dirty list state
    list<bcentry, &bcentry::link_> dirty_list;
    dirty_list.swap(bcentry::dirty_list_);
    bcentry::dirty_list_.reset();

    // write dirty entries to disk
    while(bcentry *e = dirty_list.pop_front()) {
        e->get_write();
        sata_disk->write(e->buf_, chkfs::blocksize, e->bn_ * chkfs::blocksize);
        irqstate eirqs = e->lock_.lock();
        e->estate_ = bcentry::es_clean;
        // wake processes waiting for available entries to evict
        if(!e->ref_) {
            bufcache::evict_wq_.wake_all();
        }
        e->lock_.unlock(eirqs);
        e->put_write(false);
    }

    // drop clean buffers if requested
    if (drop > 0) {
        spinlock_guard guard(lock_);
        for (size_t i = 0; i != ne; ++i) {
            spinlock_guard eguard(e_[i].lock_);

            // validity checks: referenced entries aren't empty; if drop > 1,
            // no data blocks are referenced
            assert(e_[i].ref_ == 0 || e_[i].estate_ != bcentry::es_empty);
            if (e_[i].ref_ > 0 && drop > 1 && e_[i].bn_ >= 2) {
                error_printf(CPOS(22, 0), COLOR_ERROR, "sync(2): block %u has nonzero reference count\n", e_[i].bn_);
                assert_fail(__FILE__, __LINE__, "e_[i].bn_ < 2");
            }

            // actually drop buffer
            if (e_[i].ref_ == 0) {
                e_[i].clear();
                // wake processes waiting for available entries to evict
                if(e_[i].estate_ != bcentry::es_dirty) {
                    bufcache::evict_wq_.wake_all();
                }
            }
        }
    }

    return 0;
}


// inode lock functions
//    The inode lock protects the inode's size and data references.
//    It is a read/write lock; multiple readers can hold the lock
//    simultaneously.
//
//    IMPORTANT INVARIANT: If a kernel task has an inode lock, it
//    must also hold a reference to the disk page containing that
//    inode.

namespace chkfs {

void inode::lock_read() {
    mlock_t v = mlock.load(std::memory_order_relaxed);
    while (true) {
        if (v >= mlock_t(-2)) {
            current()->yield();
            v = mlock.load(std::memory_order_relaxed);
        } else if (mlock.compare_exchange_weak(v, v + 1,
                                               std::memory_order_acquire)) {
            return;
        } else {
            // `compare_exchange_weak` already reloaded `v`
            pause();
        }
    }
}

void inode::unlock_read() {
    mlock_t v = mlock.load(std::memory_order_relaxed);
    assert(v != 0 && v != mlock_t(-1));
    while (!mlock.compare_exchange_weak(v, v - 1,
                                        std::memory_order_release)) {
        pause();
    }
}

void inode::lock_write() {
    mlock_t v = 0;
    while (!mlock.compare_exchange_weak(v, mlock_t(-1),
                                        std::memory_order_acquire)) {
        current()->yield();
        v = 0;
    }
}

void inode::unlock_write() {
    assert(has_write_lock());
    mlock.store(0, std::memory_order_release);
}

bool inode::has_write_lock() const {
    return mlock.load(std::memory_order_relaxed) == mlock_t(-1);
}

}


// chickadeefs state

chkfsstate chkfsstate::fs;

chkfsstate::chkfsstate() {
}


// clean_inode_block(entry)
//    Called when loading an inode block into the buffer cache. It clears
//    values that are only used in memory.

static void clean_inode_block(bcentry* entry) {
    uint32_t entry_index = entry->index();
    auto is = reinterpret_cast<chkfs::inode*>(entry->buf_);
    for (unsigned i = 0; i != chkfs::inodesperblock; ++i) {
        // inode is initially unlocked
        is[i].mlock = 0;
        // containing entry's buffer cache position is `entry_index`
        is[i].mbcindex = entry_index;
    }
}


// chkfsstate::get_inode(inum)
//    Returns inode number `inum`, or `nullptr` if there's no such inode.
//    Obtains a reference on the buffer cache block containing the inode;
//    you should eventually release this reference by calling `ino->put()`.

chkfs::inode* chkfsstate::get_inode(inum_t inum) {
    auto& bc = bufcache::get();
    auto superblock_entry = bc.get_disk_entry(0);
    assert(superblock_entry);
    auto& sb = *reinterpret_cast<chkfs::superblock*>
        (&superblock_entry->buf_[chkfs::superblock_offset]);
    superblock_entry->put();

    chkfs::inode* ino = nullptr;
    if (inum > 0 && inum < sb.ninodes) {
        auto bn = sb.inode_bn + inum / chkfs::inodesperblock;
        if (auto inode_entry = bc.get_disk_entry(bn, clean_inode_block)) {
            ino = reinterpret_cast<inode*>(inode_entry->buf_);
        }
    }
    if (ino != nullptr) {
        ino += inum % chkfs::inodesperblock;
    }
    return ino;
}


namespace chkfs {
// chkfs::inode::entry()
//    Returns a pointer to the buffer cache entry containing this inode.
//    Requires that this inode is a pointer into buffer cache data.
bcentry* inode::entry() {
    assert(mbcindex < bufcache::ne);
    auto entry = &bufcache::get().e_[mbcindex];
    assert(entry->contains(this));
    return entry;
}

// chkfs::inode::put()
//    Releases the caller’s reference to this inode, which must be located
//    in the buffer cache.
void inode::put() {
    entry()->put();
}
}


// chkfsstate::lookup_inode(dirino, filename)
//    Looks up `filename` in the directory inode `dirino`, returning the
//    corresponding inode (or nullptr if not found). The caller must have
//    a read lock on `dirino`. The returned inode has a reference that
//    the caller should eventually release with `ino->put()`.

chkfs::inode* chkfsstate::lookup_inode(inode* dirino,
                                       const char* filename) {
    chkfs_fileiter it(dirino);

    // read directory to find file inode
    chkfs::inum_t in = 0;
    for (size_t diroff = 0; !in; diroff += blocksize) {
        if (bcentry* e = it.find(diroff).get_disk_entry()) {
            size_t bsz = min(dirino->size - diroff, blocksize);
            auto dirent = reinterpret_cast<chkfs::dirent*>(e->buf_);
            for (unsigned i = 0; i * sizeof(*dirent) < bsz; ++i, ++dirent) {
                if (dirent->inum && strcmp(dirent->name, filename) == 0) {
                    in = dirent->inum;
                    break;
                }
            }
            e->put();
        } else {
            return nullptr;
        }
    }
    return get_inode(in);
}


// chkfsstate::lookup_inode(filename)
//    Looks up `filename` in the root directory.

chkfs::inode* chkfsstate::lookup_inode(const char* filename) {
    auto dirino = get_inode(1);
    if (dirino) {
        dirino->lock_read();
        auto ino = fs.lookup_inode(dirino, filename);
        dirino->unlock_read();
        dirino->put();
        return ino;
    } else {
        return nullptr;
    }
}


// chkfsstate::allocate_extent(unsigned count)
//    Allocates and returns the first block number of a fresh extent.
//    The returned extent doesn't need to be initialized (but it should not be
//    in flight to the disk or part of any incomplete journal transaction).
//    Returns the block number of the first block in the extent, or an error
//    code on failure. Errors can be distinguished by
//    `blocknum >= blocknum_t(E_MINERROR)`.

auto chkfsstate::allocate_extent(unsigned count) -> blocknum_t {
    // load superblock into the buffer cache
    auto& bc = bufcache::get();
    auto superblock_entry = bc.get_disk_entry(0);
    assert(superblock_entry);
    auto& sb = *reinterpret_cast<chkfs::superblock*>
        (&superblock_entry->buf_[chkfs::superblock_offset]);
    superblock_entry->put();

    // load free block bitmap into buffer cache
    bcentry* fbb_entry = bc.get_disk_entry(sb.fbb_bn);
    
    fbb_entry->get_write();

    // find a countiguous range of 'count' 1 bits
    bitset_view fbb_view(reinterpret_cast<uint64_t*>(fbb_entry->buf_),
        chkfs::bitsperblock);

    size_t bn, curr_bn;
    bool found_extent;
    for(bn = sb.data_bn; bn < sb.journal_bn;) {
        found_extent = true;
        for(curr_bn = bn; curr_bn < bn + count; ++curr_bn) {
            if(!fbb_view[curr_bn]) {
                found_extent = false;
                break;
            }
        }
        
        if(found_extent) {
            for(curr_bn = bn; curr_bn < bn + count; ++curr_bn) {
                fbb_view[curr_bn] = false;
            }
            break;
        }

        bn = curr_bn + 1;
    }

    fbb_entry->put_write();
    fbb_entry->put();
    if(!found_extent) {
        return 0;
    }
    // return first block number in allocated extent
    return bn;
}
