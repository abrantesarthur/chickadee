#include "k-vfs.hh"
#include "k-wait.hh"
#include "k-devices.hh"
#include "k-chkfs.hh"
#include "k-chkfsiter.hh"


uintptr_t keyboard_console_vnode::read(file_descriptor* f, uintptr_t addr, size_t sz) {
    auto& kbd = keyboardstate::get();
    auto irqs = kbd.lock_.lock();

    // mark that we are now reading from the keyboard
    // (so 'q' should not power off)
    if(kbd.state_ == kbd.boot) {
        kbd.state_ = kbd.input;
    }

    // block until a line is available
    // (special case: do not block if the user wants to read 0 bytes)
    waiter w;
    w.block_until(kbd.wq_, [&] () {
        return (sz == 0 || kbd.eol_ > 0);
    }, kbd.lock_, irqs);

    // read that line or lines
    size_t n = 0;
    while(kbd.eol_ != 0 && n < sz) {
        if(kbd.buf_[kbd.pos_] == 0x04) {
            // Ctrl-D means EOF
            if(n == 0) {
                kbd.consume(1);
            }
            break;
        } else {
            *reinterpret_cast<char*>(addr) = kbd.buf_[kbd.pos_];
            ++addr;
            ++n;
            kbd.consume(1);
        }
    }

    kbd.lock_.unlock(irqs);
    return n;
}

uintptr_t keyboard_console_vnode::write(file_descriptor* f, uintptr_t addr, size_t sz) {
    auto& csl = consolestate::get();
    spinlock_guard guard(csl.lock_);
    size_t n = 0;
    while (n < sz) {
        int ch = *reinterpret_cast<const char*>(addr);
        ++addr;
        ++n;
        console_printf(0x0F00, "%c", ch);
    }
    return n;
}

uintptr_t pipe_vnode::read(file_descriptor* f, uintptr_t addr, size_t sz) {
    // avoid reading from write end of the pipe
    if(f->writable_) {
        return E_BADF;
    }
    return buf_->read(reinterpret_cast<char*>(addr), sz);
}

uintptr_t pipe_vnode::write(file_descriptor* f, uintptr_t addr, size_t sz) {
    // avoid reading from write end of the pipe
    if(f->readable_) {
        return E_BADF;
    }
    return buf_->write(reinterpret_cast<char*>(addr), sz);
}

// read(buf, sz)
//      read sz bytes from bounded_buffer::buf_ into 'buf'
//      in one atomic step
uintptr_t bounded_buffer::read(char* buf, size_t sz) {
    // make read atomic
    spinlock_guard guard(lock_);

    size_t pos = 0;
    
    // block until write end is written to or closed
    waiter w;
    w.block_until(wq_, [&] () {
        return (len_ > 0 || write_closed_);
    }, guard);

    // read data
    while(pos < sz && len_ > 0) {
        size_t left_to_read = sz - pos;
        size_t in_buffer = min(len_, cap_ - pos_);
        size_t n = min(left_to_read, in_buffer);
        memcpy(&buf[pos], &buf_[pos_], n);
        pos_ = (pos_ + n) % cap_;
        len_ -= n;
        pos += n;
    }

    // wake processes waiting for butter to have space
    if(pos > 0) {
        wq_.wake_all();
    }

    // return error if failed to read even though write end is open
    if(pos == 0 && sz > 0 && !write_closed_) {
        return -1;
    }

    return pos;
}

uintptr_t bounded_buffer::write(const char* buf, size_t sz) {
    // make write atomic
    spinlock_guard guard(lock_);

    assert(!write_closed_);

    // block until there is available space in buffer or read end is closed
    waiter w;
    w.block_until(wq_, [&] () {
        return (len_ < cap_ || read_closed_);
    }, guard);

    // it's illegal to write to a pipe with closed read
    if(read_closed_) {
        return E_PIPE;
    }

    // write data
    size_t pos = 0;
    while(pos < sz && len_ < cap_) {
        size_t index = (pos_ + len_) % cap_;
        size_t space = min(cap_ - index, cap_ - len_);
        size_t n = min(sz - pos, space);
        memcpy(&buf_[index], &buf[pos], n);  
        len_ += n;
        pos += n; 
    }

    // wake process waiting for buffer to have data
    if(pos > 0) {
        wq_.wake_all();
    }

    // couldn't write to pipe even though read end isn't closed
    if(pos == 0 && sz > 0 && !read_closed_) {
        return -1;  // try again
    }

    return pos;
}

uintptr_t memfile_vnode::read(file_descriptor* f, uintptr_t addr, size_t sz) {
    // grab file_descriptor lock to sync with write
    spinlock_guard fd_guard(f->lock_);

    // illegal to read from non-readable file
    if(!f->readable_) {
        return E_BADF;
    }

    // TODO: add an invariant to avoid deadlock
    // grab memfile lock to sync with memfile_vnode::write()
    spinlock_guard mf_guard(mf_->lock_);
    // avoid buffer overflow
    sz = min(sz, mf_->len_ - f->rpos_);
    // read next 'sz' bytes from memfile into 'addr'
    memcpy(reinterpret_cast<void*>(addr), &mf_->data_[f->rpos_], sz);
    f->rpos_ += sz;

    return sz;
}

uintptr_t memfile_vnode::write(file_descriptor *f, uintptr_t addr, size_t sz) {
    // grab file_descriptor lock to sync with read
    spinlock_guard fd_guard(f->lock_);

    // illegal to write to non-writable file
    if(!f->writable_) {
        return E_BADF;
    }

    // grab memfile lock to sync with memfile_vnode::read()
    spinlock_guard mf_guard(mf_->lock_);
    if(mf_->set_length(f->wpos_ + sz) == E_NOSPC) {
        return E_NOSPC;
    }
    // avoid overflow
    sz = mf_->capacity_ - f->wpos_;
    // write 'sz' bytes from 'addr' to memfile
    memcpy(&mf_->data_[f->wpos_], reinterpret_cast<void*>(addr), sz);
    f->wpos_ += sz;
    memset(&mf_->data_[f->wpos_], 0, 1);

    return sz;
}

uintptr_t diskfile_vnode::read(file_descriptor *f, uintptr_t addr, size_t sz) {
    if(!f->readable_) return E_BADF;

    // concurrent reads are not allowed
    ino_->lock_read();
    if(!ino_->size) {
        ino_->unlock_read();
        return 0;
    }

    chkfs_fileiter it(ino_);

    size_t nread = 0;
    unsigned char* buf = reinterpret_cast<unsigned char*>(addr);

    while(nread < sz) {
        // copy data from current block
        if(bcentry* e = it.find(f->rpos_).get_disk_entry()) {
            unsigned b = it.block_relative_offset();
            size_t ncopy = min(
                size_t(ino_->size - it.offset()),
                chkfs::blocksize - b,
                sz - nread
            );
            memcpy(buf + nread, e->buf_ + b, ncopy);
            e->put();

            nread += ncopy;
            f->rpos_ += ncopy;
            // TODO: why do we do this?
            if(f->writable_) f->wpos_ += ncopy;
            if(ncopy == 0) break;
        } else {
            break;
        }
    }

    ino_->unlock_read();
    return nread;
}

uintptr_t diskfile_vnode::write(file_descriptor *f, uintptr_t addr, size_t sz) {
    if(!sata_disk) return E_IO;
    if(!f->writable_) return E_BADF;

    // synchronize access to inode's size and data references
    ino_->lock_write();
    chkfs_fileiter it(ino_);

    // extend file if necessary
    uint32_t allocated_sz = round_up(ino_->size, chkfs::blocksize);
    if(allocated_sz < size_t(f->wpos_ + sz)) {
        // calculate number of blocks to allocate
        size_t alloc_sz = f->wpos_ + sz - allocated_sz;
        unsigned nbs = round_up(alloc_sz, chkfs::blocksize) / chkfs::blocksize;

        // allocate extent and get its first block number
        auto& fs = chkfsstate::get();
        chkfs::blocknum_t bn = fs.allocate_extent(nbs);

        // append extent to the end of the file
        it.find(-1).insert(bn, nbs);
    }

    // update file true size, if necessary
    if(ino_->size < size_t(f->wpos_ + sz)) {
        // sync writing to buffer cache (ino_->size is in buffer cache)
        ino_->entry()->get_write();
        ino_->size = f->wpos_ + sz;
        ino_->entry()->put_write();
    }

    size_t nwritten = 0;
    unsigned char* buf = reinterpret_cast<unsigned char*>(addr);
    
    while(nwritten < sz) {
        if(bcentry* e = it.find(f->wpos_).get_disk_entry()) {
            e->get_write();
            unsigned b = it.block_relative_offset();
            size_t ncopy = min(
                size_t(ino_->size - it.offset()),       // bytes left in the file
                chkfs::blocksize - b,                   // bytes left in the block
                sz - nwritten                           // bytes left in the request
            );
            memcpy(e->buf_ + b, buf + nwritten, ncopy);
            e->put_write();
            e->put();

            nwritten += ncopy;
            f->wpos_ += ncopy;
            if(f->readable_) f->rpos_ += nwritten;
            if(!ncopy) break;
        } else {
            break;
        }
    }

    ino_->unlock_write();
    return nwritten;
}