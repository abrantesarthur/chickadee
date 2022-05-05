#include "u-lib.hh"

int shmget(int key, size_t size) {
    return make_syscall(SYSCALL_SHMGET, key, size);
}

void* shmat(int shmid, const void* shmaddr) {
    uintptr_t a = reinterpret_cast<uintptr_t>(shmaddr);
    return reinterpret_cast<void*>(make_syscall(SYSCALL_SHMAT, shmid, a));
}

int shmdt(const void* shmaddr) {
    return make_syscall(SYSCALL_SHMDT, reinterpret_cast<uintptr_t>(shmaddr));
}