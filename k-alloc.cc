#include "kernel.hh"
#include "k-vmiter.hh"
#include "k-lock.hh"

// TODO: implement changes starting from 'implmeent buddy allocator' commit

static spinlock page_lock;
static uintptr_t next_free_pa;

#define MIN_ORDER 12
#define MAX_ORDER 21
#define ORDER_COUNT 10

// structure holding the beginning and end of a memory block
struct Block {
    uintptr_t begin_, end_;
    uintptr_t buddy_addr_;
    list_links link_;
};

struct Page {
    bool free = 0;
    int order = MIN_ORDER;
};

// declare a free_list of Block structures, each linked by their link_ member
list<Block, &Block::link_> free_list[ORDER_COUNT];
Block block_table[ORDER_COUNT][PAGES_COUNT];
Page pages[PAGES_COUNT];

uintptr_t block_number(int order, uintptr_t addr) {
    return addr / ((1<<(order - MIN_ORDER)) * PAGESIZE);
}

int get_buddy_addr(int block_order, uintptr_t block_addr) {
    int blk_number = block_number(block_order, block_addr);
    uintptr_t offset = (1 << (block_order - MIN_ORDER)) * PAGESIZE;
    return (blk_number % 2 == 0) ? block_addr + offset : block_addr - offset;
}

void merge(uintptr_t block_addr) {

    //get block
    int blk_order = pages[block_addr / PAGESIZE].order;
    uintptr_t blk_number = block_number(blk_order, block_addr);
    Block* block = &block_table[blk_order - 12][blk_number];

    // check if block is completely free
    for(uintptr_t addr = block->begin_; addr < block->end_; addr+=PAGESIZE) {
        if(pages[addr / PAGESIZE].free == 0) {
            return;
        }
    }

    // get buddy
    uintptr_t buddy_addr = block->buddy_addr_;
    int buddy_order = pages[buddy_addr / PAGESIZE].order;
    uintptr_t buddy_number = block_number(buddy_order, buddy_addr);
    Block* buddy = &block_table[buddy_order - 12][buddy_number];

    //check if buddy and block have the same order
    if(pages[buddy_addr / PAGESIZE].order != blk_order) {
        return;
    }

    // check if buddy is completely free and not reserved
    for(uintptr_t addr = buddy->begin_; addr < buddy->end_; addr+=PAGESIZE) {
        if(pages[addr / PAGESIZE].free == 0) {
            return;
        }
    }


    // create new block to be pushed
    static Block* new_block;
    if(blk_number % 2 == 0) {   // buddy is after block in physical memory
        new_block =
            &block_table[blk_order - 12 + 1][block_number(blk_order + 1, block->begin_)];
    } else {                    // buddy is before block in physical memory
        new_block =
            &block_table[blk_order - 12 + 1][block_number(blk_order + 1, buddy->begin_)];
    }

    // remove block and buddy from free_list of order
    free_list[blk_order - MIN_ORDER].erase(block);
    free_list[blk_order - MIN_ORDER].erase(buddy);

    // push new block to free_list entry of order + 1
    free_list[blk_order + 1 - MIN_ORDER].push_back(new_block);

    //update pages
    for(uintptr_t addr = block->begin_; addr < block->end_; addr+=PAGESIZE) {
        pages[block->begin_ / PAGESIZE].order = blk_order + 1;
    }
    for(uintptr_t addr = buddy->begin_; addr < buddy->end_; addr+=PAGESIZE) {
        pages[buddy->begin_ / PAGESIZE].order = blk_order + 1;
    }

    merge(new_block->begin_);   
}

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
    inline bool left();
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

inline bool page::left() {
    return first() % (2 * size()) == 0;
}

inline uintptr_t page::buddy() {
    return left() ? first() + size() : first() - size();
}

inline uintptr_t page::parent() {
    // max order block has no parent
    return (left() || (order == MAX_ORDER)) ? first() : first() - size();
}

inline void page::print_page() {
    log_printf("addr: %p | buddy: %p | block: %p - %p | parent: %p | %s | order: %d\n", addr, buddy(), first(), last(), parent(), status == pg_free ? "free" : (status == pg_unavailable ? "unavailable" : "allocated"), order);
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
    uintptr_t index(uintptr_t addr);  // get the index of page at address addr
    page ps_[PAGES_COUNT];

    // helper functions
    void print_pageset();
};

// declare a free_blocks of block structures, each linked by their link_ member
list<page, &page::link_> free_blocks[ORDER_COUNT];
pageset pagess;

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
    return addr / PAGESIZE;
}

page* pageset::get_buddy(page* p) {
    return &ps_[index(p->buddy())];
}

page* pageset::get_parent(page* p) {
    return &ps_[index(p->parent())];
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
    try_merge_all();
}

void pageset::try_merge_all() {
    for(int i = 0; i < PAGES_COUNT; i++) {
        try_merge(&ps_[i]);
    }
}

// TODO: should I always protect pages? What else should I protect? Some of these operations should be atomic
void pageset::try_merge(page* p) {
    // check if all block pages are free
    for(uintptr_t addr = p->first(); addr < p->last(); addr += PAGESIZE) {
        if(ps_[index(addr)].status != pg_free) {
            return;
        }
    }

    // check if all buddy pages are free
    page* b = get_buddy(p);
    for(uintptr_t addr = b->first(); addr < b->last(); addr+=PAGESIZE) {
        if(ps_[index(addr)].status != pg_free) {
            return;
        }
    }

    //buddy and block must have the same order
    // TODO: why not put everything in the previous loops?
    int order = p->order;
    for(uintptr_t addr = p->first(); addr < p->last(); addr += PAGESIZE) {
        if(ps_[index(addr)].order != order) {
            return;
        }
    }
    for(uintptr_t addr = b->first(); addr < b->last(); addr+=PAGESIZE) {
        if(ps_[index(addr)].order != order) {
            return;
        }
    }


    // merge by increasing order of pages and updating free_blocks
    // TODO: improve this
    uintptr_t first = p->first();
    uintptr_t last = p->last();
    page* parent = p->left() ? p : b;
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

    print_freeblocks(10);

    try_merge(parent);
}

// init_kalloc
//    Initialize stuff needed by `kalloc`. Called from `init_hardware`,
//    after `physical_ranges` is initialized.
void init_kalloc() {

    pagess.init();

    // initialize block_table
    int rows;
    Block* blk;
    for (int col = 0; col < ORDER_COUNT; col++) {
        rows = PAGES_COUNT / (1 << col);
        for (int row = 0; row < rows; row++) {
            // set begin and end addresses of block of memory of given col
            blk = &block_table[col][row];
            blk->begin_ = row * (1<<(col)) * PAGESIZE ;
            blk->end_ = (row + 1) * (1<<(col)) * PAGESIZE - 1;
            blk->buddy_addr_ = ((row % 2) == 0) ? blk->end_ + 1 :
                                blk->begin_ - PAGESIZE * (1<<(col)) ;
        }
    }

    //Initialize free list's first order column and pages
    auto irqs = page_lock.lock();
    for(uintptr_t pa = 0; pa < physical_ranges.limit(); pa += PAGESIZE) {
        if(physical_ranges.type(pa) == mem_available) {
            //add block to free_list[0]
            free_list[0].push_back(&block_table[0][pa / PAGESIZE]);
            // mark block as free and with order 12
            pages[pa / PAGESIZE].free = true;
            pages[pa / PAGESIZE].order = MIN_ORDER;
        } 
    }
    page_lock.unlock(irqs);
    //
    // merge
    for(int row = 0; row < PAGES_COUNT; row++) {
        merge(row * PAGESIZE);
    }    
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
    Block* block = free_list[order - MIN_ORDER].pop_front();
    if (block) {
        //use this block
        ptr = pa2kptr<void*>(block->begin_);
    } else {

        Block* first_block;
        Block* second_block;
        // find free block with order o > order, minimizing o.
        for(int o = order + 1; o < MAX_ORDER; o++) {
            // if found a block
            if((block = free_list[o - MIN_ORDER].pop_front())) {
                break;
            }
        }

        if(!block) {
            // return nullptr
            return nullptr;
        }

        // traverse down the list
        for(int o = pages[block->begin_ / PAGESIZE].order; o > order; o--) {
            //divide the block into two
            // buddy is after block in physical memory
            if (block_number(o - 1, block->begin_) % 2 == 0) {  
                first_block = &block_table[o - MIN_ORDER - 1][block_number(o-1,block->begin_)];
                second_block = &block_table[o - MIN_ORDER - 1][block_number(o-1,get_buddy_addr(o-1, block->begin_))];   
            } else { // buddy is before block in physical memory
                first_block = &block_table[o - MIN_ORDER - 1][block_number(o-1, get_buddy_addr(o-1, block->begin_))];
                second_block = &block_table[o - MIN_ORDER - 1][block_number(o-1, block->begin_)];
            }

            //update pages orders
            pages[first_block->begin_ /PAGESIZE].order = o - 1;
            pages[second_block->begin_ /PAGESIZE].order = o - 1;

            //update free_list
            free_list[o - MIN_ORDER - 1].push_back(second_block);
            block = first_block;
        }
    
        //use this block
        ptr = pa2kptr<void*>(block->begin_);
     }

    // set block's free status to false
    for (uintptr_t addr = block->begin_; addr < block->end_; addr += PAGESIZE) {
        pages[addr / PAGESIZE].free = false; 
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
    uintptr_t block_addr = ka2pa(ptr);

    // prevent freeing reserved memory
    if(physical_ranges.type(block_addr) != mem_available) {
        return;
    }

    // tell sanitizers the freed page is inaccessible
    asan_mark_memory(ka2pa(ptr), PAGESIZE, true);
    
    // let o be the order of the freed block.
    int order = pages[block_addr / PAGESIZE].order;

    // get block
    Block* block = &block_table[order - MIN_ORDER][block_number(order, block_addr)];

    // update pages, setting block's pages to free
    for(uintptr_t addr = block->begin_; addr < block->end_; addr+=PAGESIZE){
        pages[addr / PAGESIZE].free = true;
    }

    // try merging the block
    // merge() checks whether the freed block’s order-o buddy is also completely free
    // If it is, merge recursively coalesces them into a single free block of order o + 1.
    return merge(block_addr);
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