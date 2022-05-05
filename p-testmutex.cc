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

        // trylock fails
        assert_eq(m.trylock(), 0);

        sys_msleep(50);

        assert_eq(m.trylock(), 1);


        sys_exit(0);
    }

    sys_msleep(20);

    m.unlock();

    console_printf("testmutex succeeded.\n");
    sys_exit(0);
}