#ifndef CHICKADEE_KERNEL_HH
#define CHICKADEE_KERNEL_HH
#include "x86-64.h"
#include "lib.hh"
#include "k-list.hh"
#include "k-lock.hh"
#include "k-memrange.hh"
#include "k-waitstruct.hh"
#include "k-vfs.hh"
#include "k-futex.hh"
#if CHICKADEE_PROCESS
#error "kernel.hh should not be used by process code."
#endif
struct proc;
struct proc_group;
struct yieldstate;
struct proc_loader;
struct elf_program;
struct vnode;
struct file_descriptor;
#define PROC_RUNNABLE 1
#define PROC_CANARY 0xabcdef
#define FDS_COUNT 32

// kernel.hh
//
//    Functions, constants, and definitions for the kernel.

// Process (i.e., Thread) descriptor type
struct __attribute__((aligned(4096))) proc {
    enum pstate_t {
        ps_blank = 0,
        ps_runnable = PROC_RUNNABLE,
        ps_exiting,
        ps_faulted,
        ps_blocked
    };

    // These four members must come first:
    pid_t id_ = 0;                             // Thread ID
    regstate* regs_ = nullptr;                 // Process's current registers
    yieldstate* yields_ = nullptr;             // Process's current yield state
    std::atomic<int> pstate_ = ps_blank;       // Process state
    proc_group* pg_;                           // Process' group
    std::atomic<bool> sleeping_ = false;       // Whether the process is sleeping
    std::atomic<bool> interrupted_ = false;    // The process was interrupted while sleeping
    unsigned long resume_count_ = 0;           // How many times the process has resumed
    uintptr_t recent_user_rip_ = 0;            // Most recent user-mode %rip
#if HAVE_SANITIZERS
    int sanitizer_status_ = 0;
#endif

    list_links runq_link_;                     // cpustate::runq link
    list_links link_;                          // proc_group::threads link

    proc();
    NO_COPY_OR_ASSIGN(proc);

    inline bool contains(uintptr_t addr) const;
    inline bool contains(void* ptr) const;

    void init_user(pid_t pid, proc_group* pg);
    void init_kernel(pid_t pid, void (*f)());

    int fd_alloc(int type, int flags, vnode* v);

    static int load(proc_loader& ld);

    void exception(regstate* reg);
    uintptr_t syscall(regstate* reg);
    uintptr_t unsafe_syscall(regstate* reg);

    void yield();
    [[noreturn]] void yield_noreturn();
    [[noreturn]] void resume();
    [[noreturn]] void panic_nonrunnable();

    inline bool resumable() const;

    int syscall_alloc(uintptr_t addr, uintptr_t sz);
    int syscall_fork(regstate* regs);
    void syscall_exit(int status);
    pid_t syscall_getppid(regstate* regs);
    pid_t syscall_waitpid(pid_t pid, int* status, int options);
    pid_t kill_zombie(proc_group* zombie, int* status);

    int syscall_nasty();

    uintptr_t syscall_read(regstate* reg);
    uintptr_t syscall_write(regstate* reg);
    uintptr_t syscall_readdiskfile(regstate* reg);
    int syscall_dup2(int fd1, int fd2);
    int syscall_close(int fd);
    uintptr_t syscall_pipe();
    int syscall_execv(uintptr_t program_name, const char* const* argv, size_t argc);
    int syscall_open(const char* pathname, int flags);
    ssize_t syscall_lseek(int fd, off_t off, int whence);
    void try_close_pipe(file_descriptor* f);
    pid_t syscall_clone(regstate* regs);
    pid_t syscall_texit(int status);
    int syscall_futex(uintptr_t addr, int futex_op, int val);
    int syscall_shmget(int key, size_t size);
    uintptr_t syscall_shmat(int shmid, uintptr_t shmaddr);
    int syscall_shmdt(uintptr_t shmaddr);


    // buddy allocator test syscalls
    int syscall_testkalloc(regstate* regs);
    int syscall_wildalloc(regstate *regs);

    bool is_address_user_accessible(uintptr_t addr, size_t len);

    void wake();

    inline irqstate lock_pagetable_read();
    inline void unlock_pagetable_read(irqstate& irqs);

 private:
    static int load_segment(const elf_program& ph, proc_loader& ld);

public:
    int canary = PROC_CANARY;
};

#define NPROC 16
#define NSEGS 16
extern proc* ptable[NPROC];
extern spinlock ptable_lock;
extern proc_group* pgtable[NPROC];
extern spinlock pgtable_lock;
extern struct keyboard_console_vnode* kbd_cons_vnode;
extern futex_table ftable;
#define PROCSTACK_SIZE 4096UL


struct shared_mem_segment {
    int ref = 0;            // how many processes referece this segment
    size_t size = 0;        // segment size (at least PAGESIZE)
    void* pa = 0;           // segment starting physical address
    uintptr_t va = 0;       // segment starting virtual address
    spinlock lock_;         // protects ref
};

// Process group (i.e., Process) descriptor type
struct proc_group {
    proc_group(pid_t pid, x86_64_pagetable* pt);

    pid_t pid_ = 0;                                 // Process ID
    pid_t ppid_;                                    // Parents process ID
    x86_64_pagetable* pagetable_ = nullptr;         // Process's page table
    int exit_status_;                               // Process exit status
    std::atomic<proc*> who_exited_ = nullptr;       // Which process exited the process group

    list_links link_;
    list<proc_group, &proc_group::link_> children_; // this process' child processes

    list<proc, &proc::link_> procs_;                // this process' threads

    struct file_descriptor* fd_table_[FDS_COUNT] = {nullptr};

    // shared memory segments
    shared_mem_segment* sm_segs_[NSEGS] = {nullptr};

    spinlock lock_;                                 // protects pagetable_, sm_segs_
    void init_fd_table();
    void add_proc(proc* p);
    void add_child(proc_group* pg);
    void remove_child(proc_group* pg);
    bool is_zombie();
    int alloc_shared_mem_seg(size_t size);
    shared_mem_segment* get_shared_mem_seg(int id);
    int get_shared_mem_seg_id(int id);
    int get_shared_mem_seg_id(uintptr_t va);
    size_t get_shared_mem_seg_sz(int id);
    int map_shared_mem_seg_at(int shmid, uintptr_t shmaddr);
    int unmap_shared_mem_seg_at(uintptr_t shmaddr);
    int unmap_all_shared_mem();
};

struct proc_loader {
    x86_64_pagetable* pagetable_;
    uintptr_t entry_rip_ = 0;
    inline proc_loader(x86_64_pagetable* pt)
        : pagetable_(pt) {
    }
    virtual ssize_t get_page(uint8_t** pg, size_t off) = 0;
    virtual void put_page() = 0;
};



// CPU state type
struct __attribute__((aligned(4096))) cpustate {
    // These three members must come first:
    cpustate* self_;
    proc* current_ = nullptr;
    uint64_t syscall_scratch_;

    int cpuindex_;
    int lapic_id_;

    list<proc, &proc::runq_link_> runq_;
    spinlock runq_lock_;
    unsigned long nschedule_;
    proc* idle_task_;

    unsigned spinlock_depth_;

    uint64_t gdt_segments_[7];
    x86_64_taskstate taskstate_;


    inline cpustate()
        : self_(this) {
    }
    NO_COPY_OR_ASSIGN(cpustate);

    inline bool contains(uintptr_t addr) const;
    inline bool contains(void* ptr) const;

    void init();
    void init_ap();

    void exception(regstate* reg);

    void enqueue(proc* p);
    [[noreturn]] void schedule(proc* yielding_from);

    void enable_irq(int irqno);
    void disable_irq(int irqno);

 private:
    void init_cpu_hardware();
    void init_idle_task();
};

#define MAXCPU 16
extern cpustate cpus[MAXCPU];
extern int ncpu;
#define CPUSTACK_SIZE 4096UL
#define CPUALTSTACK_SIZE 3072UL

inline cpustate* this_cpu();


// yieldstate: callee-saved registers that must be preserved across
// proc::yield()

struct yieldstate {
    uintptr_t reg_rbp;
    uintptr_t reg_rbx;
    uintptr_t reg_r12;
    uintptr_t reg_r13;
    uintptr_t reg_r14;
    uintptr_t reg_r15;
    uintptr_t reg_rflags;
};


// timekeeping

// `HZ` defines the number of timer interrupts per second, or ticks.
// Real kernels typically use 100 or 1000; Chickadee typically uses 100.
// Exception: Sanitizers slow down the kernel so much that recursive timer
// interrupts can become a problem, so when sanitizers are on, we reduce the
// interrupt frequency to 10 per second.
#if HAVE_SANITIZERS
# define HZ 10
#else
# define HZ 100
#endif

extern std::atomic<unsigned long> ticks;        // number of ticks since boot


// Segment selectors
#define SEGSEL_BOOT_CODE        0x8             // boot code segment
#define SEGSEL_KERN_CODE        0x8             // kernel code segment
#define SEGSEL_KERN_DATA        0x10            // kernel data segment
#define SEGSEL_APP_CODE         0x18            // application code segment
#define SEGSEL_APP_DATA         0x20            // application data segment
#define SEGSEL_TASKSTATE        0x28            // task state segment


// Physical memory size
#define MEMSIZE_PHYSICAL        0x200000
// Virtual memory size
#define MEMSIZE_VIRTUAL         0x300000
// Number of pages in physical memory
// TODO: define it in terms of other constants
#define PAGES_COUNT 512
#define MIN_ORDER 12
#define MAX_ORDER 21
#define ORDER_COUNT 10

enum memtype_t {
    mem_nonexistent = 0,
    mem_available = 1,
    mem_kernel = 2,
    mem_reserved = 3,
    mem_console = 4
};
extern memrangeset<16> physical_ranges;


// Hardware interrupt numbers
#define INT_IRQ                 32U
#define IRQ_TIMER               0
#define IRQ_KEYBOARD            1
#define IRQ_IDE                 14
#define IRQ_ERROR               19
#define IRQ_SPURIOUS            31

#define KTEXT_BASE              0xFFFFFFFF80000000UL
#define HIGHMEM_BASE            0xFFFF800000000000UL

inline uint64_t pa2ktext(uint64_t pa) {
    assert(pa < -KTEXT_BASE);
    return pa + KTEXT_BASE;
}

template <typename T>
inline T pa2ktext(uint64_t pa) {
    return reinterpret_cast<T>(pa2ktext(pa));
}

inline uint64_t ktext2pa(uint64_t ka) {
    assert(ka >= KTEXT_BASE);
    return ka - KTEXT_BASE;
}

template <typename T>
inline uint64_t ktext2pa(T* ptr) {
    return ktext2pa(reinterpret_cast<uint64_t>(ptr));
}


inline uint64_t pa2ka(uint64_t pa) {
    assert(pa < -HIGHMEM_BASE);
    return pa + HIGHMEM_BASE;
}

template <typename T>
inline T pa2kptr(uint64_t pa) {
    static_assert(std::is_pointer<T>::value, "T must be pointer");
    return reinterpret_cast<T>(pa2ka(pa));
}

inline uint64_t ka2pa(uint64_t ka) {
    assert(ka >= HIGHMEM_BASE && ka < KTEXT_BASE);
    return ka - HIGHMEM_BASE;
}

template <typename T>
inline uint64_t ka2pa(T* ptr) {
    return ka2pa(reinterpret_cast<uint64_t>(ptr));
}

inline uint64_t kptr2pa(uint64_t kptr) {
    assert(kptr >= HIGHMEM_BASE);
    return kptr - (kptr >= KTEXT_BASE ? KTEXT_BASE : HIGHMEM_BASE);
}

template <typename T>
inline uint64_t kptr2pa(T* ptr) {
    return kptr2pa(reinterpret_cast<uint64_t>(ptr));
}

template <typename T>
inline bool is_kptr(T* ptr) {
    uintptr_t va = reinterpret_cast<uint64_t>(ptr);
    return va >= HIGHMEM_BASE;
}

template <typename T>
inline bool is_ktext(T* ptr) {
    uintptr_t va = reinterpret_cast<uint64_t>(ptr);
    return va >= KTEXT_BASE;
}


template <typename T>
inline T read_unaligned(const uint8_t* x) {
    T a;
    memcpy(&a, x, sizeof(T));
    return a;
}
template <typename T>
inline T read_unaligned_pa(uint64_t pa) {
    return read_unaligned<T>(pa2kptr<const uint8_t*>(pa));
}
template <typename T, typename U>
inline T read_unaligned(const uint8_t* ptr, T (U::* member)) {
    alignas(U) char space[sizeof(U)] = {};
    U* dummy = (U*)(space);
    T a;
    memcpy(&a, ptr + (reinterpret_cast<uintptr_t>(&(dummy->*member)) - reinterpret_cast<uintptr_t>(dummy)), sizeof(T));
    return a;
}


// kalloc(sz)
//    Allocate and return a pointer to at least `sz` contiguous bytes
//    of memory. Returns `nullptr` if `sz == 0` or on failure.
//
//    If `sz` is a multiple of `PAGESIZE`, the returned pointer is guaranteed
//    to be page-aligned.
void* kalloc(size_t sz) __attribute__((malloc));

// kfree(ptr)
//    Free a pointer previously returned by `kalloc`. Does nothing if
//    `ptr == nullptr`.
void kfree(void* ptr);

// kfree_mem(pt, pg)
//    Free user-accessible memory of pagetable 'pt'
void kfree_mem(x86_64_pagetable* pt, proc_group* pg);

// kfree_mem(p)
//    Free user-accessible memory of process 'p'
void kfree_mem(proc* p);

// kfree_pagetable(pagetable)
//    Free the 'pagetable'
void kfree_pagetable(x86_64_pagetable* pagetable);

// operator new, operator delete
//    Expressions like `new (std::nothrow) T(...)` and `delete x` work,
//    and call kalloc/kfree.
void* operator new(size_t sz, const std::nothrow_t&) noexcept;
void* operator new(size_t sz, std::align_val_t al, const std::nothrow_t&) noexcept;
void* operator new[](size_t sz, const std::nothrow_t&) noexcept;
void* operator new[](size_t sz, std::align_val_t al, const std::nothrow_t&) noexcept;
void operator delete(void* ptr) noexcept;
void operator delete(void* ptr, size_t sz) noexcept;
void operator delete(void* ptr, std::align_val_t al) noexcept;
void operator delete(void* ptr, size_t sz, std::align_val_t al) noexcept;
void operator delete[](void* ptr) noexcept;
void operator delete[](void* ptr, size_t sz) noexcept;
void operator delete[](void* ptr, std::align_val_t al) noexcept;
void operator delete[](void* ptr, size_t sz, std::align_val_t al) noexcept;

// knew<T>(), knew<T>(args...)
//    Like `new (std::nothrow) T(args...)`.
template <typename T>
inline __attribute__((malloc)) T* knew() {
    return new (std::nothrow) T;
}
template <typename T, typename... Args>
inline __attribute__((malloc)) T* knew(Args&&... args) {
    return new (std::nothrow) T(std::forward<Args>(args)...);
}

// init_kalloc
//    Initialize stuff needed by `kalloc`. Called from `init_hardware`,
//    after `physical_ranges` is initialized.
void init_kalloc();


// Initialize hardware and CPUs
void init_hardware();

// Query machine configuration
unsigned machine_ncpu();
unsigned machine_pci_irq(int pci_addr, int intr_pin);

struct ahcistate;
extern ahcistate* sata_disk;


// Early page table (only kernel mappings)
extern x86_64_pagetable early_pagetable[3];

// Allocate and initialize a new, empty page table
x86_64_pagetable* kalloc_pagetable();

// Change current page table
void set_pagetable(x86_64_pagetable* pagetable);

// Print memory viewer
void console_memviewer(proc* p);


// Start the kernel
[[noreturn]] void kernel_start(const char* command);

// Turn off the virtual machine
[[noreturn]] void poweroff();

// Reboot the virtual machine
[[noreturn]] void reboot();

// Call after last process exits
[[noreturn]] void process_halt();


// log_printf, log_vprintf
//    Print debugging messages to the host's `log.txt` file. We run QEMU
//    so that messages written to the QEMU "parallel port" end up in `log.txt`.
__noinline void log_printf(const char* format, ...);
__noinline void log_vprintf(const char* format, va_list val);


// log_backtrace
//    Print a backtrace to the host's `log.txt` file, either for the current
//    stack or for a given stack range.
void log_backtrace(const char* prefix = "");
void log_backtrace(const char* prefix, uintptr_t rsp, uintptr_t rbp);


// lookup_symbol(addr, name, start)
//    Use the debugging symbol table to look up `addr`. Return the
//    corresponding symbol name (usually a function name) in `*name`
//    and the first address in that symbol in `*start`.
__no_asan
bool lookup_symbol(uintptr_t addr, const char** name, uintptr_t* start);


#if HAVE_SANITIZERS
// Sanitizer functions
void init_sanitizers();
void disable_asan();
void enable_asan();
void asan_mark_memory(unsigned long pa, size_t sz, bool poisoned);
#else
inline void disable_asan() {}
inline void enable_asan() {}
inline void asan_mark_memory(unsigned long pa, size_t sz, bool poisoned) {}
#endif


// `panicking == true` iff some CPU has panicked
extern std::atomic<bool> panicking;


// this_cpu
//    Return a pointer to the current CPU. Requires disabled interrupts.
inline cpustate* this_cpu() {
    assert(is_cli());
    cpustate* result;
    asm volatile ("movq %%gs:(0), %0" : "=r" (result));
    return result;
}

// current
//    Return a pointer to the current `struct proc`.
inline proc* current() {
    proc* result;
    asm volatile ("movq %%gs:(8), %0" : "=r" (result));
    return result;
}

// adjust_this_cpu_spinlock_depth(delta)
//    Adjust this CPU's spinlock_depth_ by `delta`. Does *not* require
//    disabled interrupts.
inline void adjust_this_cpu_spinlock_depth(int delta) {
    asm volatile ("addl %1, %%gs:%0"
                  : "+m" (*reinterpret_cast<int*>
                          (offsetof(cpustate, spinlock_depth_)))
                  : "er" (delta) : "cc", "memory");
}

// cpustate::contains(ptr)
//    Return true iff `ptr` lies within this cpustate's allocation.
inline bool cpustate::contains(void* ptr) const {
    return contains(reinterpret_cast<uintptr_t>(ptr));
}
inline bool cpustate::contains(uintptr_t addr) const {
    uintptr_t delta = addr - reinterpret_cast<uintptr_t>(this);
    return delta <= CPUSTACK_SIZE;
}

// proc::contains(ptr)
//    Return true iff `ptr` lies within this cpustate's allocation.
inline bool proc::contains(void* ptr) const {
    return contains(reinterpret_cast<uintptr_t>(ptr));
}
inline bool proc::contains(uintptr_t addr) const {
    uintptr_t delta = addr - reinterpret_cast<uintptr_t>(this);
    return delta <= PROCSTACK_SIZE;
}

// proc::resumable()
//    Return true iff this `proc` can be resumed (`regs_` or `yields_`
//    is set). Also checks some assertions about `regs_` and `yields_`.
inline bool proc::resumable() const {
    assert(!(regs_ && yields_));            // at most one at a time
    assert(!regs_ || contains(regs_));      // `regs_` points within this
    assert(!yields_ || contains(yields_));  // same for `yields_`
    return regs_ || yields_;
}

// proc::lock_pagetable_read()
//    Obtain a “read lock” on this process’s page table. While the “read
//    lock” is held, it is illegal to remove or change existing valid
//    mappings in that page table, or to free page table pages.
inline irqstate proc::lock_pagetable_read() {
    return irqstate();
}
inline void proc::unlock_pagetable_read(irqstate&) {
}

#endif
