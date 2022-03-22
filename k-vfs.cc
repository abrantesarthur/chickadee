#include "k-vfs.hh"
#include "k-devices.hh"
#include "k-wait.hh"

// TODO: add keyboardstate_wq

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
    assert(f->vnode_ && f->vnode_->data_);
    bounded_buffer* b = reinterpret_cast<bounded_buffer*>(f->vnode_->data_);
    return b->read(reinterpret_cast<char*>(addr), sz);
}

uintptr_t pipe_vnode::write(file_descriptor* file, uintptr_t addr, size_t sz) {
    // avoid reading from write end of the pipe
    if(file->readable_) {
        return E_BADF;
    }
    assert(file->vnode_ && file->vnode_->data_);
    bounded_buffer* b = reinterpret_cast<bounded_buffer*>(file->vnode_->data_);
    return b->write(reinterpret_cast<char*>(addr), sz);
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