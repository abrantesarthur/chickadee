#include "u-lib.hh"

void process_main() {
    int r;

    // kalloc tests
    r = sys_testkalloc(0);
    assert_eq(r, 0);

    r = sys_testkalloc(1);
    assert_eq(r, 0);

    r = sys_testkalloc(2);
    assert_eq(r, 0);

    r = sys_testkalloc(3);
    assert_eq(r, 0);

    r = sys_testkalloc(4);
    assert_eq(r, 0);

    r = sys_testkalloc(5);
    assert_eq(r, 0);

    r = sys_testkalloc(6);
    assert_eq(r, 0);

    r = sys_testkalloc(7);
    assert_eq(r, 0);

    // wild alloc tests: they should cause assertion failures!
    // sys_wildalloc(1);
    // sys_wildalloc(2);
    // sys_wildalloc(3);


    console_printf("testkalloc succeeded.\n");

    sys_exit(0);
}
