#include "u-lib.hh"
#include <atomic>

extern uint8_t end[];

std::atomic_flag message_lock;
int pfd[2] = {-1, -1};
int futex = 0;
const char* msg;

static void message(const char* x) {
    while (message_lock.test_and_set()) {
        pause();
    }
    console_printf("T%d (P%d): %s\n", sys_gettid(), sys_getpid(), x);
    message_lock.clear();
}


pid_t clone(int (*function)(void*)) {
    char* stack1 = reinterpret_cast<char*>(
        round_up(reinterpret_cast<uintptr_t>(end), PAGESIZE) + 16 * PAGESIZE
    );
    int r = sys_page_alloc(stack1);
    assert_eq(r, 0);

    pid_t t = sys_clone(function, pfd, stack1 + PAGESIZE);
    return t;
}


static int thread1a(void* x) {
    message("starting thread1a");

    // futex starts as 0
    assert_eq(futex, 0);

    // sys_futex should block as long as futex is still 0
    sys_futex(reinterpret_cast<std::atomic<int>*>(&futex), FUTEX_WAIT, 0);

    message("thread1a did not block");

    // assertion will fail if sys_futex didn't block
    assert(false);
}

static void test1() {
    // clone a thread
    pid_t t = clone(thread1a);
    assert_gt(t, 0);
    
    message("thread1a should block");
    
    // sleep to make suree thread1a indeed blocked
    sys_msleep(250);

    message("thread1a blocked");

    // this should quit thread1a
    sys_exit(1);
}

void process_main() {

    // TODO: this returns a segment with size PAGESIZE
    int segid = shmget(IPC_PRIVATE, 1);
    

    console_printf("segid: %d\n", segid);

    segid = shmget(1);

    console_printf("segid: %d\n", segid);

    sys_exit(0);
}

/**
 * TESTS
 *  calling shmget passing its id returns its id
 *  caling shmget passing IPC_PRIVATE returns new id
 *  caling shmget multiple times passing IPC_PRIVATE eventually returns -1
 * 
 */