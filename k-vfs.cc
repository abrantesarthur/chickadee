#include "k-vfs.hh"
#include "k-devices.hh"
#include "k-wait.hh"

// TODO: add wait queues

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