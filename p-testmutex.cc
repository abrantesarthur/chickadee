#include "u-lib.hh"

extern uint8_t end[];

void process_main() {

    // create a futex-based lock
    mutex m;

    // child grabs the lock first
    pid_t p = sys_fork();
    if(p == 0) {
        assert_eq(m.trylock(), 1);
        sys_msleep(20);
        m.unlock();
        sys_exit(0);
    }
    sys_msleep(20);
    assert_eq(m.trylock(), 0);
    sys_msleep(50);
    assert_eq(m.trylock(), 1);
    pid_t ch = sys_waitpid(p);
    assert_eq(ch, p);


    // test with lock
    m.unlock();
    m.lock();

    p = sys_fork();
    if(p == 0) {
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