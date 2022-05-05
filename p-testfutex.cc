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

static int thread2a(void* x) {
    message("starting thread2a");

    // futex starts as 0
    assert_eq(futex, 0);

    assert_memeq(msg, "thread2a should block\n", 22);

    // sys_futex should block as long as futex is still 0
    sys_futex(reinterpret_cast<std::atomic<int>*>(&futex), FUTEX_WAIT, 0);

    assert_memeq(msg, "thread2a blocked\n", 17);
    message("thread2a blocked");

    assert_ne(futex, 0);

    // notify parent that thread2a unblocked
    msg = "thread2a unblocked\n";

    sys_texit(0);
}


static void test2() {
    msg = "thread2a should block\n";

    pid_t t = clone(thread2a);
    assert_gt(t, 0);

    message("thread2a should block");

    // sleep to make suree thread2a indeed blocked
    sys_msleep(300);

    msg = "thread2a blocked\n";

    // passing a 0 'val' shouldn't wake up any threads
    int ts = sys_futex(reinterpret_cast<std::atomic<int>*>(&futex), FUTEX_WAKE, 0);
    assert_eq(ts, 0);

    // wake thread2a without modifying futex
    ts = sys_futex(reinterpret_cast<std::atomic<int>*>(&futex), FUTEX_WAKE, 1);
    assert_eq(ts, 1);

    // sleep to make sure that thread2a has time to receive wake call
    sys_msleep(50);

    // thread2 should remaing blocked
    assert_memeq(msg, "thread2a blocked\n", 17);
    message("waking up thread2a failed as expected");

    // wake thread2a after modifying futex
    futex = 2;
    ts = sys_futex(reinterpret_cast<std::atomic<int>*>(&futex), FUTEX_WAKE, 10);

    // should still wake up only one thread, even though 'val' was 10
    assert_eq(ts, 1);

    // sleep to make sure that thread2a unblocks
    sys_msleep(20);

     // thread2 should ublocked
    assert_memeq(msg, "thread2a unblocked\n", 19);
    message("woke up thread2a as expected");

    // reset futex
    futex = 0;

    // this should quit all threads
    sys_exit(2);
}

static int thread3a(void* x) {
    message("starting thread3a");

    // futex starts as 0
    assert_eq(futex, 0);

    assert_memeq(msg, "thread3a should block\n", 22);

    // sys_futex should block as long as futex is still 0
    sys_futex(reinterpret_cast<std::atomic<int>*>(&futex), FUTEX_WAIT, 0);

    assert_eq(futex, 1);

    msg = "thread3a unblocked\n";

    sys_texit(0);
}

static void test3() {

    msg = "thread3a should block\n";

    // span a 4 threads
    pid_t t1 = clone(thread3a);
    pid_t t2 = clone(thread3a);
    pid_t t3 = clone(thread3a);
    pid_t t4 = clone(thread3a);
    assert_gt(t1, 0);
    assert_gt(t2, 0);
    assert_gt(t3, 0);
    assert_gt(t4, 0);

    // wait for all threads to block
    sys_msleep(20);

    // when 'val' is 0, no threads should unblock
    futex = 1;
    int ts = sys_futex(reinterpret_cast<std::atomic<int>*>(&futex), FUTEX_WAKE, 0);
    assert_eq(ts, 0);

    sys_msleep(20);

    // assert that no thread3a unblocked
    assert_memeq(msg, "thread3a should block\n", 22);

    // unblock 2 threads
    ts = sys_futex(reinterpret_cast<std::atomic<int>*>(&futex), FUTEX_WAKE, 2);
    assert_eq(ts, 2);

    sys_msleep(20);

    // assert some thread
    assert_memeq(msg, "thread3a unblocked\n", 22);
    
    msg = "threads are still blocked\n";

     // unblock remaining threads
    ts = sys_futex(reinterpret_cast<std::atomic<int>*>(&futex), FUTEX_WAKE, 10);
    // only 2 should unblock, even though 'val' was 10
    assert_eq(ts, 2);

    sys_msleep(20);

    // assert some thread
    assert_memeq(msg, "thread3a unblocked\n", 22);
    msg = "no thread is still blocked\n";

    // this shouldn't unblock any more threads
    ts = sys_futex(reinterpret_cast<std::atomic<int>*>(&futex), FUTEX_WAKE, 10);
    assert_eq(ts, 0);

    sys_msleep(20);

    assert_memeq(msg, "no thread is still blocked\n", 27);

    // this should exit all thread3as
    sys_exit(3);
}

void process_main() {
    assert_eq(sys_getppid(), 1);

    mutex m;


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

    p = sys_fork();
    assert_ge(p, 0);
    if(p == 0) {
        test3();
    }
    status = 0;
    ch = sys_waitpid(p, &status);
    assert_eq(ch, p);
    assert_eq(status, 3);

    // TODO: test that if a thread is killed by a parent, it is also removed from the waitqueue

    console_printf("testfutex succeeded.\n");
    sys_exit(0);
}
