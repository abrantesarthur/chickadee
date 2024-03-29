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
wait_queue proc_group_exiting_wq;

static void tick();
static void boot_process_start(pid_t pid, const char* program_name);
static void init_process_start();

void print_pgtable() {
    log_printf("ptable: [");
    for(int i = 0; i < NPROC; ++i) {
        proc_group *p = pgtable[i];
        if(p) {
            log_printf("%d, ", p->pid_);
        } else {
            log_printf("NULL, ",i, p);
        }
    }
    log_printf("]\n");
}

void print_processes() {
    for(int i = 0; i < NPROC; ++i) {
        proc_group *parent = pgtable[i];
        proc_group *child;
        proc* thread;
        if(parent) {
            // print child processes
            log_printf("P%d -> | ", parent->pid_);
            child = parent->children_.front();
            while(child) {
                log_printf("P%d | ", child->pid_);
                child = parent->children_.next(child);
            }
            log_printf("\n\t  | ");
            thread = parent->procs_.front();
            while(thread) {
                log_printf("T%d | ", thread->id_);
                thread = parent->procs_.next(thread);
            }
            log_printf("\n");
        }
    }
    log_printf("\n");
}



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
        pgtable[i] = nullptr;
    }

    // start init process
    init_process_start();

    // start boot process
    boot_process_start(2, CHICKADEE_FIRST_PROCESS);;

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
    {
        spinlock_guard guard(ptable_lock);
        assert(!ptable[1]);
        ptable[1] = init_process;
    }

    // allocate new process group
    proc_group* pg = knew<proc_group>(1, early_pagetable);
    assert(pg);
    // add process to process group
    init_process->pg_ = pg;
    pg->add_proc(init_process);
    // add process group to proces group table
    {
        spinlock_guard guard(pgtable_lock);
        assert(!pgtable[1]);
        pgtable[1] = pg;
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
    irqstate irqs = initfs_lock.lock();
    memfile_loader ld(memfile::initfs_lookup(name), kalloc_pagetable());
    assert(ld.memfile_ && ld.pagetable_);
    int r = proc::load(ld);
    initfs_lock.unlock(irqs);
    assert(r >= 0);
    assert(init_process);

    // allocate process group, make it a child of the initial
    // process group, and initialize memory and file descriptor table
    proc_group* pg = knew<proc_group>(pid, ld.pagetable_);
    init_process->pg_->add_child(pg);
    pg->init_fd_table();

    // add to process group table (requires lock in case another CPU is already
    // running processes)
    {
        spinlock_guard guard(pgtable_lock);
        assert(!pgtable[pid]);
        pgtable[pid] = pg;
    }

    // allocate process and add it to process group
    proc* p = knew<proc>();
    p->init_user(pid, pg);
    p->regs_->reg_rip = ld.entry_rip_;
    pg->add_proc(p);


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
            // wake sleeping processes
            sleep_wqs[ticks % SLEEP_WQS_COUNT].wake_all();
            // wake exiting processes
            proc_group_exiting_wq.wake_all();
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
     // yield if process group is exiting
     // cpustate::schedule will set this process state to ps_exiting
    auto irqs = pg_->lock_.lock();
    proc* e = pg_->who_exited_;
    pg_->lock_.unlock(irqs);
    if(e != nullptr) {
        yield_noreturn();
        // this won't be reached
    }

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
            return pg_->pid_;

        case SYSCALL_GETTID:
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

        case SYSCALL_CLONE:
            return syscall_clone(regs);

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
            return pg_->ppid_;
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
            // sleep until wakeup time, or thread is interrupted, or should exit
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

        case SYSCALL_EXECV: {
            uintptr_t program_name = regs->reg_rdi;
            const char* const* argv = reinterpret_cast<const char* const*>(regs->reg_rsi);
            size_t argc = regs->reg_rdx;
            return syscall_execv(program_name, argv, argc);
        }

        case SYSCALL_OPEN: {
            const char* pathname = reinterpret_cast<const char*>(regs->reg_rdi);
            int flags = regs->reg_rsi;
            return syscall_open(pathname, flags);
        }


        case SYSCALL_LSEEK: {
            int fd = regs->reg_rdi;
            off_t off = regs->reg_rsi;
            int whence = regs->reg_rdx;
            return syscall_lseek(fd, off, whence);
        }

        case SYSCALL_TESTKALLOC: {
            return syscall_testkalloc(regs);
        }

        case SYSCALL_WILDALLOC: {
            return syscall_wildalloc(regs);
        }

        case SYSCALL_TEXIT: {
            return syscall_texit(regs->reg_rdi);
        }

        case SYSCALL_LOGPROCS: {
            print_processes();
            return 0;
        }

        case SYSCALL_FUTEX: {
            uintptr_t uaddr = regs->reg_rdi;
            int futex_op = regs->reg_rsi;
            int val = regs->reg_rdx;
            return syscall_futex(uaddr, futex_op, val);
        }

        case SYSCALL_SHMGET: {
            int key = regs->reg_rdi;
            size_t size = regs->reg_rsi;
            return syscall_shmget(key, size);
        }

        case SYSCALL_SHMAT: {
            int shmid = regs->reg_rdi;
            uintptr_t shmaddr = regs->reg_rsi;
            return syscall_shmat(shmid, shmaddr);
        }

        case SYSCALL_SHMDT: {
            return syscall_shmdt(regs->reg_rdi);
        }

        default:
            // no such system call
            log_printf("%d: no such system call %u\n", id_, regs->reg_rax);
            return E_NOSYS;
    }
}

int proc::syscall_nasty() {
    // make this sz == 1000 to properly evoke syscall_nasty.
    // we set it to only 10 to avoid compilation warnings.
    const int sz = 10;
    int nasty_array[sz];
    for(int i = 0; i < sz; i++) {
        nasty_array[i] = 2;
    }
    return nasty_array[1] + nasty_array[2];
}

// proc::syscall_fork(regs)
//    Handle fork system call.
// TODO: implement copy-on-write (see lecture 09)
// TODO: make use of syscall_clone
int proc::syscall_fork(regstate* regs) {
    // protect access to ptable
    spinlock_guard ptable_guard(ptable_lock);

    // protect access to pgtable
    spinlock_guard pgtable_guard(pgtable_lock);

    // protect shared memory segments
    spinlock_guard pg_lock(pg_->lock_);

    proc_group* pg;
    pid_t child_pid;
    pid_t j;
    // look for available process group PID
    for(j = 1; j < NPROC; j++) {
        if(!pgtable[j]) {
            child_pid = j;
            break;
        }
    }
    // return error if out of pids
    if(j == NPROC) {
        return E_NOMEM;
    }

    // allocate pagetable for the process group
    x86_64_pagetable* pagetable = kalloc_pagetable();
    if (!pagetable) {
        return E_NOMEM;
    }

    // allocate process group with PID child_pid, make it a child
    // of this process group, and initiate its memory
    pg = knew<proc_group>(child_pid, pagetable);
    if (!pg) {
        goto bad_fork_free_pagetable;
    }
    pgtable[child_pid] = pg;
    pg_->add_child(pg);

    proc* p;
    pid_t child_id;
    pid_t i;
    // look for available thread pid
    for (i = 1; i < NPROC; ++i) {
        if (!ptable[i]) {
            child_id = i;
            break;
        }
    }

    // return error if out of pids
    if (i == NPROC) {
        goto bad_fork_cleanup_procgroup;
    }


    // allocate process and assign found pid to it
    p = knew<proc>();
    if (!p) {
        goto bad_fork_cleanup_procgroup;
    }
    ptable[child_id] = p;

    // initialize process and add it to process group
    p->init_user(child_id, pg);
    pg->add_proc(p);

    // copy the parent process' user-accessible memory
    for (vmiter it(this, 0); it.low(); it.next()) {
        // don't duplicate shared memory segments
        if(pg_->get_shared_mem_seg_id(it.va()) >= 0) {
            log_printf("sys_fork don't duplicate va %p pa %p\n", it.va(), it.pa());
            if(vmiter(p, it.va()).try_map(it.pa(), it.perm()) < 0) {
                goto bad_fork_cleanup_childproc;
            }
            log_printf("sys_fork now child has va %p mapped to pa %p\n", vmiter(p, it.va()).va(), vmiter(p, it.va()).pa());
            continue;
        }
        
        // don't duplicate console page
        if (it.pa() == CONSOLE_ADDR) {
            if(vmiter(p, it.va()).try_map(CONSOLE_ADDR, it.perm()) < 0) {
                goto bad_fork_cleanup_childproc;
            }
            continue;
        }

        // copy regular user pages
        if (it.user()) {
            // allocate new page
            void* new_page = kalloc(PAGESIZE);
            // map page's physical address to a virtual address
            if (!new_page || vmiter(p, it.va()).try_map(new_page, it.perm()) != 0) {
                // free most recently allocated memory page
                kfree(new_page);
                goto bad_fork_cleanup_childproc;
            }
            // copy parent's page
            memcpy(new_page, it.kptr(), PAGESIZE);
        }
    }

    // copy parent's shared memory segments
    for(int shimd = 0; shimd < NSEGS; ++shimd) {
        shared_mem_segment* sms = pg_->get_shared_mem_seg(shimd);
        if(sms) {
            auto irqs = sms->lock_.lock();
            ++sms->ref;
            p->pg_->sm_segs_[shimd] = sms;
            sms->lock_.unlock(irqs);
        }
    }

    // copy parent's file descriptors
    for(int fd = 0; fd < FDS_COUNT; ++fd) {
        if(pg_->fd_table_[fd]) {
            spinlock_guard(pg_->fd_table_[fd]->lock_);
            p->pg_->fd_table_[fd] = pg_->fd_table_[fd];
            ++pg_->fd_table_[fd]->ref_;
        }
    }

    // copy parent's register state
    memcpy(reinterpret_cast<void*>(p->regs_), reinterpret_cast<void*>(regs), sizeof(regstate));

    // set %rax so 0 gets returned to child
    p->regs_->reg_rax = 0;

    // add child to a cpu
    cpus[child_id % ncpu].enqueue(p);

    return child_id;

    // error handling 'goto' statements
    bad_fork_cleanup_childproc:
        // remove process from ptable
        ptable[child_id] = nullptr;
        // free memory pages allocated in previous iterations
        kfree_mem(p);
        // free process page (struct proc and kernel stack)
        kfree(p);
    bad_fork_cleanup_procgroup:
        // remove process group from pgtable
        pgtable[child_pid] = nullptr;
        // remove process group from this group's children
        pg_->remove_child(pg);
        // free process group
        kfree(pg);
    bad_fork_free_pagetable:
        kfree_pagetable(pagetable);
    return E_NOMEM;
}

pid_t proc::syscall_clone(regstate* regs) {
    // protect access to ptable
    spinlock_guard ptable_guard(ptable_lock);

    proc* p;
    pid_t child_id;
    pid_t i;
    // look for available thread pid
    for (i = 1; i < NPROC; ++i) {
        if (!ptable[i]) {
            child_id = i;
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
    ptable[child_id] = p;

    // initialize process and add it to this process group
    p->init_user(child_id, pg_);
    pg_->add_proc(p);

    // copy parent's register state
    memcpy(reinterpret_cast<void*>(p->regs_), reinterpret_cast<void*>(regs), sizeof(regstate));

    // set %rax so 0 gets returned to child
    p->regs_->reg_rax = 0;

    // add child to a cpu
    cpus[child_id % ncpu].enqueue(p);

    return child_id;
}

// syscall_exit(status)
//      initiates process' exiting by making it non-runnable,
//      reparenting its children, freeing its user-accessible memory,
//      and waking up its parent so it can finish the exit process.
void proc::syscall_exit(int status) {
    {
        // synchronize access to pstate_, ppid_, and proc_group::children_
        // TODO: protect pstate_ with a less generic lock
        spinlock_guard guard(ptable_lock);

        // protect access to pg_ properties
        auto pg_lock_irqs = pg_->lock_.lock();

        // if process group is already exiting, simply yield. This proc
        // will set its status to ps_exiting in cpustate::schedule()
        // TODO: test this case: two threads exiting at the same time
        if(pg_->who_exited_ != nullptr) {
            guard.lock_.unlock(guard.irqs_);
            yield_noreturn();
            // this won't be reached
        }

        // flag process group as exiting. This signals other processes
        // in the group to set their state to ps_exiting in cpustate::schedule()s
        pg_->who_exited_ = this;

        // reparent process group' children
        proc_group* child_group = pg_->children_.pop_front();
        while(child_group) {
            child_group->ppid_ = init_process->pg_->pid_;
            init_process->pg_->children_.push_front(child_group);
            child_group = pg_->children_.pop_front();
        }

        pg_->lock_.unlock(pg_lock_irqs);

        // block until all other processes (i.e., threads) have exited
        waiter w;
        w.block_until(proc_group_exiting_wq, [&] () {
            proc* p = pg_->procs_.front();
            assert(p);
            while(p && p != this) {
                // if 'p' is blocked, wake it up so it can switch its state to ps_exiting
                if(p->pstate_ == ps_blocked) {
                    // it may happen that 'p' will block, but just hasn't yet, so waking
                    // it up will have not effect. Hence, the predicate will fail and this
                    // process will block. This is okay, though, since whenever a timer
                    // interrupt happens we wake this process, so it can rerun the predicate
                    // By then, 'p' should be already blocked and we'll wake it up.
                    p->wake();
                    return false;
                }

                if(p->pstate_ != ps_exiting) {
                    return false;
                }

                p = pg_->procs_.next(p);
            }
            return true;
        }, guard);

        // set the state of this process as ps_exiting
        // parent will finish exit process
        pstate_ = ps_exiting;

        // set exit status to be retrieved later when parent calls waitpid
        pg_->exit_status_ = status;

        //close process group's file descriptor table
        for(int fd = 0; fd < FDS_COUNT; fd++) {
            syscall_close(fd);
        }

        // unmap process' shared memory
        pg_->unmap_all_shared_mem();

        // free process' user-acessible memory
        kfree_mem(this);

        // iterate over threads in parent process
        proc_group* parent = pgtable[pg_->ppid_];
        proc* p = parent->procs_.front();
        while(p) {
            // interrupt a thread if it's sleeping
            if(p->sleeping_) {
                p->interrupted_ = true;
            }

            // wake thread if it's waiting for child process to exit
            wait_child_exit_wq.wake_proc(p);

            p = parent->procs_.next(p);
        }
    }

    yield_noreturn();
}

// TODO: try to integrate this with syscall_exit
pid_t proc::syscall_texit(int status) {
    {
        // synchronize access to pstate_ and to ppid_
        // TODO: protect pstate_ with a less generic lock
        spinlock_guard guard(ptable_lock);

        // set the state of this process as ps_exiting
        // parent will finish exit process in sys_waitpid
        pstate_ = ps_exiting;

        // set exit status to be retrieved later when parent calls waitpid
        pg_->exit_status_ = status;

        // if there are no other threads in this process group
        proc* p = pg_->procs_.front();
        assert(p);
        p = pg_->procs_.next(p);
        if(!p) {
            assert(p == this);
            // reparent process group' children
            proc_group* child_group = pg_->children_.pop_front();
            while(child_group) {
                child_group->ppid_ = init_process->pg_->pid_;
                init_process->pg_->children_.push_front(child_group);
                child_group = pg_->children_.pop_front();
            }

            //close process group's file descriptor table
            for(int fd = 0; fd < FDS_COUNT; fd++) {
                syscall_close(fd);
            }

            // free process' user-acessible memory
            kfree_mem(this);
        }

        // iterate over threads in parent process
        proc_group* parent = pgtable[pg_->ppid_];
        p = parent->procs_.front();
        while(p) {
            // interrupt a thread if it's sleeping
            if(p->sleeping_) {
                p->interrupted_ = true;
            }

            // wake thread if it's waiting for child process to exit
            wait_child_exit_wq.wake_proc(p);

            p = parent->procs_.next(p);
        }
    }

    yield_noreturn();
}


// kill_zombie(zombie, status)
//      removes zombie from ptable and from parent's children's list.
//      frees zombie's resources (i.e, pagetables, struct proc, and kernel task stack), saves its
//      exit status in 'status', and return its pid to the caller.
pid_t proc::kill_zombie(proc_group* zombie, int* status) {
    // assert that all processes within the group are zombies
    assert(zombie->is_zombie());
    // assert that access to ptable and ppid_ is protected
    assert(ptable_lock.is_locked());
    // assert zombie is a child of this process
    assert(zombie->ppid_ == pg_->pid_);

    // save zombie's exit status to 'status'
    if(status) {
        *status = zombie->exit_status_;
    }

    // remove all processes within zombie from ptable
    proc* p = zombie->procs_.pop_front();
    assert(p);
    while(p) {
        ptable[p->id_] = nullptr;
        kfree(p);
        p = zombie->procs_.pop_front();
    }

    // remove zombie from pgtable and from its parent's children's list
    spinlock_guard guard(pgtable_lock);
    pid_t zid = zombie->pid_;
    pgtable[zid] = nullptr;
    pg_->children_.erase(zombie);

    // free zombie's resources. This would work even if ptable_lock
    // was not acquired, since zombie is no longer at ptable at this point.
    // Hence, there are no synchronization conflicts with memviewer::refresh()
    kfree_pagetable(zombie->pagetable_);
    kfree(zombie);

    return zid;
}

// proc::is_zombie(pg)
//    Returns true iff all processes within process group
//    are non-runnable (i.e., zombies)
bool proc_group::is_zombie() {
    proc* p = procs_.front();
    assert(p);
    while(p) {
        if(p->pstate_ != proc::ps_exiting) {
            return false;
        }
        p = procs_.next(p);
    }
    return true;
}

pid_t proc::syscall_waitpid(pid_t pid, int* status, int options) {
    // synchronize access to pstate_
    spinlock_guard g(ptable_lock);

    proc_group* child = pg_->children_.front();
    if(!child) {
        return E_CHILD;
    }

    if(pid == 0) {
        // wait for any child
        if(options == W_NOHANG) {
            // iterate once over children process group
            while(child) {
                // if found a group whose all processes are all zombies, kill the group
                if(child->is_zombie()) {
                    return kill_zombie(child, status);
                }
                // otherwise, go to next group
                child = pg_->children_.next(child);
            }

            // W_NOHANG means not blocking
            return E_AGAIN;
        }

        // set state to blocked with lock held and release the lock before actually sleeping.
        // this avoids the lost wakeup problem
        waiter w;
        w.block_until(wait_child_exit_wq, [&] () {
            child = pg_->children_.next(child);
            // loop back if out of children
            if(!child) {
                child = pg_->children_.front();
            }
            return child->is_zombie();
        }, g);

        // now, process group has only non-runnable processes, so kill it
        return kill_zombie(child, status);
    }

    // wait for specific child with id 'pid'
    while(child) {
        if(child->pid_ != pid) {
            child = pg_->children_.next(child);
        } else {
            // found the child
            if(options == W_NOHANG) {
                // don't block
                if(!child->is_zombie()) {
                    return E_AGAIN;
                }
                return kill_zombie(child, status);
            }
            // block until child exits, releasing ptable_lock while doing so
            waiter w;
            w.block_until(wait_child_exit_wq, [&] () {
                return child->is_zombie();
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
    if(fd < 0 || !pg_->fd_table_[fd] || !pg_->fd_table_[fd]->readable_) {
        return E_BADF;
    }


    // test that memory range is present, writable, and user-accessible
    if(!(vmiter(this, addr).range_perm(sz, PTE_PWU))) {
        return E_FAULT;
    }
    // read 'sz' bytes into 'addr' and from file descriptor
    return pg_->fd_table_[fd]->vnode_->read(pg_->fd_table_[fd], addr, sz);
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
    if(fd < 0 || !pg_->fd_table_[fd] || !pg_->fd_table_[fd]->writable_) {
        return E_BADF;
    }

    // check for present and user-accessible memory
    if(!(vmiter(this, addr).range_perm(sz, PTE_P | PTE_U))) {
        return E_FAULT;
    }

    return pg_->fd_table_[fd]->vnode_->write(pg_->fd_table_[fd], addr, sz);
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
    if(fd1 < 0 || fd1 >= FDS_COUNT || !pg_->fd_table_[fd1]) {
        return E_BADF;
    }
    if(fd2 < 0 || fd2 >= FDS_COUNT) {
        return E_BADF;
    }

    // close fd2 if present
    if(pg_->fd_table_[fd2]) {
        syscall_close(fd2);
    }

    // copy fd1 into fd2 and increase reference count
    pg_->fd_table_[fd2] = pg_->fd_table_[fd1];
    spinlock_guard(pg_->fd_table_[fd2]->lock_);
    ++pg_->fd_table_[fd2]->ref_;
    return fd2;
}

int proc::syscall_close(int fd) {
    // test that file descriptor is valid
    if(fd < 0 || fd >= FDS_COUNT) {
        return E_BADF;
    }
    file_descriptor *f = pg_->fd_table_[fd];
    if(!f) {
        return E_BADF;
    }

    // clear file descriptor table entry
    pg_->fd_table_[fd] = nullptr;

    // protect access to file descriptor table
    spinlock_guard(f->lock_);

    if(f->type_ == file_descriptor::disk_t) {
        // illegal to close a disk file while holding a write reference
        assert(reinterpret_cast<diskfile_vnode*>(f->vnode_)->ino_->entry()->write_ref_ == 0);
    }

    // free file descriptor if not referenced by any process
    --f->ref_;
    if(!f->ref_) {
        // if file is a pipe, try closing it
        if(f->type_ == file_descriptor::pipe_t) {
            try_close_pipe(f);
        }

        // free vnode if not referenced by any file descriptor
        spinlock_guard g(f->vnode_->lock_);
        --f->vnode_->ref_;
        if(!f->vnode_->ref_) {
            if(f->type_ == file_descriptor::disk_t) {
                // release buffer cache reference the file
                reinterpret_cast<diskfile_vnode*>(f->vnode_)->ino_->put();
            }
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
    assert(f->type_ == file_descriptor::pipe_t);
    pipe_vnode* vnode_ = reinterpret_cast<pipe_vnode*>(f->vnode_);
    if(f->writable_) {
        vnode_->buf_->write_closed_ = true;
        vnode_->buf_->wq_.wake_all();
    }
    if(f->readable_) {
        vnode_->buf_->read_closed_ = true;
        vnode_->buf_->wq_.wake_all();
    }
    // free buffer if read and write ends are closed
    if(vnode_->buf_->write_closed_ && vnode_->buf_->read_closed_) {
        kfree(vnode_->buf_);
        vnode_->buf_ = nullptr;
    }
}

// fd_alloc()
//     allocate a file descriptor if there is an available entry in the fd table.
//     set its readable_, writable_, and type_ arguments accordingly.
//     return fd on success and error code on failure
int proc::fd_alloc(int type, int flags, vnode* v) {
    for(int fd = 3; fd < FDS_COUNT; ++fd) {
        if(!pg_->fd_table_[fd]) {
            file_descriptor* fd_ptr = knew<file_descriptor>(type, flags, v);
            if(!fd_ptr) {
                return E_NOMEM;
            }
            pg_->fd_table_[fd] = fd_ptr;
            return fd;
        }
    }
    return E_MFILE;
}

uintptr_t proc::syscall_pipe() {
    // allocate vnode and its bounded buffer
    bounded_buffer* buf = knew<bounded_buffer>();
    if(!buf) {
        kfree(buf);
        return E_NOMEM;
    }
    pipe_vnode* vnode = knew<pipe_vnode>(buf, 2);
    if(!vnode) {
        kfree(vnode);
        return E_NOMEM;
    }

    // allocate read and write ends
    int rfd = fd_alloc(file_descriptor::pipe_t, OF_READ, vnode);
    if(rfd < 0) {
        kfree(buf);
        kfree(vnode);
        return rfd;
    }
    int wfd = fd_alloc(file_descriptor::pipe_t, OF_WRITE, vnode);
    if(wfd < 0) {
        kfree(pg_->fd_table_[rfd]);
        kfree(buf);
        kfree(vnode);
        pg_->fd_table_[rfd] = nullptr;
        return wfd;
    }

    uintptr_t wfd_cast = wfd;
    return (wfd_cast << 32) | rfd;
}

// is_address_user_accessible(addr, len)
//      checks whether the address range starting at 'addr' and ending
//      at 'len' is present and useraccessible
bool proc::is_address_user_accessible(uintptr_t addr, size_t len) {
    if(!addr) {
        return false;
    }

    vmiter it(this, addr);
    uintptr_t init_va = it.va();
    for(; it.va() < (init_va + len + 1) && it.va() < MEMSIZE_VIRTUAL; it += 1) {
        if(!it.user() || !it.present()) {
            return false;
        }

        // end of string
        if(*(it.kptr<char*>()) == 0) {
            break;
        }
    }
    return true;
}

int proc::syscall_execv(uintptr_t program_name, const char* const* argv, size_t argc) {
    // validate program name
    if(!is_address_user_accessible(program_name, chkfs::maxnamelen)) {
        return E_FAULT;
    }

    // validate argc and argv
    if(argc < 1 ||
    strcmp(reinterpret_cast<const char*>(program_name), argv[0]) != 0 ||
    argv[argc] != nullptr) {
        return E_FAULT;
    }

    // make sure arguments won't overflow the stack
    uintptr_t args_sz = 0;
    for(size_t i = 0; i < argc; ++i) {
        args_sz += strlen(argv[i]) + 1;
    }
    if(args_sz > 4096) {
        return E_FAULT;
    }

    // allocate a pagetable
    x86_64_pagetable *pt = kalloc_pagetable();
    if(!pt) {
        return E_NOMEM;
    }

    // find the corresponding disk file
    if(!sata_disk) return E_IO;
    auto ino = chkfsstate::get().lookup_inode(reinterpret_cast<const char*>(program_name));
    if(!ino) return E_FAULT;

    // instantiate a proc_loader with the disk file and pagetable
    diskfile_loader ld(ino, pt);

    // load program into user-level memory
    int r = proc::load(ld);
    if(r < 0){
        kfree_pagetable(pt);
        return r;
    }

    // map the user level stack at address MEMSIZE_VIRTUAL
    void* stackpg = kalloc(PAGESIZE);
    if(!stackpg || vmiter(pt, MEMSIZE_VIRTUAL - PAGESIZE).try_map(stackpg, PTE_PWU) < 0) {
        kfree(stackpg);
        kfree_pagetable(pt);
        return E_NOMEM;
    }

    // map the console at address CONSOLE_ADDR
    if(vmiter(pt, CONSOLE_ADDR).try_map(CONSOLE_ADDR, PTE_PWU) < 0) {
        kfree(stackpg);
        kfree_pagetable(pt);
        return E_NOMEM;
    }

    // copy arguments into new stack
    uintptr_t args_addrs[argc];
    uintptr_t sz;
    vmiter it(pt, MEMSIZE_VIRTUAL);
    for(int i = argc - 1; i >= 0; --i) {
        sz = strlen(argv[i]) + 1;
        it -= sz;
        memcpy(it.kptr<char*>(), argv[i], sz);
        // TODO: this is causing compilation warnings. Fix it by removing the
        // args_addrs variable and removing the second for loop.
        args_addrs[i] = it.va();
    }
    it -= sizeof(uintptr_t);
    memset(it.kptr<unsigned long*>(), 0, sizeof(uintptr_t));
    for(int i = argc - 1; i>= 0; --i) {
        it -= sizeof(uintptr_t);
        *(it.kptr<unsigned long*>()) = args_addrs[i];
    }

    // save old pagetable
    x86_64_pagetable *old_pt = pg_->pagetable_;

    // reset this process to have pagetable 'pt'
    pg_->pagetable_ = pt;
    init_user(id_, pg_);

    // set the registers
    regs_->reg_rbp = MEMSIZE_VIRTUAL;
    regs_->reg_rsp = it.va();
    regs_->reg_rip = ld.entry_rip_;
    regs_->reg_rsi = regs_->reg_rsp;
    regs_->reg_rdi = argc;

    // we assume syscall_execv is called by single-threaded processes

    // switch to new pagetable
    set_pagetable(pt);

    // free old pagetable and user memory
    kfree_mem(old_pt, pg_);
    kfree_pagetable(old_pt);
    yield_noreturn();
}

int proc::syscall_open(const char* pathname, int flags) {
    if(!is_address_user_accessible(
        reinterpret_cast<uintptr_t>(pathname), chkfs::maxnamelen)) return E_FAULT;
    if(!sata_disk) return E_IO;

    // read file from disk's root directory
    chkfs::inode* ino = chkfsstate::get().lookup_inode(pathname);
    if(!ino) {  // file doesn't exist
        if(flags & OF_CREATE && flags & OF_WRITE) {
            ino = chkfsstate::get().create_file(pathname);
            if(!ino) return E_AGAIN;
        } else {
            return E_NOENT;
        }
    }

    // allocate disk vnode
    vnode* v = knew<diskfile_vnode>(ino);
    if(!v) {
        ino->put();
        return E_NOMEM;
    }

    // allocate file descriptor
    int fd = fd_alloc(file_descriptor::disk_t, flags, v);
    if(fd < 0) {
        kfree(v);
        ino->put();
        return fd;
    }

    if(flags & OF_TRUNC && flags & OF_WRITE) {
        ino->lock_write();
        ino->entry()->get_write();
        ino->size = 0;
        ino->entry()->put_write();
        ino->unlock_write();
    }

    return fd;
}

// TODO: write a test that forks a child that seeks a disk file whereas the
// parent writes to it to make sure that the file_descriptor wpos_ and rpos_
// are correclty synchronized
ssize_t proc::syscall_lseek(int fd, off_t off, int whence) {
    file_descriptor *f = pg_->fd_table_[fd];

    if(!f || !f->vnode_) {
        return E_BADF;
    }

    if(f->type_ != file_descriptor::disk_t && f->type_ != file_descriptor::memfile_t) {
        return E_SPIPE;
    }

    size_t fsz;
    diskfile_vnode* dv = nullptr;
    memfile_vnode* mv = nullptr;

    if(f->type_ == file_descriptor::disk_t) {
        dv = reinterpret_cast<diskfile_vnode*>(f->vnode_);
        assert(dv && dv->ino_);
        dv->ino_->lock_read();
        fsz = dv->ino_->size;
    } else {
        mv = reinterpret_cast<memfile_vnode*>(f->vnode_);
        assert(mv);
        mv->lock_.lock_noirq();
        fsz = mv->mf_->len_;
    }

    int result;
    switch (whence) {
        case LSEEK_SET: {   // set position to 'off'
            if(off > 0 && size_t(off) >= fsz){
                result = E_INVAL;
                break;
            }
            f->rpos_ = off;
            f->wpos_ = off;
            result = f->rpos_;
            break;
        }
        case LSEEK_CUR: {   // adjust position by 'off'
            if(off > 0 && (size_t(off + f->rpos_) >= fsz || size_t(off + f->wpos_) >= fsz)) {
                result = E_INVAL;
                break;
            }
            f->rpos_ += off;
            f->wpos_ += off;
            result = f->rpos_;
            break;
        }
        case LSEEK_SIZE: {  // return file size
            result = fsz;
            break;
        }
        case LSEEK_END: {   // set position to 'off' bytes beyoned end of file
            f->rpos_ = fsz + off;
            f->wpos_ = fsz + off;
            result = f->rpos_;
            break;
        }
        default: {
            result = E_INVAL;
            break;
        }
    }

    if(f->type_ == file_descriptor::disk_t) dv->ino_->unlock_read();
    if(f->type_ == file_descriptor::memfile_t) mv->lock_.unlock_noirq();

    return result;
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
            || !ptable[showing]->pg_->pagetable_
            || ptable[showing]->pg_->pagetable_ == early_pagetable)
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



/**
 * FUTEX
 *  keep a hash table keyed by the address (virtual of physycal?) to find the proper queue data
 *  structure and add the calling process to the wait queue.
 *
 * FUTEX_WAIT: the kernel will block only if the futex word has the value that the calling thread supplied
 * as the expected value of the futex word
 *
 * loading the futex word's value, comparing that value with the expected value, and the actual
 * blocking are done atomically
 *
 */

// TODO: support timeout
int proc::syscall_futex(uintptr_t uaddr, int futex_op, int val) {
    // check that 32-bit word at 'uaddr' is valid user space
    if(!is_address_user_accessible(uaddr, 4)) return E_FAULT;

    // uaddr must be aligned on a 4 byte boundary
    if(uaddr % 4 != 0) return E_INVAL;

    if(futex_op & FUTEX_WAIT) {
        // beginning of critical area (loading, comparing, and blocking must be atomic)
        spinlock_guard guard(ftable.lock_);

        // atomically load the 32-bit word at 'uaddr'
        // TODO: this is a user address: test that this works
        std::atomic<int>* uaddr_ = reinterpret_cast<std::atomic<int>*>(uaddr);
        int actual_val = std::atomic_load(uaddr_);


        // if value at 'uaddr' doesn't match 'val', try again
        if(actual_val != val) return E_AGAIN;
            
        // otherwis, we need to block

        // get the wait_queue of processes that care about the futex in 'uaddr'
        int* kptr = vmiter(this, uaddr).kptr<int*>();
        wait_queue* wq = ftable.get_wait_queue(kptr);

        // if not found, try allocating a new entry
        if(!wq) wq = ftable.create_wait_queue(kptr);

        // if failed, instruct caller to try again
        if(!wq) return E_AGAIN;

        // block until woken up by another thread, which must modify 'actual_val' 
        waiter().block_until(*wq, [&]() {
            actual_val = std::atomic_load(uaddr_);
            return actual_val != val;
        }, guard);

        // TODO: return EINTR if blocking was interrupted by a signal

        // success
        return 0;
    }
    
    if(futex_op & FUTEX_WAKE) {
        // beginning of critical area
        spinlock_guard guard(ftable.lock_);

        // wake 'val' processes waiting on futex in 'uaddr'
        int* kptr = vmiter(this, uaddr).kptr<int*>();
        int awoken = ftable.wake_processes(kptr, val);

        // return number of processes woken up
        return awoken;
    }

    // futex_op is invalid
    return E_INVAL;
}

int proc::syscall_shmget(int key, size_t size) {
    spinlock_guard guard(pg_->lock_);
    if(key == IPC_PRIVATE) {
        // allocate new memory segment
        return pg_->alloc_shared_mem_seg(size);
    }

    // return previously allocated segment
    return pg_->get_shared_mem_seg_id(key);
}


uintptr_t proc::syscall_shmat(int shmid, uintptr_t shmaddr) {
    spinlock_guard guard(pg_->lock_);

    // shmaddr must be page-aligned and can't be null
    if(!shmaddr || shmaddr & 0xFFF) return 0;

    // return if shmaddr is already mapped shmid segment
    int curr_shmid = pg_->get_shared_mem_seg_id(shmaddr);
    if(curr_shmid == shmid) return shmaddr;

    // assert shmaddr is not already mapped to another segment
    if (curr_shmid != -1) return 0;

    // get size of the segment
    size_t segsz = pg_->get_shared_mem_seg_sz(shmid);
    if(!segsz) return 0;
    
    // map shmaddr to the segment
    if(pg_->map_shared_mem_seg_at(shmid, shmaddr) < 0) return 0;

    // success
    return shmaddr;
}

int proc::syscall_shmdt(uintptr_t shmaddr) {
    spinlock_guard guard(pg_->lock_);
    return pg_->unmap_shared_mem_seg_at(shmaddr);
}