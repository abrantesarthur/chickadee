#include "u-lib.hh"
#include <atomic>

extern uint8_t end[];

void process_main() {
    const int memsize_virtual = 0x300000;

    // // test shmget

    // console_printf("shmget with IPC_PRIVATE returns new segment\n"); 
    // int shmid0 = shmget(IPC_PRIVATE);
    // assert_eq(shmid0, 0);
    // int shmid1 = shmget(IPC_PRIVATE);
    // assert_eq(shmid1, 1);

    // console_printf("shmget with valid id works\n"); 
    // assert_eq(shmget(shmid0), shmid0);

    // console_printf("shmget with invalid id fails\n"); 
    // assert_eq(shmget(3), -1);


    // // test shmat
    // char* aligned_addr = reinterpret_cast<char*>(
    //     round_up(reinterpret_cast<uintptr_t>(end), PAGESIZE)
    // );

    // console_printf("shmget with unaligned shmaddr fails\n");
    // void* shmaddr = shmat(shmid0, aligned_addr + 1);
    // if(shmaddr) assert(false);

    // console_printf("shmget with null shmaddr fails\n");
    // shmaddr = shmat(shmid0, 0);
    // if(shmaddr) assert(false);

    console_printf("shmget with overflowing shmaddr fails\n");
    // shmaddr = shmat(shmid0, reinterpret_cast<void*>(memsize_virtual));
    // if(shmaddr) assert(false);
    int shmid2 = shmget(IPC_PRIVATE, 2 * PAGESIZE);
    assert_eq(shmid2, 0);
    // shmaddr = shmat(shmid2, reinterpret_cast<void*>(memsize_virtual - PAGESIZE));
    // if(shmaddr) assert(false);
    // shmaddr = shmat(shmid2, reinterpret_cast<void*>(memsize_virtual - 2 * PAGESIZE));
    // if(shmaddr) assert(false);
    void* shmaddr = shmat(shmid2, reinterpret_cast<void*>(memsize_virtual - 3 * PAGESIZE));
    if(!shmaddr) assert(false);


     
    // test shmdt
    // shmdt(shmaddr);



    console_printf("testshm succeeded.\n");
    sys_exit(0);
}

/**
 * TESTS
 *  caling shmget passing IPC_PRIVATE returns new id
 *  caling shmget multiple times passing IPC_PRIVATE eventually returns -1
 * 
 */