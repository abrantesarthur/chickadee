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


    // test shmat
    const char* aligned_addr = reinterpret_cast<const char*>(
        round_up(reinterpret_cast<uintptr_t>(end), PAGESIZE)
    );

    // console_printf("shmat with unaligned shmaddr fails\n");
    // void* shmaddr = shmat(shmid0, aligned_addr + 1);
    // if(shmaddr) assert(false);

    // console_printf("shmat with null shmaddr fails\n");
    // shmaddr = shmat(shmid0, 0);
    // if(shmaddr) assert(false);

    // console_printf("shmat with overflowing shmaddr fails\n");
    // shmaddr = shmat(shmid0, reinterpret_cast<void*>(memsize_virtual));
    // if(shmaddr) assert(false);
    // int shmid2 = shmget(IPC_PRIVATE, 2 * PAGESIZE);
    // assert_eq(shmid2, 2);
    // // can't allocate beyond virtual memory size
    // shmaddr = shmat(shmid2, reinterpret_cast<void*>(memsize_virtual - PAGESIZE));
    // if(shmaddr) assert(false);
    // // can't allocate last virtual memory page
    // shmaddr = shmat(shmid2, reinterpret_cast<void*>(memsize_virtual - 2 * PAGESIZE));
    // if(shmaddr) assert(false);
    // // this works
    // shmaddr = shmat(shmid2, reinterpret_cast<void*>(memsize_virtual - 3 * PAGESIZE));
    // if(!shmaddr) assert(false);
    // // cleanup
    // assert_eq(shmdt(shmaddr), 0);


    // console_printf("shmat with already mapped shmaddr fails\n");
    // int shmid3 = shmget(IPC_PRIVATE);
    // assert_eq(shmid3, 2);
    // // it works the first time
    // shmaddr = shmat(shmid3, aligned_addr);
    // if(!shmaddr) assert(false);
    // // it also works the second time for the same segment
    // shmaddr = shmat(shmid3, aligned_addr);
    // if(!shmaddr) assert(false);
    // // it doesn't work for another segment
    // int shmid4 = shmget(IPC_PRIVATE);
    // assert_eq(shmid4, 3);
    // shmaddr = shmat(shmid4, aligned_addr);
    // if(shmaddr) assert(false);

    // // test shmdt
    // console_printf("shmdt with unmapped segment fails\n");
    // // get shmaddr of shmid3 segment
    // shmaddr = shmat(shmid3, aligned_addr);
    // if(!shmaddr) assert(false);
    // // unmaping shmid3 segment once works
    // assert_eq(shmdt(shmaddr), 0);
    // // unmaping shmid3 segment twice fails
    // assert_eq(shmdt(shmaddr), -1);

    // console_printf("shmat works after shmdt\n");
    // // map shmid5 segment
    // int shmid5 = shmget(IPC_PRIVATE);
    // assert_eq(shmid5, 2);
    // shmaddr = shmat(shmid5, aligned_addr);
    // if(!shmaddr) assert(false);
    // // trying remapping aligned_addr before shmdt fails
    // int shmid6 = shmget(IPC_PRIVATE);
    // assert_eq(shmid6, 4);
    // shmaddr = shmat(shmid6, aligned_addr);
    // if(shmaddr) assert(false);
    // // trying remapping aligned_addr after shmdt succeeds
    // shmaddr = shmat(shmid5, aligned_addr);
    // if(!shmaddr) assert(false);
    // assert_eq(shmdt(shmaddr), 0);
    // shmaddr = shmat(shmid6, aligned_addr);
    // if(!shmaddr) assert(false);


    // test that forking copies over segments

    // allocate new segment
    int shmid7 = shmget(IPC_PRIVATE);
    assert_eq(shmid7, 0);
    // map shared data
    void* shared_data = shmat(shmid7, aligned_addr);
    if(!shared_data) assert(false);
    int* d = reinterpret_cast<int*>(shared_data);
    *d = 2;
    // fork child
    pid_t p = sys_fork();
    if(p == 0) {
        // assert that child has aceess to shared_data
        int chshmid7 = shmget(shmid7);
        assert_eq(chshmid7, shmid7);
        void* chshared_data = shmat(shmid7, aligned_addr);
        assert_eq(chshared_data, shared_data);

        assert_eq(*d, 2);

        sys_msleep(50);
        // update child's view of the data
        *d = 3;

        sys_exit(0);
    }
    assert_eq(*d, 2);
    sys_msleep(200);
    // child update must be visible in the parent
    assert_eq(*d, 3);
    console_printf("testshm succeeded.\n");
    sys_exit(0);
}