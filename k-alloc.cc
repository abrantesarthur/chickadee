#include "kernel.hh"
#include "k-vmiter.hh"
#include "k-lock.hh"

static spinlock page_lock;
static uintptr_t next_free_pa;

#define MIN_ORDER 12
#define MAX_ORDER 21
#define ORDER_COUNT 10

struct page {
    bool free = false;
    int order = MIN_ORDER;
    uintptr_t addr = 0;
    list_links link_;
    
    inline uintptr_t size();
    inline uintptr_t first();    // first address in block
    inline uintptr_t last();     // last address in block
    inline uintptr_t buddy();     // first address of buddy 
    inline uintptr_t parent();
    inline bool left();
};

inline bool page::left() {
    return first() % (2 * size()) == 0;
}

inline uintptr_t page::size() {
    return 1<<(order);
}

inline uintptr_t page::first() {
    return addr - (addr % size());
}

inline uintptr_t page::last() {
    return first() + size() - 1;
}

inline uintptr_t page::buddy() {
    return left() ? first() + size() : first() - size();
}

inline uintptr_t page::parent() {
    // max order block has no parent
    return (left() || (order == MAX_ORDER)) ? first() : first() - size();
}

struct pageset {
    void init();
    void try_merge_all();
    void try_merge(page* p);
    page* get_page(uintptr_t addr);
    page* get_buddy(page* p);   // get first page in buddy block
    page* get_parent(page* p);
    void decrement_order(page*p);
    void free(page* p);
    page ps_[PAGES_COUNT];
};

// declare a free_blocks of block structures, each linked by their link_ member
list<page, &page::link_> free_blocks[ORDER_COUNT];
pageset pages;

void pageset::init() {
    auto irqs = page_lock.lock();
    for(uintptr_t pa = 0; pa < physical_ranges.limit(); pa += PAGESIZE) {
        if(physical_ranges.type(pa) == mem_available) {
            ps_[pa / PAGESIZE].free = true;
            ps_[pa / PAGESIZE].addr = pa;
            free_blocks[0].push_back(&ps_[pa / PAGESIZE]);
        }
    }
    page_lock.unlock(irqs);
    try_merge_all();
}


page* pageset::get_page(uintptr_t addr) {
    return &ps_[addr / PAGESIZE];
}


page* pageset::get_buddy(page* p) {
    return &ps_[p->buddy() / PAGESIZE];
}

page* pageset::get_parent(page* p) {
    return &ps_[p->parent() / PAGESIZE];
}


// TODO: should I always protect pages? What else should I protect? Some of these operations should be atomic
void pageset::try_merge(page* p) {

    // check if all pages within the block are free
    for(uintptr_t addr = p->first(); addr < p->last(); addr+=PAGESIZE) {
        // TODO: improve this
        if(pages.ps_[addr / PAGESIZE].free == 0) {
            return;
        }
    }

    // get buddy
    page* b = get_buddy(p);

    // check if all pages within the block are free
    for(uintptr_t addr = b->first(); addr < b->last(); addr+=PAGESIZE) {
         // TODO: improve this
        if(pages.ps_[addr / PAGESIZE].free == 0) {
            return;
        }
    }

    //buddy and block must have the same order
    // TODO: should i check all pages?
    if(p->order == b->order) {
        return;
    }
    

    // merge by increasing order of pages and updating free_blocks
    page* parent = get_parent(p);
    for(uintptr_t addr = parent->first(); addr < parent->last(); addr+= PAGESIZE) {
        ps_[addr].order = p->order + 1;
    }
    free_blocks[p->order - MIN_ORDER].erase(p);
    free_blocks[b->order - MIN_ORDER].erase(b);
    free_blocks[p->order + 1 - MIN_ORDER].push_back(p);

    try_merge(parent);   
}

void pageset::try_merge_all() {
    for(int i = 0; i < PAGES_COUNT; i++) {
        try_merge(&ps_[i]);
    }
}

void pageset::decrement_order(page* p) {
    if(p->order == MIN_ORDER) {
        return;
    }
    for(uintptr_t addr = p->first(); addr < p->last(); addr += PAGESIZE) {
        --ps_[addr / PAGESIZE].order;
    }
}

void pageset::free(page* p) {
    for (uintptr_t addr = p->first(); addr < p->last(); addr += PAGESIZE) {
        ps_[addr / PAGESIZE].free = false; 
    }
}

// init_kalloc
//    Initialize stuff needed by `kalloc`. Called from `init_hardware`,
//    after `physical_ranges` is initialized.
void init_kalloc() {
    pages.init();
}

// kalloc(sz)
//    Allocate and return a pointer to at least `sz` contiguous bytes of
//    memory. Returns `nullptr` if `sz == 0` or on failure.
//
//    The caller should initialize the returned memory before using it.
//    The handout allocator sets returned memory to 0xCC (this corresponds
//    to the x86 `int3` instruction and may help you debug).
//
//    If `sz` is a multiple of `PAGESIZE`, the returned pointer is guaranteed
//    to be page-aligned.
//
//    The handout code does not free memory and allocates memory in units
//    of pages.
void* kalloc(size_t sz) {
    //calculate order of allocation
    int order = msb(sz - 1);

    if(order > MAX_ORDER || order < MIN_ORDER) {
        return nullptr;
    }


    void* ptr = nullptr;

    // find a free block with desired order 
    page* p = free_blocks[order - MIN_ORDER].pop_front();
    if (p) {
        //use this block
        ptr = pa2kptr<void*>(p->first());
    } else {
        // find free block with order o > order, minimizing o.
        for(int o = order + 1; o < MAX_ORDER; o++) {
            // if found a block
            p = free_blocks[o - MIN_ORDER].pop_front();
            if(p) {
                break;
            }
        }

        if(!p) {
            // return nullptr
            return nullptr;
        }

        // splitting the block as much as possible
        while(p->order > order) {
            pages.decrement_order(p);
            free_blocks[p->order - MIN_ORDER].push_back(pages.get_buddy(p));
        }
    
        //use this block
        ptr = pa2kptr<void*>(p->first());
    }

    // set block's free status to false
    pages.free(p);

    if (ptr) {
        // tell sanitizers the allocated page is accessible
        asan_mark_memory(ka2pa(ptr), (1 << order), false);
        // initialize to `int3` | NOT SURE
        memset(ptr, 0xCC, (1 << order)); 
    }

    return ptr;
}

// kfree(ptr)
//    Free a pointer previously returned by `kalloc`. Does nothing if
//    `ptr == nullptr`.
void kfree(void* ptr) {
    // do nothing if ptr == nullptr
    if (!ptr) {
        return;
    }

    // convert to physical address
    uintptr_t block_pa = ka2pa(ptr);

    // prevent freeing reserved memory
    if(physical_ranges.type(block_pa) != mem_available) {
        return;
    }

    // tell sanitizers the freed page is inaccessible
    asan_mark_memory(block_pa, PAGESIZE, true);
    

    // free pages within that block
    page* p = pages.get_page(block_pa);
    pages.free(p);

    // TODO: should I not also add it to free_list?

    // try merging the block
    // try_merge() checks whether the freed block’s order-o buddy is also completely free
    // If it is, merge recursively coalesces them into a single free block of order o + 1.
    return pages.try_merge(p);
}

// kfree_proc(p)
//    Free the user-accessible memory of a process and the process itself
void kfree_proc(proc *p) {
    if(p) {
        for (vmiter it(p, 0); it.low(); it.next()) {
            if (it.user()) {
                it.kfree_page();
            }
        }
        kfree(p);
    }
}

// operator new, operator delete
//    Expressions like `new (std::nothrow) T(...)` and `delete x` work,
//    and call kalloc/kfree.
void* operator new(size_t sz, const std::nothrow_t&) noexcept {
    return kalloc(sz);
}
void* operator new(size_t sz, std::align_val_t, const std::nothrow_t&) noexcept {
    return kalloc(sz);
}
void* operator new[](size_t sz, const std::nothrow_t&) noexcept {
    return kalloc(sz);
}
void* operator new[](size_t sz, std::align_val_t, const std::nothrow_t&) noexcept {
    return kalloc(sz);
}
void operator delete(void* ptr) noexcept {
    kfree(ptr);
}
void operator delete(void* ptr, size_t) noexcept {
    kfree(ptr);
}
void operator delete(void* ptr, std::align_val_t) noexcept {
    kfree(ptr);
}
void operator delete(void* ptr, size_t, std::align_val_t) noexcept {
    kfree(ptr);
}
void operator delete[](void* ptr) noexcept {
    kfree(ptr);
}
void operator delete[](void* ptr, size_t) noexcept {
    kfree(ptr);
}
void operator delete[](void* ptr, std::align_val_t) noexcept {
    kfree(ptr);
}
void operator delete[](void* ptr, size_t, std::align_val_t) noexcept {
    kfree(ptr);
}
