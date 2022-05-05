#include "u-mutex.hh"
#include <atomic>
#include "u-lib.hh"

extern uint8_t end[];

void process_main() {
    // create a futex-based lock
    mutex m;

    // grab it
    m.lock();

    // fork child
    pid_t p = sys_fork();
    if(p == 0) {

        sys_exit(0);
    }

    console_printf("testmutex succeeded.\n");
    sys_exit(0);
}