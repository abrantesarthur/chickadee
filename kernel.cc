#include "kernel.hh"
#include "k-ahci.hh"
#include "k-apic.hh"
#include "k-chkfs.hh"
#include "k-chkfsiter.hh"
#include "k-devices.hh"
#include "k-vmiter.hh"
#include "obj/k-firstprocess.h"

/**
 * TODO 1: add cpuindex_ to proc struct
 */

// kernel.cc
//
//    This is the kernel.

// # timer interrupts so far on CPU 0
std::atomic<unsigned long> ticks;
proc* init_process = nullptr;

// wait queues
wait_queue wait_child_exit_wq;
#define SLEEP_WQS_COUNT 10
wait_queue sleep_wqs[SLEEP_WQS_COUNT];

static void tick();
static void boot_process_start(pid_t pid, const char* program_name);
static void init_process_start();

// kernel_start(command)
//    Initialize the hardware and processes and start running. The `command`
//    string is an optional string passed from the boot loader.

void kernel_start(const char* command) {
    init_hardware();
    consoletype = CONSOLE_NORMAL;
    console_clear();

    // set up process descriptors
    for (pid_t i = 0; i < NPROC; i++) {
        ptable[i] = nullptr;
    }

    // start init process
    init_process_start();

    // start boot process
    boot_process_start(2, CHICKADEE_FIRST_PROCESS);

    // start running processes
    cpus[0].schedule(nullptr);
}

// init_process_function()
//      function that the init process executes
void init_process_function() {
    // sti();
    while(init_process->syscall_waitpid(0, nullptr, W_NOHANG) != E_CHILD) {
    }
    process_halt();
}


// init_process_start()
//      initialize the init process and enqueue it to run
void init_process_start() {
    init_process = knew<proc>();
    assert(init_process);
    init_process->init_kernel(1, init_process_function);
    init_process->ppid_ = init_process->id_;
    {
        spinlock_guard guard(ptable_lock);
        assert(!ptable[1]);
        ptable[1] = init_process;
    }
    cpus[init_process->id_ % ncpu].enqueue(init_process);
}

// boot_process_start(pid, name)
//    Load application program `name` as process number `pid`.
//    This loads the application's code and data into memory, sets its
//    %rip and %rsp, gives it a stack page, and marks it as runnable.
//    Only called at initial boot time.

void boot_process_start(pid_t pid, const char* name) {
    // look up process image in initfs
    memfile_loader ld(memfile::initfs_lookup(name), kalloc_pagetable());
    assert(ld.memfile_ && ld.pagetable_);
    int r = proc::load(ld);
    assert(r >= 0);

    // allocate process, initialize memory
    proc* p = knew<proc>();
    p->init_user(pid, ld.pagetable_);
    p->regs_->reg_rip = ld.entry_rip_;

    // set ppid to init process ID
    assert(init_process);
    p->ppid_ = init_process->id_;

    // initiazlie file descriptor table
    p->init_fd_table();

    // add boot process to init process children list
    init_process->children_.push_front(p);

    void* stkpg = kalloc(PAGESIZE);
    assert(stkpg);
    vmiter(p, MEMSIZE_VIRTUAL - PAGESIZE).map(stkpg, PTE_PWU);
    p->regs_->reg_rsp = MEMSIZE_VIRTUAL;

    // make console user-accessible so console_printf() works
    if (vmiter(p, ktext2pa(console)).try_map(CONSOLE_ADDR, PTE_PWU) < 0) {
        assert(false);
    }

    // add to process table (requires lock in case another CPU is already
    // running processes)
    {
        spinlock_guard guard(ptable_lock);
        assert(!ptable[pid]);
        ptable[pid] = p;
    }

    // add to run queue
    cpus[pid % ncpu].enqueue(p);
}

// proc::exception(reg)
//    Exception handler (for interrupts, traps, and faults).
//
//    The register values from exception time are stored in `reg`.
//    The processor responds to an exception by saving application state on
//    the current CPU stack, then jumping to kernel assembly code (in
//    k-exception.S). That code transfers the state to the current kernel
//    task's stack, then calls proc::exception().

void proc::exception(regstate* regs) {
    // It can be useful to log events using `log_printf`.
    // Events logged this way are stored in the host's `log.txt` file.

    // Record most recent user-mode %rip.
    if ((regs->reg_cs & 3) != 0) {
        recent_user_rip_ = regs->reg_rip;
    }

    // Show the current cursor location.
    consolestate::get().cursor();

    // Actually handle the exception.
    switch (regs->reg_intno) {
        case INT_IRQ + IRQ_TIMER: {
            cpustate* cpu = this_cpu();
            if (cpu->cpuindex_ == 0) {
                tick();
            }
            sleep_wqs[ticks % SLEEP_WQS_COUNT].wake_all();
            lapicstate::get().ack();
            regs_ = regs;
            yield_noreturn();
            break; /* will not be reached */
        }

        case INT_PF: {  // pagefault exception
            // Analyze faulting address and access type.
            uintptr_t addr = rdcr2();
            const char* operation = regs->reg_errcode & PFERR_WRITE
                                        ? "write"
                                        : "read";
            const char* problem = regs->reg_errcode & PFERR_PRESENT
                                      ? "protection problem"
                                      : "missing page";

            if ((regs->reg_cs & 3) == 0) {
                panic_at(*regs, "Kernel page fault for %p (%s %s)!\n",
                         addr, operation, problem);
            }

            error_printf(CPOS(24, 0), 0x0C00,
                         "Process %d page fault for %p (%s %s, rip=%p)!\n",
                         id_, addr, operation, problem, regs->reg_rip);
            pstate_ = proc::ps_faulted;
            yield();
            break;
        }

        case INT_IRQ + IRQ_KEYBOARD:
            keyboardstate::get().handle_interrupt();
            break;

        default:
            if (sata_disk && regs->reg_intno == INT_IRQ + sata_disk->irq_) {
                sata_disk->handle_interrupt();
            } else {
                panic_at(*regs, "Unexpected exception %d!\n", regs->reg_intno);
            }
            break; /* will not be reached */
    }

    // return to interrupted context
}


// proc::syscall(regs)
//    System call handler.
//
//    The register values from system call time are stored in `regs`.
//    The return value from `proc::syscall()` is returned to the user
//    process in `%rax`.
uintptr_t proc::syscall(regstate* regs) {
    uintptr_t ret_val = unsafe_syscall(regs);
    assert(canary = PROC_CANARY);
    return ret_val;
}


// proc::unsafe_syscall(regs)
//      same as proc::syscall(regs), but does not assert
//      the stack canary is valid.
uintptr_t proc::unsafe_syscall(regstate* regs) {

    // Record most recent user-mode %rip.
    recent_user_rip_ = regs->reg_rip;

    switch (regs->reg_rax) {
        case SYSCALL_CONSOLETYPE:
            if (consoletype != (int)regs->reg_rdi) {
                console_clear();
            }
            consoletype = regs->reg_rdi;
            return 0;

        case SYSCALL_PANIC:
            panic_at(*regs, "process %d called sys_panic()", id_);
            break;  // will not be reached

        case SYSCALL_GETPID:
            return id_;

        case SYSCALL_YIELD:
            yield();
            return 0;

        case SYSCALL_PAGE_ALLOC: {
            uintptr_t addr = regs->reg_rdi;
            if (addr >= VA_LOWEND || addr & 0xFFF) {
                return -1;
            }
            void* pg = kalloc(PAGESIZE);
            if (!pg || vmiter(this, addr).try_map(ka2pa(pg), PTE_PWU) < 0) {
                return -1;
            }
            return 0;
        }

        case SYSCALL_PAGES_ALLOC: {
            uintptr_t addr = regs->reg_rdi;
            uintptr_t n = regs->reg_rsi;
            return syscall_alloc(addr, PAGESIZE * n);
        }

        case SYSCALL_ALLOC: {
            uintptr_t addr = regs->reg_rdi;
            uintptr_t sz = regs->reg_rsi;
            return syscall_alloc(addr, sz);
        }


        case SYSCALL_PAUSE: {
            sti();
            for (uintptr_t delay = 0; delay < 1000000; ++delay) {
                pause();
            }
            return 0;
        }

        case SYSCALL_MAP_CONSOLE: {
            uintptr_t addr = regs->reg_rdi;
            // verify that addr is low-canonical and page aligned
            if (addr > VA_LOWMAX || addr & 0xFFF) {
                return E_INVAL;
            }
            // the console is at physical address CONSOLE_ADDR
            int r = vmiter(this, addr).try_map(CONSOLE_ADDR, PTE_PWU);
            if(r < 0) {
                return E_NOMEM;
            }
            return r;
        }

        case SYSCALL_FORK:
            return syscall_fork(regs);

        case SYSCALL_NASTY:
            return syscall_nasty();

        case SYSCALL_READ:
            return syscall_read(regs);

        case SYSCALL_WRITE:
            return syscall_write(regs);

        case SYSCALL_READDISKFILE:
            return syscall_readdiskfile(regs);

        case SYSCALL_GETPPID: {
            // synchronize access to ppid_ with exit
            spinlock_guard g(ptable_lock);
            return ppid_;
        }

        case SYSCALL_WAITPID: {
            pid_t pid = regs->reg_rdi;
            int* status = reinterpret_cast<int*>(regs->reg_rsi);
            int options = regs->reg_rdx;
            return syscall_waitpid(pid, status, options);
        }

        case SYSCALL_DUP2: {
            return syscall_dup2(regs->reg_rdi, regs->reg_rsi);
        }

        case SYSCALL_CLOSE: {
            return syscall_close(regs->reg_rdi);
        }

        case SYSCALL_PIPE: {
            return syscall_pipe();
        }

        case SYSCALL_EXIT: {
            int status = regs->reg_rdi;
            syscall_exit(status);
            return 0;
        }
        
        case SYSCALL_SLEEP: {
            unsigned long wakeup_time = ticks + (regs->reg_rdi + 9) / 10;
            sleeping_ = true;
            waiter w;
            w.block_until(sleep_wqs[wakeup_time % SLEEP_WQS_COUNT], [&] () {
                return (long(wakeup_time - ticks) < 0 || interrupted_);
            });
            sleeping_ = false;
            if(interrupted_) {
                interrupted_ = false;
                return E_INTR;
            }
            return 0;
        }

        case SYSCALL_SYNC: {
            int drop = regs->reg_rdi;
            // `drop > 1` asserts that no data blocks are referenced (except
            // possibly superblock and FBB blocks). This can only be ensured on
            // tests that run as the first process.
            if (drop > 1 && strncmp(CHICKADEE_FIRST_PROCESS, "test", 4) != 0) {
                drop = 1;
            }
            return bufcache::get().sync(drop);
        }

        default:
            // no such system call
            log_printf("%d: no such system call %u\n", id_, regs->reg_rax);
            return E_NOSYS;
    }
}

int proc::syscall_nasty() {
    int nasty_array[1000];
    for(int i = 0; i < 1000; i++) {
        nasty_array[i] = 2;
    }
    return nasty_array[1] + nasty_array[2];
}

// proc::syscall_alloc(regs)
//    Handle allocation system calls.
int proc::syscall_alloc(uintptr_t addr, uintptr_t sz) {
    if (addr >= VA_LOWEND || addr & 0xFFF) {
        return -1;
    }
    if (sz == 0) {
        return -1;
    }

    void* ptr = kalloc(sz);
    if (!ptr) {
        return -1;
    }

    for(uintptr_t va = addr; va < addr + sz; va += PAGESIZE, ptr += PAGESIZE) {
        if(vmiter(this, va).try_map(ka2pa(ptr), PTE_PWU) < 0) {
            kfree(ptr);
            // TODO: unmap memory
            return -1;
        }
    }

    return 0;
}

// proc::syscall_fork(regs)
//    Handle fork system call.
// TODO: use goto statements for cleaner deallocation (see lecture 6)
// TODO: implement copy-on-write (see lecture 09)
int proc::syscall_fork(regstate* regs) {
    // protect access to ptable
    spinlock_guard guard(ptable_lock);

    proc* p;
    pid_t child_pid;
        
    pid_t i;
    // look for available pid
    for (i = 1; i < NPROC; ++i) {
        if (!ptable[i]) {
            child_pid = i;
            break;
        }
    }

    // return error if out of pids
    if (i == NPROC) {
        return E_NOMEM;
    }

    // allocate process and assign found pid to it
    p = knew<proc>();
    if (!p) {
        return E_NOMEM;
    }
    ptable[child_pid] = p;

    // allocate pagetable for the process
    x86_64_pagetable* pagetable = kalloc_pagetable();
    if (!pagetable) {
        kfree(p);
        ptable[child_pid] = nullptr;
        return E_NOMEM;
    }

    // initialize process
    p->init_user(child_pid, pagetable);

    // copy the parent process' user-accessible memory
    for (vmiter it(this, 0); it.low(); it.next()) {
        if (it.user() && it.pa() != CONSOLE_ADDR) {
            // allocate new page
            void* new_page = kalloc(PAGESIZE);

            // map page's physical address to a virtual address
            if (!new_page || vmiter(p, it.va()).try_map(new_page, it.perm()) != 0) {
                // remove proces from ptable
                ptable[child_pid] = nullptr;
                // free most recently allocated memory page
                kfree(new_page);
                // free memory pages allocated in previous iterations
                kfree_mem(p);
                // free pagetable
                kfree_pagetable(pagetable);
                // free process page (struct proc and kernel stack)
                kfree(p);
                return E_NOMEM;
            }

            // copy parent's page
            memcpy(new_page, it.kptr(), PAGESIZE);
        } else if (it.pa() == CONSOLE_ADDR) {
            if(vmiter(p, it.va()).try_map(CONSOLE_ADDR, it.perm()) < 0) {
                // remove proces from ptable
                ptable[child_pid] = nullptr;
                // free memory pages allocated in previous iterations
                kfree_mem(p);
                // free pagetable
                kfree_pagetable(pagetable);
                // free process page (struct proc and kernel stack)
                kfree(p);
                return E_NOMEM;
            }
        }
    }

    // copy parent's file descriptors
    for(int fd = 0; fd < FDS_COUNT; ++fd) {
        if(fd_table_[fd]) {
            spinlock_guard(fd_table_[fd]->lock_);
            p->fd_table_[fd] = fd_table_[fd];
            ++fd_table_[fd]->ref_;
        }
    }

    // copy parent's register state
    memcpy(reinterpret_cast<void*>(p->regs_), reinterpret_cast<void*>(regs), sizeof(regstate));

    // set %rax so 0 gets returned to child
    p->regs_->reg_rax = 0;

    // set child's ppid
    p->ppid_ = this->id_;
    
    // add child to this process' children
    children_.push_front(p);

    // add child to a cpu
    cpus[child_pid % ncpu].enqueue(p);
    
    return child_pid;
}

// syscall_exit(status)
//      initiates process' exiting by making it non-runnable,
//      reparenting its children, freeing its user-accessible memory,
//      and waking up its parent so it can finish the exit process.
void proc::syscall_exit(int status) {

    {
        // synchronize access to pstate_ and to ppid_
        spinlock_guard guard(ptable_lock);

        //close file descriptor table
        for(int fd = 0; fd < FDS_COUNT; fd++) {
            syscall_close(fd);
        }

        // reparent process' children
        proc* child = children_.pop_front();
        while(child) {
            child->ppid_ = init_process->id_;
            init_process->children_.push_front(child);
            child = children_.pop_front();
        }

        // set the state of process to ps_nonrunnable parent finishes exit process
        pstate_ = ps_nonrunnable;

        // set exit status to be retrieved later when parent calls waitpid
        exit_status_ = status;

         // free process' user-acessible memory
        kfree_mem(this);


        // interrupt parent if it's sleeping
        proc* parent = ptable[ppid_];
        if(parent->sleeping_) {
            parent->interrupted_ = true;
        }
        
        // wake parent if it's waiting for child to exit
        wait_child_exit_wq.wake_proc(parent);


    }

    yield_noreturn();
}

// kill_zombie(zombie, status)
//      removes zombie from ptable and from parent's children's list.
//      frees zombie's resources (i.e, pagetables, struct proc, and kernel task stack), saves its
//      exit status in 'status', and return its pid to the caller.
pid_t proc::kill_zombie(proc* zombie, int* status) {
    // assert that access to ptable and ppid_ is protected
    assert(ptable_lock.is_locked());
    // assert zombie is a child of this process
    assert(zombie->ppid_ == id_);
    
    // save zombie's exit status to 'status'
    if(status) {
        *status = zombie->exit_status_;
    }

    // remove zombie from ptable and children' list
    pid_t zid = zombie->id_;
    ptable[zid] = nullptr;
    children_.erase(zombie);

    // free zombie's resources. This would work even if ptable_lock
    // was not acquired, since zombie is no longer at ptable at this point.
    // Hence, there are no synchronization conflicts with memviewer::refresh()
    kfree_pagetable(zombie->pagetable_);
    kfree(zombie);
    
    return zid;
}

pid_t proc::syscall_waitpid(pid_t pid, int* status, int options) {
    // synchronize access to pstate_
    spinlock_guard g(ptable_lock);

    proc* child = children_.front();
    if(!child) {
        return E_CHILD;
    }

    if(pid == 0) {
        // wait for any child
        if(options == W_NOHANG) {
            // iterate once over children looking for zombies
            while(child) {
                if(child->pstate_ == proc::ps_nonrunnable) {
                    return kill_zombie(child, status);
                }
                child = children_.next(child);
            }
            // W_NOHANG means not blocking
            return E_AGAIN;
        }

        // set state to blocked with lock held and release the lock before actually sleeping.
        // this avoids the lost wakeup problem 
        waiter w;
        w.block_until(wait_child_exit_wq, [&] () {
            child = children_.next(child);
            // loop back if out of children
            if(!child) {
                child = children_.front();
            }
            return child->pstate_ == proc::ps_nonrunnable;
        }, g);

        // now, child state is ps_nonrunnable, so kill zombie
        return kill_zombie(child, status);
    }
    
    // wait for specific child with id 'pid'
    while(child) {
        if(child->id_ != pid) {
            child = children_.next(child);
        } else {
            // found the child
            if(options == W_NOHANG) {
                // don't block
                if(child->pstate_ != proc::ps_nonrunnable) {
                    return E_AGAIN;
                }
                return kill_zombie(child, status);
            }
            // block until child exits, releasing ptable_lock while doing so
            waiter w;
            w.block_until(wait_child_exit_wq, [&] () {
                return (child->pstate_ == proc::ps_nonrunnable);
            }, g);
            return kill_zombie(child, status);
        }
    }
    // did not find the child
    return E_CHILD;
}

// proc::syscall_read(regs), proc::syscall_write(regs),
// proc::syscall_readdiskfile(regs)
//    Handle read and write system calls.
uintptr_t proc::syscall_read(regstate* regs) {
    // This is a slow system call, so allow interrupts by default
    sti();

    int fd = regs->reg_rdi;
    uintptr_t addr = regs->reg_rsi;
    size_t sz = regs->reg_rdx;
    // read no characters
    if(!sz) return 0;

    // check for integer overflow
    if(addr + sz < addr || sz == SIZE_MAX) {
        return E_FAULT;
    }

    // test that file descriptor is present and readable
    if(fd < 0 || !fd_table_[fd] || !fd_table_[fd]->readable_) {
        return E_BADF;
    }

    // test that memory range is present, writable, and user-accessible
    if(!(vmiter(this, addr).range_perm(sz, PTE_PWU))) {
        return E_FAULT;
    }

    // read 'sz' bytes into 'addr' and from file descriptor
    return fd_table_[fd]->vnode_->read(fd_table_[fd], addr, sz);
}

uintptr_t proc::syscall_write(regstate* regs) {
    // This is a slow system call, so allow interrupts by default
    sti();

    int fd = regs->reg_rdi;
    uintptr_t addr = regs->reg_rsi;
    size_t sz = regs->reg_rdx;
    // read no characters;
    if(!sz) return 0;

    // check for integer overflow
    if(addr + sz < addr || sz == SIZE_MAX) {
        return E_FAULT;
    }

    // test that file descriptor is present and writable
    if(fd < 0 || !fd_table_[fd] || !fd_table_[fd]->writable_) {
        return E_BADF;
    }

    // check for present and user-accessible memory
    if(!(vmiter(this, addr).range_perm(sz, PTE_P | PTE_U))) {
        return E_FAULT;
    }
    
    return fd_table_[fd]->vnode_->write(fd_table_[fd], addr, sz);
}

uintptr_t proc::syscall_readdiskfile(regstate* regs) {
    // This is a slow system call, so allow interrupts by default
    sti();

    const char* filename = reinterpret_cast<const char*>(regs->reg_rdi);
    unsigned char* buf = reinterpret_cast<unsigned char*>(regs->reg_rsi);
    size_t sz = regs->reg_rdx;
    off_t off = regs->reg_r10;

    if (!sata_disk) {
        return E_IO;
    }

    // read root directory to find file inode number
    auto ino = chkfsstate::get().lookup_inode(filename);
    if (!ino) {
        return E_NOENT;
    }

    // read file inode
    ino->lock_read();
    chkfs_fileiter it(ino);

    size_t nread = 0;
    while (nread < sz) {
        // copy data from current block
        if (bcentry* e = it.find(off).get_disk_entry()) {
            unsigned b = it.block_relative_offset();
            size_t ncopy = min(
                size_t(ino->size - it.offset()),   // bytes left in file
                chkfs::blocksize - b,              // bytes left in block
                sz - nread                         // bytes left in request
            );
            memcpy(buf + nread, e->buf_ + b, ncopy);
            e->put();

            nread += ncopy;
            off += ncopy;
            if (ncopy == 0) {
                break;
            }
        } else {
            break;
        }
    }

    ino->unlock_read();
    ino->put();
    return nread;
}

int proc::syscall_dup2(int fd1, int fd2) {
    if(fd1 == fd2) return fd2;

    // test that file descriptors are valid
    if(fd1 < 0 || fd1 >= FDS_COUNT || !fd_table_[fd1]) {
        return E_BADF;
    }
    if(fd2 < 0 || fd2 >= FDS_COUNT) {
        return E_BADF;
    }

    // close fd2 if present
    if(fd_table_[fd2]) {
        syscall_close(fd2);
    }
    
    // copy fd1 into fd2 and increase reference count
    fd_table_[fd2] = fd_table_[fd1];
    spinlock_guard(fd_table_[fd2]->lock_);
    ++fd_table_[fd2]->ref_;
    return fd2;
}

int proc::syscall_close(int fd) {
    // test that file descriptor is valid
    if(fd < 0 || fd >= FDS_COUNT) {
        return E_BADF;
    }
    file_descriptor *f = fd_table_[fd];
    if(!f) {
        return E_BADF;
    }

    // clear file descriptor table entry
    fd_table_[fd] = nullptr;

    // protect access to file descriptor table
    spinlock_guard(f->lock_);
    
    // free file descriptor if not referenced by any process
    --f->ref_;
    if(!f->ref_) {
        // if file is a pipe, try closing it
        if(f->type_ == file_descriptor::fd_pipe) {
            try_close_pipe(f);
        }

        // free vnode if not referenced by any file descriptor
        spinlock_guard g(f->vnode_->lock_);
        --f->vnode_->ref_;
        if(!f->vnode_->ref_) {
            kfree(f->vnode_);
        }

        // free file descriptor
        kfree(f);
    }

    return 0;
}

// try_close_pipe(f)
//      close pipe and free its resources if possible
void proc::try_close_pipe(file_descriptor* f) {
    assert(f->type_ == file_descriptor::fd_pipe);
    bounded_buffer* b = reinterpret_cast<bounded_buffer*>(f->vnode_->data_);
    if(f->writable_) {
        b->write_closed_ = true;
        b->wq_.wake_all();
    }
    if(f->readable_) {
        b->read_closed_ = true;
        b->wq_.wake_all();
    }
    // free buffer if read and write ends are closed
    if(b->write_closed_ && b->read_closed_) {
        kfree(f->vnode_->data_);
        f->vnode_->data_ = nullptr;
    }
}

uintptr_t proc::syscall_pipe() {
    // allocate read and write ends
    int rfd = fd_alloc(true, false, file_descriptor::fd_pipe);
    if(rfd < 0) {
        return rfd;
    }
    int wfd = fd_alloc(false, true, file_descriptor::fd_pipe);
    if(wfd < 0) {
        fd_table_[rfd] = nullptr;
        kfree(reinterpret_cast<void*>(fd_table_[rfd]));
        return wfd;
    }

    // allocate vnode and its bounded buffer
    pipe_vnode* vnode_ptr = reinterpret_cast<pipe_vnode*>(knew<pipe_vnode>());
    bounded_buffer* bbuffer_ptr = reinterpret_cast<bounded_buffer*>(knew<bounded_buffer>());
    if(!vnode_ptr || !bbuffer_ptr) {
        kfree(reinterpret_cast<void*>(fd_table_[rfd]));
        fd_table_[rfd] = nullptr;
        kfree(reinterpret_cast<void*>(fd_table_[wfd]));
        fd_table_[wfd] = nullptr;
        if(vnode_ptr) kfree(reinterpret_cast<void*>(vnode_ptr));
        if(bbuffer_ptr) kfree(reinterpret_cast<void*>(bbuffer_ptr));
        return E_NOMEM;
    }

    // point file descriptors to vnode
    vnode_ptr->data_ = bbuffer_ptr;
    vnode_ptr->ref_ = 2;
    fd_table_[rfd]->vnode_ = vnode_ptr;
    fd_table_[wfd]->vnode_ = vnode_ptr;
    
    uintptr_t wfd_cast = wfd;
    return (wfd_cast << 32) | rfd;
}

// fd_alloc()
//     allocate a file descriptor if there is an available entry in the fd table.
//     set its readable_, writable_, and type_ arguments accordingly.
//     return fd on success and error code on failure
int proc::fd_alloc(bool readable, bool writable, int type) {
    for(int fd = 3; fd < FDS_COUNT; ++fd) {
        if(!fd_table_[fd]) {
            file_descriptor* fd_ptr = reinterpret_cast<file_descriptor*>(knew<file_descriptor>());
            if(!fd_ptr) {
                return E_NOMEM;
            }
            fd_ptr->ref_++;
            fd_ptr->readable_ = readable;
            fd_ptr->writable_ = writable;
            fd_ptr->type_ = type;
            fd_table_[fd] = fd_ptr;
            return fd;
        }
    }
    return E_MFILE;
}


// memshow()
//    Draw a picture of memory (physical and virtual) on the CGA console.
//    Switches to a new process's virtual memory map every 0.25 sec.
//    Uses `console_memviewer()`, a function defined in `k-memviewer.cc`.

static void memshow() {
    static unsigned long last_redisplay = 0;
    static unsigned long last_switch = 0;
    static int showing = 1;

    // redisplay every 0.04 sec
    if (last_redisplay != 0 && ticks - last_redisplay < HZ / 25) {
        return;
    }
    last_redisplay = ticks;

    // switch to a new process every 0.5 sec
    if (ticks - last_switch >= HZ / 2) {
        showing = (showing + 1) % NPROC;
        last_switch = ticks;
    }

    spinlock_guard guard(ptable_lock);

    int search = 0;
    while ((!ptable[showing]
            || !ptable[showing]->pagetable_
            || ptable[showing]->pagetable_ == early_pagetable)
           && search < NPROC) {
        showing = (showing + 1) % NPROC;
        ++search;
    }

    console_memviewer(ptable[showing]);
    if (!ptable[showing]) {
        console_printf(CPOS(10, 26), 0x0F00, "   VIRTUAL ADDRESS SPACE\n"
            "                          [All processes have exited]\n"
            "\n\n\n\n\n\n\n\n\n\n\n");
    }
}


// tick()
//    Called once every tick (0.01 sec, 1/HZ) by CPU 0. Updates the `ticks`
//    counter and performs other periodic maintenance tasks.

void tick() {
    // Update current time
    ++ticks;

    // Update display
    if (consoletype == CONSOLE_MEMVIEWER) {
        memshow();
    }
}
