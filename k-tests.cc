#include "kernel.hh"

/**********************************************************
 BUDDY ALLOCATOR TESTS
***********************************************************/

int proc::syscall_testkalloc(regstate* regs) {
    int tcase = regs->reg_rdi; // type of test to run
    
    int num_allocs = 50; // number of allocations to make per test
    void* ptr_arr[num_allocs]; // ptr_arr to save, which will be freed afterwards

    switch (tcase) {
        case 0: { 

            // simple test case 1
            // straigforward pagesize allocations, followed by frees

            uint64_t sz = PAGESIZE;
            for (int i = 0; i < num_allocs; ++i) {
                ptr_arr[i] = kalloc(sz);
            }

            for (int i = 0; i < num_allocs; ++i) {
                kfree(ptr_arr[i]);
            }

            console_printf("======= TEST CASE [0] for PROCESS [%d] COMPLETED =======\n", this->id_);
            break;
        }

        case 1: { 

            // randomized general case 1
            // Multiples of PAGESIZE allocations
            // the order of the allocation is randomized
            // ranges between max order to min order

            int ro = 0;
            uint64_t sz = PAGESIZE;

            for (int i = 0; i < num_allocs; ++i) {
                ro = rand(MIN_ORDER, MAX_ORDER);
                sz = 1 << ro;
                ptr_arr[i] = kalloc(sz);
            }

            for (int i = 0; i < num_allocs; ++i) {
                kfree(ptr_arr[i]);
            }

            console_printf("======= TEST CASE [1] for PROCESS [%d] COMPLETED =======\n", this->id_);
            break;
        }

        case 2: { 

            // randomized general case 2
            // non-multiples of PAGESIZE
            // can range fom 4096 bytes to 2^21 bytes

            uint64_t sz;

            for (int i = 0; i < num_allocs; ++i) {
                sz = rand(1 << MIN_ORDER, 1 << MAX_ORDER);
                ptr_arr[i] = kalloc(sz);
            }

            for (int i = 0; i < num_allocs; ++i) {
                kfree(ptr_arr[i]);
            }
            console_printf("======= TEST CASE [2] for PROCESS [%d] COMPLETED =======\n", this->id_);
            break;
        }

        case 3: { 
            // randomized general case 3
            // smaller but randomized page size allocations
            // sz requested by random generator is constrained to 
            // smaller allocation sizes thus, more allocations overall

            uint64_t sz;
            for (int j = 0; j < 10; ++j) {
                for (int i = 0; i < num_allocs; ++i) {
                    sz = rand(1 << MIN_ORDER, 1 << (MAX_ORDER - 5));
                    ptr_arr[i] = kalloc(sz);
                }

                for (int i = 0; i < num_allocs; ++i) {
                    kfree(ptr_arr[i]);
                }
            }
            console_printf("======= TEST CASE [3] for PROCESS [%d] COMPLETED =======\n", this->id_);
            break;
        }
        // ----- SLAB ALLOCATOR TESTS -----
        case 4: {

            // slab allocator random test 1
            // sizes of only smaller slabs are allocated
            // we expect here that by the time 50 allocations are requested
            // the smaller slabs will be used up and larger slabs will be
            // allocated until those are used up as well, at which point the 
            // buddy allocator will take over

            for (int j = 0; j < 10; ++j) {
                uint64_t sz;
                for (int i = 0; i < num_allocs; ++i) {
                    sz = rand(1 << 2, 1 << 6);
                    ptr_arr[i] = kalloc(sz);
                }
                for (int i = 0; i < num_allocs; ++i) {
                    kfree(ptr_arr[i]);
                }
            }
            console_printf("======= {SLAB} TEST CASE [4] for PROCESS [%d] COMPLETED =======\n", this->id_);
            break;

        }

        case 5: {

            // randomized slab allocator test 2
            // now we start with the larger 512 byte allocation sizes
            // we expect that these chunks will be used up quickly and
            // then the buddy allocator will take over

            for (int j = 0; j < 10; ++j) {
                uint64_t sz;
                for (int i = 0; i < num_allocs; ++i) {
                    sz = rand(1 << 7, (1 << 9) - 8);
                    ptr_arr[i] = kalloc(sz);
                }
                for (int i = 0; i < num_allocs; ++i) {
                    kfree(ptr_arr[i]);
                }
            }
            console_printf("======= {SLAB} TEST CASE [5] for PROCESS [%d] COMPLETED =======\n", this->id_);
            break;
        }

        case 6: { 

            // randomized slab allocator test 3
            // we randomly switch between larger slab sizes and smaller
            // slab sizes untill the buddy allocator takes over

            for (int j = 0; j < 10; ++j) {
                uint64_t sz;
                for (int i = 0; i < num_allocs; ++i) {
                    sz = rand(1 << 2, (1 << 9)- 8);
                    ptr_arr[i] = kalloc(sz);
                }
                for (int i = 0; i < num_allocs; ++i) {
                    kfree(ptr_arr[i]);
                }
            }
            console_printf("======= {SLAB} TEST CASE [6] for PROCESS [%d] COMPLETED =======\n", this->id_);
            break;
        }

        case 7: {

            // randomized slab allocator test 4
            // here we switch constantly between the large slab, small slab
            // and the buddy allocator at the same time

            for (int j = 0; j < 10; ++j) {
                uint64_t sz;
                for (int i = 0; i < num_allocs; ++i) {
                    sz = rand(1 << 2, 1 << (MIN_ORDER + 2));
                    ptr_arr[i] = kalloc(sz);
                }
                for (int i = 0; i < num_allocs; ++i) {
                    kfree(ptr_arr[i]);
                }
            }
            console_printf("======= {SLAB} TEST CASE [7] for PROCESS [%d] COMPLETED =======\n", this->id_);
            break;
        }

        default: {
            // if an incorrect test case number is called
            console_printf("======= ERROR: Test case number %d not implemented  =======\n", tcase);
            break;
        }
    }
    return 0;
}


// proc::syscall_wildalloc(regs)
//  edge test cases for the buddy allocator
//  tests the invariants themselves and 
//  false system calls
//  the goal is catch the errors with our own assertions
//  rather than the given chicakdee system code assertions
int proc::syscall_wildalloc(regstate *regs) {
    int c = regs->reg_rdi;
    switch (c) {
        case 1: {

            // here we test if our invariant statement and buddy checker
            // can catch the double free that we plan trigger
            // this can be caught because we have a list of the pointers returned
            // by our system

            console_printf("[SYSCALL_WILDALLOC] Wild allocation 1: free of un-allocated ptr\n");
            void* nonptr = nullptr;
            kfree(nonptr);
            // kfree should not do anything when given a nullptr

            void *nasty_ptr = kalloc(PAGESIZE);
            kfree(nasty_ptr);
            kfree(nasty_ptr);
            // this double free should be caught by the local check
            // located in kfree
            
            break;
        }

        case 2: {

            // here we attempt to free a pointer that is offset from 
            // the actual page alignment, so it should no longer
            // point towards the struct

            console_printf("[SYSCALL_WILDALLOC] Wild allocation 2: free non page-aligned pointer\n");
            void *ptr = kalloc(PAGESIZE);
            uint64_t addr = kptr2pa(ptr) + 6;
            // here we offset the value of the actual struct by a non-power of 2
            kfree(pa2kptr<void*>(addr));
            break;
        }

        case 3: { 

            // here we attempt to free in the middle of some allocation 
            // the pointer should point to some address not recorded on the list

            console_printf("[SYSCALL_WILDALLOC] Wild allocation 3: offset free\n");
            void *ptr = kalloc(2*PAGESIZE);
            uint64_t addr = reinterpret_cast<uint64_t>(ptr) + PAGESIZE;
            kfree(reinterpret_cast<void*>(addr));
            break;
        }

        default: {
            console_printf("[SYSCALL_WILDALLOC] No wild tests were run.\n");
        }
    }
    return 0;
}

