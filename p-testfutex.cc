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
    message("clone");
    pid_t t = clone(thread1a);
    assert_gt(t, 0);
    
    message("thread1a should block");
    
    // sleep to make suree thread1a indeed blocked
    sys_msleep(250);

    message("thread1a blocked");

     // trying to wake up threads without modifying futex fails
    message("try waking up thread1a");
    int ts = sys_futex(reinterpret_cast<std::atomic<int>*>(&futex), FUTEX_WAKE, 1);
    // tread1a is awaken to check whether 'futex' changed
    assert_eq(ts, 1);

    // thread1a will block again
    sys_msleep(250);

    message("waking up thread1a failed as expected");

    // this should quit thread1a
    sys_exit(1);
}

static int thread2a(void* x) {
    message("starting thread2a");

    // futex starts as 0
    assert_eq(futex, 0);

    assert_memeq(msg, "thread2a should block\n", 22);

    // sys_futex should block as long as futex is still 0
    sys_futex(reinterpret_cast<std::atomic<int>*>(&futex), FUTEX_WAIT, 0);

    assert_memeq(msg, "thread2a blocked\n", 17);
    message("thread2a blocked");

    // assertion will fail if sys_futex didn't block
    assert(false);
}

static void test2() {
    msg = "thread2a should block\n";

    message("clone");
    pid_t t = clone(thread2a);
    assert_gt(t, 0);

    message("thread2a should block");

    // sleep to make suree thread2a indeed blocked
    sys_msleep(300);

    msg = "thread2a blocked\n";


    // wake thread2a
    int ts = sys_futex(reinterpret_cast<std::atomic<int>*>(&futex), FUTEX_WAKE, 1);

    // this should work even though we didn't update futex
    assert_eq(ts, 1);

    // send message to child so it can verify that is blocked
    msg = "Thread2a blocked\n";

    // sleep to make suree thread2a verifies the message
    sys_msleep(50);

    // TODO: test that it also works if you do update futex

    // TODO: passing more than 'val' still wakes up only the amount of threads sleeping

    // this should quit thread2a
    sys_exit(2);
}

void process_main() {
    assert_eq(sys_getppid(), 1);

    // test FUTEX_WAIT

    message("testing that FUTEX_WAIT works");

    pid_t p = sys_fork();
    assert_ge(p, 0);
    if(p == 0) {
        test1();
    }

    int status = 0;
    pid_t ch = sys_waitpid(p, &status);
    assert_eq(ch, p);
    assert_eq(status, 1);

    // test FUTEX_WAKE

    message("testing that FUTEX_WAKE works");
    p = sys_fork();
    assert_ge(p, 0);
    if(p == 0) {
        test2();
    }

    status = 0;
    ch = sys_waitpid(p, &status);
    assert_eq(ch, p);
    assert_eq(status, 2);

    // TODO: test that if a thread is killed by a parent, it is also removed from the waitqueue

    console_printf("testfutex succeeded.\n");
    sys_exit(0);
}
