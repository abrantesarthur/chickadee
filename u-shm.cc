#include "u-lib.hh"

int shmget(int key, size_t size) {
    return make_syscall(SYSCALL_SHMGET, key, size);
}