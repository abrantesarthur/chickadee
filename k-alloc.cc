#include "kernel.hh"
#include "k-vmiter.hh"
#include "k-lock.hh"

static spinlock page_lock;
static uintptr_t next_free_pa;

#define MIN_ORDER 12
#define MAX_ORDER 21
#define ORDER_COUNT 10

////////////////////////////////////////////////////////////////////////////
struct page {
    // TODO: make private
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

inline bool left() {
    return first() % (2 * size()) == 0;
}

uintptr_t page::size() {
    return 1<<(order);
}

uintptr_t page::first() {
    return addr - (addr % size());
}

uintptr_t page::last() {
    return first() + size() - 1;
}

uintptr_t page::buddy() {
    return left() ? first() + size() : first() - size();
}

uintptr_t page::parent() {
    // max order block has no parent
    return (left() || (order == MAX_ORDER)) ? first() : first() - size();
}

struct pageset {
    void init();
    void try_merging_all();
    void try_merge(page* p);
    page* get_buddy(page* p);   // get first page in buddy block
    page* get_parent(page* p);
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

    try_merging_all();
}

page* pageset::get_buddy(page* p) {
    return ps_[p->buddy() / PAGESIZE];
}

page* pageset::get_parent(page* p) {
    return ps_[p->parent() / PAGESIZE];
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

// TODO: rename to try_merge
// TODO: make it iterate over pages
void pageset::try_merging_all() {
    for(int i = 0; i < PAGES_COUNT; i++) {
        try_merge(&ps_[i]);
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
    block* blk = free_blocks[order - MIN_ORDER].pop_front();
    if (blk) {
        //use this block
        ptr = pa2kptr<void*>(blk->first_);
    } else {
        // find free block with order o > order, minimizing o.
        for(int o = order + 1; o < MAX_ORDER; o++) {
            // if found a block
            blk = free_blocks[o - MIN_ORDER].pop_front();
            if(blk) {
                break;
            }
        }

        if(!blk) {
            // return nullptr
            return nullptr;
        }

        // splitting the block as much as possible
        block* left_blk;
        block* right_blk;
        for(int o = blk->order_; o > order; o--) {
            left_blk = btable.get_block(blk->first_, o - 1);
            right_blk = btable.get_block(left_blk->buddy_addr_, o - 1);  

            // swap blocks if necessary
            if (left_blk->index_ % 2 != 0) {  
                block* tmp_blk = left_blk;
                left_blk = right_blk;
                right_blk = tmp_blk;  
            }

            //update pages orders
            // TODO: should I not update all pages within that block
            pages.ps_[left_blk->first_ / PAGESIZE].order = o - 1;
            pages.ps_[right_blk->first_ / PAGESIZE].order = o - 1;

            //update free_blocks
            free_blocks[o - MIN_ORDER - 1].push_back(right_blk);
            blk = left_blk;
        }
    
        //use this block
        ptr = pa2kptr<void*>(blk->first_);
    }

    // set block's free status to false
    for (uintptr_t addr = blk->first_; addr < blk->last_; addr += PAGESIZE) {
        pages.ps_[addr / PAGESIZE].free = false; 
    }

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
    

    // get block
    int order = pages.ps_[block_pa / PAGESIZE].order;
    block* blk = btable.get_block(block_pa, order);

    // free pages within that block
    for(uintptr_t addr = blk->first_; addr < blk->last_; addr+=PAGESIZE){
        // assert that pages within the block have the same order and are notfree
        assert(pages.ps_[addr / PAGESIZE].free == false);
        assert(pages.ps_[addr / PAGESIZE].order == order);

        // free pages
        pages.ps_[addr / PAGESIZE].free = true;
    }

    // TODO: should I not also add it to free_list?

    // try merging the block
    // try_merge() checks whether the freed block’s order-o buddy is also completely free
    // If it is, merge recursively coalesces them into a single free block of order o + 1.
    return try_merge(block_pa);
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
