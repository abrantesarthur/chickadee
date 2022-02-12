#include "kernel.hh"
#include "k-vmiter.hh"
#include "k-lock.hh"

// TODO: implement changes starting from 'implmeent buddy allocator' commit

static spinlock page_lock;
static uintptr_t next_free_pa;

#define MIN_ORDER 12
#define MAX_ORDER 21
#define ORDER_COUNT 10

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

// TODO: comment that this is in relation to parent
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

// TODO: should I rename this to blocks?
struct pageset {
    void init();
    void try_merge_all();
    void try_merge(page* p);
    page* get_page(uintptr_t addr);     // returns page at addr
    page* get_block(uintptr_t addr);    // gets first page of the block
    page* get_buddy(page* p);   // get first page in buddy block
    page* get_parent(page* p);
    void decrement_order(page*p);
    void set_status(page* p, pagestatus_t s);     // update block's status
    void free(page* p);     // free the block
    void allocate(page* p);     // allocate the block
    bool is_free(page* b);  // returns true if all pages within block are free
    bool has_order(page* b, int o);  // returns true if all pages within block are free
    uintptr_t index(uintptr_t addr);  // get the index of page at address addr
    // helper functions
    void print_block(page* p);
    void print_pageset();

    private:
        page ps_[PAGES_COUNT];
};

// declare a free_blocks of block structures, each linked by their link_ member
// TODO: turn this into a struct that allows me to pop and push blocks at specific indexes
list<page, &page::link_> free_blocks[ORDER_COUNT];
pageset pagess;

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

void print_freeblocks(unsigned count) {
    for(int i = 0; i < ORDER_COUNT; i++) {
        page* p = free_blocks[i].front();
        log_printf("ORDER %d =========================================\n", i + MIN_ORDER);
        unsigned iteration = 0;
        while(p && iteration < count) {
            log_printf("%p - %p\n", p->first(), p->last());
            p = free_blocks[i].next(p);
            iteration++;
        }
        log_printf("\n");
    }
}

// TODO: cast to int
uintptr_t pageset::index(uintptr_t addr) {
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

void pageset::decrement_order(page* p) {
    uintptr_t first = p->first();
    uintptr_t last = p->last();
    for(uintptr_t addr = first; addr < last; addr += PAGESIZE) {
        ps_[index(addr)].order--;
    }
}

void pageset::init() {
    auto irqs = page_lock.lock();
    for(uintptr_t pa = 0; pa < physical_ranges.limit(); pa += PAGESIZE) {
        if(physical_ranges.type(pa) == mem_available) {
            ps_[index(pa)].status = pg_free;
            ps_[index(pa)].addr = pa;
            free_blocks[0].push_back(&ps_[index(pa)]);
        }
    }
    page_lock.unlock(irqs);
}

void pageset::try_merge_all() {
    for(int i = 0; i < PAGES_COUNT; i++) { 
        try_merge(&ps_[i]);
    }
}

// TODO: should I always protect pages? What else should I protect? Some of these operations should be atomic
void pageset::try_merge(page* p) {
    // check if block and its buddy are both free
    page* b = get_buddy(p);
    if(!is_free(p) || !is_free(b)) {
        return;
    }


    //buddy and block must have the same order
    // TODO: put everything in the previous loops?
    int order = p->order;
    if(!has_order(p, order) || !has_order(b, order)) {
        return;
    }


    // merge by increasing order of pages and updating free_blocks
    // TODO: improve this
    uintptr_t first = p->first();
    uintptr_t last = p->last();
    page* parent = p->is_left() ? p : b;
    for(uintptr_t addr = first; addr < last; addr += PAGESIZE) {
        ps_[index(addr)].order += 1;
    }
    first = b->first();
    last = b->last();
    for(uintptr_t addr = first; addr < last; addr+=PAGESIZE) {
        ps_[index(addr)].order += 1;
    }

    free_blocks[order - MIN_ORDER].erase(p);
    free_blocks[order - MIN_ORDER].erase(b);
    free_blocks[order + 1 - MIN_ORDER].push_back(parent);
    
    try_merge(parent);
}


// init_kalloc
//    Initialize stuff needed by `kalloc`. Called from `init_hardware`,
//    after `physical_ranges` is initialized.
void init_kalloc() {
    pagess.init();
    pagess.try_merge_all(); 
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

// TODO: think about lock strategy for pages datastructure!
void* kalloc(size_t sz) {
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
    page* p = free_blocks[order - MIN_ORDER].pop_front();
    if(p) {
        // if found, use this block
        ptr = pa2kptr<void*>(p->first());

       // assert invariant
       assert(p->order == MAX_ORDER || !pagess.is_free(pagess.get_buddy(p)));
       assert(pagess.is_free(p));
    } else {
        // if not found, look for block with order o > order, minimizing o
        for(int o = order + 1; o <= MAX_ORDER; o++) {
            p = free_blocks[o - MIN_ORDER].pop_front();
            if(p) {
                // if found block, assert invariants and stop looking
                assert(o == p->order);
                assert(p->order == MAX_ORDER || !pagess.is_free(pagess.get_buddy(p)));
                assert(pagess.is_free(p));
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
            pagess.decrement_order(p);
            b = pagess.get_buddy(p);
            assert(p->order == b->order);
            free_blocks[b->order - MIN_ORDER].push_back(b);
        }


        //use found block
        // TODO: move this after else
        ptr = pa2kptr<void*>(p->first());
    }

     // at this point, block should have the desired order
    assert(p->order == order);

    // set block's status to allocated
    pagess.allocate(p);

    // TODO: can ptr even be null at this point

    if (ptr) {
        // tell sanitizers the allocated page is accessible
        asan_mark_memory(ka2pa(ptr), p->size(), false);
        // initialize to `int3` | NOT SURE
        memset(ptr, 0xCC, p->size()); 
    }
    return ptr;
}

// kfree(ptr)
//    Free a pointer previously returned by `kalloc`. Does nothing if
//    `ptr == nullptr`.
void kfree(void* ptr) {
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
    page* p = pagess.get_block(pa);

    // pa must be memory returned by kalloc
    assert(p->first() == pa);

    log_printf("BLOCK FIRST ADDRS: %p\n", p->first());

    // TODO: this should be an atomic operation!
    // set pages within block to free
    pagess.free(p);
    // add block to free_blocks list
    free_blocks->push_back(p);


    // tell sanitizers the freed block is inaccessible
    // TODO: move this to the end
    asan_mark_memory(pa, p->size(), true);


    // try merging the block
    return pagess.try_merge(p);
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