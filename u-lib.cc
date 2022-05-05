#include "u-lib.hh"

// dprintf
//    Construct a string from `format` and pass it to `sys_write(fd)`.
//    Returns the number of characters printed, or E_2BIG if the string
//    could not be constructed.

int dprintf(int fd, const char* format, ...) {
    char buf[513];
    va_list val;
    va_start(val, format);
    size_t n = vsnprintf(buf, sizeof(buf), format, val);
    if (n < sizeof(buf)) {
        return sys_write(fd, buf, n);
    } else {
        return E_2BIG;
    }
}


// printf
//    Like `printf(1, ...)`.

int printf(const char* format, ...) {
    char buf[513];
    va_list val;
    va_start(val, format);
    size_t n = vsnprintf(buf, sizeof(buf), format, val);
    if (n < sizeof(buf)) {
        return sys_write(1, buf, n);
    } else {
        return E_2BIG;
    }
}


// panic, assert_fail
//     Call the SYSCALL_PANIC system call so the kernel loops until Control-C.

void panic(const char* format, ...) {
    va_list val;
    va_start(val, format);
    char buf[160];
    memcpy(buf, "PANIC: ", 7);
    int len = vsnprintf(&buf[7], sizeof(buf) - 7, format, val) + 7;
    va_end(val);
    if (len > 0 && buf[len - 1] != '\n') {
        strcpy(buf + len - (len == (int) sizeof(buf) - 1), "\n");
    }
    int cpos = consoletype == CONSOLE_NORMAL ? -1 : CPOS(23, 0);
    (void) console_printf(cpos, 0xC000, "%s", buf);
    sys_panic(nullptr);
}

int error_vprintf(int cpos, int color, const char* format, va_list val) {
    return console_vprintf(cpos, color, format, val);
}

void assert_fail(const char* file, int line, const char* msg,
                 const char* description) {
    if (consoletype != CONSOLE_NORMAL) {
        cursorpos = CPOS(23, 0);
    }
    if (description) {
        error_printf("%s:%d: %s\n", file, line, description);
    }
    error_printf("%s:%d: user assertion '%s' failed\n", file, line, msg);
    sys_panic(nullptr);
}

/**
 * mutex
 * init
 * trylock
 * destroy 
 */

void mutex::lock() {
    int c = compare_exchange_strong(&futex_, 0, 1);

    // if we grabbed the lock, return without blocking
    if(c == 0) return;

    // othwerwise, block until lock is free
    do {
        // signal that we are waiting for the lock
        if(c == 2 || compare_exchange_strong(&futex_, 1, 2) != 0) {
            // block until the lock is freed
            sys_futex(&futex_, FUTEX_WAIT, 2);
        }
        // we get here if futex_ becomes 0. Hence, we try grabbing it again.
    } while((c = compare_exchange_strong(&futex_, 0, 2)) != 0);
    
    // wet get here if we grabbed the lock in the while loop
}

// mutex::trylock()
        // try grabbing the lock without blocking. Returns true if success
bool mutex::trylock() {
    return compare_exchange_strong(&futex_, 0, 1) == 0;
}

void mutex::unlock() {
    if(futex_.fetch_sub(1) != 1) {
        futex_.store(0);
        sys_futex(&futex_, FUTEX_WAKE, 1);
    }
}

int mutex::compare_exchange_strong(std::atomic<int>* futex, int expected, int desired) {
    int* e = &expected;
    // if value at futex is 'expected', replace it with 'desired'
    // otherwise, load 'e' with contained value
    std::atomic_compare_exchange_strong(futex, e, desired);
    return *e;
}

// // sys_futex(path, flags, val)
// inline int sys_futex(std::atomic<int>* uaddr, int futex_op, int val) {
//     access_memory(uaddr);
//     return make_syscall(SYSCALL_FUTEX, reinterpret_cast<uintptr_t>(uaddr),
//                         futex_op, val);
// }

