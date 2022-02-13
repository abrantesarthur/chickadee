#include "kernel.hh"
#include "k-vmiter.hh"
#include "k-lock.hh"

static spinlock page_lock;
static uintptr_t next_free_pa;

enum pagestatus_t {
    pg_unavailable = 0,   // the page cannot be allocated
    pg_free = 1,          // the page is free to be allocated
    pg_allocated = 2,     // the page is already allocated
};

struct page {
    pagestatus_t status = pg_unavailable; 
    int order = MIN_ORDER;
    uintptr_t addr = 0;
    list_links link_;
    
    inline uintptr_t size();    // size of the block
    inline uintptr_t first();    // first address in block
    inline uintptr_t last();     // last address in block
    inline uintptr_t middle();  // middle address in the block
    inline bool is_left();
    inline uintptr_t buddy();     // first address of buddy 
    inline uintptr_t parent(); 
    inline void print_page();
};

inline uintptr_t page::size() {
    return 1<<(order);
}

inline uintptr_t page::first() {
    return addr - (addr % size());
}

inline uintptr_t page::last() {
    return first() + size() - 1;
}

inline uintptr_t page::middle() {
    return first() + size() / 2;
}

inline bool page::is_left() {
    return first() % (2 * size()) == 0;
}

inline uintptr_t page::buddy() {
    return is_left() ? first() + size() : first() - size();
}

inline uintptr_t page::parent() {
    // max order block has no parent
    return (is_left() || (order == MAX_ORDER)) ? first() : first() - size();
}

inline void page::print_page() {
    log_printf("addr: %p | buddy: %p | block: %p - %p | parent: %p | %s | order: %d\n", addr, buddy(), first(), last(), parent(), status == pg_free ? "free" : (status == pg_unavailable ? "unavailable" : "allocated"), order);
}

struct pageset {
    void init();
    void try_merge_all();
    void try_merge(page* p);
    page* get_page(uintptr_t addr);     // returns page at addr
    page* get_block(uintptr_t addr);    // gets first page of the block
    page* get_buddy(page* p);   // get first page in buddy block
    page* get_parent(page* p);
    void increment_order(page*p);
    void increment_order_by(page*p, int v);
    void decrement_order(page*p);
    void set_status(page* p, pagestatus_t s);     // update block's status
    void free(page* p);     // free the block
    void allocate(page* p);     // allocate the block
    bool is_free(page* b);  // returns true if all pages within block are free
    bool has_order(page* b, int o);  // returns true if all pages within block are free
    uint32_t index(uintptr_t addr);  // get the index of page at address addr

    void freeblocks_push(page* p);
    page* freeblocks_pop(int o);
    void freeblocks_erase(page* p, int o);
        // helper functions
    void print_block(page* p);
    void print_pageset();
    void print_freeblocks(unsigned count);



    private:
        page ps_[PAGES_COUNT];
        list<page, &page::link_> fbs_[ORDER_COUNT];
}; 


pageset pages;
void pageset::print_block(page* p) {
    // print pages
    for(uintptr_t addr = p->first(); addr < p->last(); addr += PAGESIZE) {
        ps_[index(addr)].print_page();
    }
}

void pageset::print_pageset() {
    // print pages
    for(uintptr_t i = 0; i < PAGES_COUNT; i++) {
        ps_[i].print_page();
    }
}

void pageset::print_freeblocks(unsigned count) {
    for(int i = 0; i < ORDER_COUNT; i++) {
        page* p = fbs_[i].front();
        log_printf("ORDER %d =========================================\n", i + MIN_ORDER);
        unsigned iteration = 0;
        while(p && iteration < count) {
            log_printf("%p - %p\n", p->first(), p->last());
            p = fbs_[i].next(p);
            iteration++;
        }
        log_printf("\n");
    }
}

uint32_t pageset::index(uintptr_t addr) {
    assert(addr < physical_ranges.limit());
    assert(addr % PAGESIZE == 0);
    return addr / PAGESIZE;
}

page* pageset::get_buddy(page* p) {
    return &ps_[index(p->buddy())];
}

page* pageset::get_parent(page* p) {
    return &ps_[index(p->parent())];
}

page* pageset::get_page(uintptr_t addr) {
    return &ps_[index(addr)];
}

page* pageset::get_block(uintptr_t addr) {
    return &ps_[index(get_page(addr)->first())];
}

bool pageset::is_free(page* p) {
    for(uintptr_t addr = p->first(); addr < p->last(); addr += PAGESIZE) {
        if(ps_[index(addr)].status != pg_free) {
            return false;
        }
    }
    return true;
}

bool pageset::has_order(page* p, int order) {
    for(uintptr_t addr = p->first(); addr < p->last(); addr += PAGESIZE) {
        if(ps_[index(addr)].order != order) {
            return false;
        }
    }
    return true;
}

void pageset::set_status(page* p, pagestatus_t s) {
    for(uintptr_t addr = p->first(); addr < p->last(); addr += PAGESIZE) {
        ps_[index(addr)].status = s;
    }
}

void pageset::free(page* p) {
    set_status(p, pg_free);
}

void pageset::allocate(page* p) {
    set_status(p, pg_allocated);
}

void pageset::increment_order_by(page* p, int v) {
    uintptr_t first = p->first();
    uintptr_t last = p->last();
    for(uintptr_t addr = first; addr < last; addr += PAGESIZE) {
        ps_[index(addr)].order += v;
    }
}


void pageset::increment_order(page* p) {
    increment_order_by(p, 1);
}

void pageset::decrement_order(page* p) {
    increment_order_by(p, -1);
}

void pageset::freeblocks_push(page* p) {
    fbs_[p->order - MIN_ORDER].push_back(p);
}

page* pageset::freeblocks_pop(int o) {
    return fbs_[o - MIN_ORDER].pop_front();
}

void pageset::freeblocks_erase(page* p, int o) {
    fbs_[o - MIN_ORDER].erase(p);
}

void pageset::init() {
    auto irqs = page_lock.lock();
    for(uintptr_t pa = 0; pa < physical_ranges.limit(); pa += PAGESIZE) {
        if(physical_ranges.type(pa) == mem_available) {
            ps_[index(pa)].status = pg_free;
            ps_[index(pa)].addr = pa;
            freeblocks_push(&ps_[index(pa)]);
        }
    }
    page_lock.unlock(irqs);
}

void pageset::try_merge_all() {
    for(int i = 0; i < PAGES_COUNT; i++) { 
        auto irqs = page_lock.lock();
        try_merge(&ps_[i]);
        page_lock.unlock(irqs);
    }
}

// TODO: should I always protect pages? What else should I protect? Some of these operations should be atomic
void pageset::try_merge(page* p) {
    // block and buddy must be free
    page* b = get_buddy(p);
    if(!is_free(p) || !is_free(b)) {
        return;
    }

    // block and buddy must have the same order
    if(!has_order(p, p->order) || !has_order(b, p->order)) {
        return;
    }
    
    // get left block before altering their order
    page* l = p->is_left() ? p : b;

    // merge and recurse
    increment_order(p);
    increment_order(b);
    freeblocks_erase(p, p->order - 1);
    freeblocks_erase(b, b->order - 1);
    freeblocks_push(l);
    try_merge(l);
}


// init_kalloc
//    Initialize stuff needed by `kalloc`. Called from `init_hardware`,
//    after `physical_ranges` is initialized.
void init_kalloc() {
    pages.init();
    pages.try_merge_all(); 
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

// TODO: return correct error values on failure
void* kalloc(size_t sz) {
    // spinlock_guard guard(page_lock);

    // validate size
    if(sz == 0) {
        return nullptr;
    }

    // validate order of allocation
    const int order = msb(sz - 1) < MIN_ORDER ? MIN_ORDER : msb(sz - 1);
    if(order > MAX_ORDER) {
        return nullptr;
    }

    void* ptr = nullptr;

    // look for a free block with the desired order
    page* p = pages.freeblocks_pop(order);
    if(p) {
       // assert invariant
       assert(p->order == MAX_ORDER || !pages.is_free(pages.get_buddy(p)));
       assert(pages.is_free(p));
    } else {
        // if not found, look for block with order o > order, minimizing o
        for(int o = order + 1; o <= MAX_ORDER; o++) {
            p = pages.freeblocks_pop(o);
            if(p) {
                // if found block, assert invariants and stop looking
                assert(o == p->order);
                assert(p->order == MAX_ORDER || !pages.is_free(pages.get_buddy(p)));
                assert(pages.is_free(p));
                break;
            }
        }

        if(!p) {
            // no memory available
            return nullptr;
        }

        // split block into two and free one of them as much as possible
        page* b;
        for(int o = p->order; o > order; o--) {
            pages.decrement_order(p);
            b = pages.get_buddy(p);
            assert(p->order == b->order);
            pages.freeblocks_push(b);
        }
    }

    // found block
    ptr = pa2kptr<void*>(p->first());

     // at this point, block should have the desired order
    assert(p->order == order);

    // set block's status to allocated
    pages.allocate(p);

    // tell sanitizers the allocated page is accessible
    asan_mark_memory(ka2pa(ptr), p->size(), false);
    // initialize to `int3`
    memset(ptr, 0xCC, p->size());

    log_printf("BLOCK: %p - %p | %d\n", p->first(), p->last(), p->order);

    return ptr;
}

// kfree(ptr)
//    Free a pointer previously returned by `kalloc`. Does nothing if
//    `ptr == nullptr`.
void kfree(void* ptr) {
    spinlock_guard guard(page_lock);
    
    if(!ptr) {
        return;
    }
    // get physical address
    uintptr_t pa = ka2pa(ptr);

    // prevent freeing reserved memory
    if(physical_ranges.type(pa) != mem_available) {
        return;
    }

    // get block
    page* p = pages.get_block(pa);

    // pa must be memory returned by kalloc
    assert(p->first() == pa);


    // TODO: this should be an atomic operation!
    // set pages within block to free
    pages.free(p);
    pages.freeblocks_push(p);


    // tell sanitizers the freed block is inaccessible
    asan_mark_memory(pa, p->size(), true);


    // try merging the block
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
void *operator new(size_t sz, const std::nothrow_t &) noexcept
{
    return kalloc(sz);
}
void *operator new(size_t sz, std::align_val_t, const std::nothrow_t &) noexcept
{
    return kalloc(sz);
}
void *operator new[](size_t sz, const std::nothrow_t &) noexcept
{
    return kalloc(sz);
}
void *operator new[](size_t sz, std::align_val_t, const std::nothrow_t &) noexcept
{
    return kalloc(sz);
}
void operator delete(void *ptr)noexcept
{
    kfree(ptr);
}
void operator delete(void *ptr, size_t)noexcept
{
    kfree(ptr);
}
void operator delete(void *ptr, std::align_val_t)noexcept
{
    kfree(ptr);
}
void operator delete(void *ptr, size_t, std::align_val_t)noexcept
{
    kfree(ptr);
}
void operator delete[](void *ptr) noexcept
{
    kfree(ptr);
}
void operator delete[](void *ptr, size_t) noexcept
{
    kfree(ptr);
}
void operator delete[](void *ptr, std::align_val_t) noexcept
{
    kfree(ptr);
}
void operator delete[](void *ptr, size_t, std::align_val_t) noexcept
{
    kfree(ptr);
}