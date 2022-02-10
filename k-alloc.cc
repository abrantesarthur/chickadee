#include "kernel.hh"
#include "k-vmiter.hh"
#include "k-lock.hh"

static spinlock page_lock;
static uintptr_t next_free_pa;

#define MIN_ORDER 12
#define MAX_ORDER 21
#define ORDER_COUNT 10

////////////////////////////////////////////////////////////////////////////
// TODO: how to make the properties constant?
struct block {
        uintptr_t first_;            // first address in the block
        uintptr_t last_;            // last address in the block
        uintptr_t size_;            // size of this block                       
        uintptr_t buddy_addr_;      // buddy's first address
        int order_;                 // order of this block
        int index_;                 // the index of this block
        list_links link_;
};

struct blocktable {
    public:
        void init();

        // TODO: make it return unsigned
        uintptr_t block_number(int order, uintptr_t addr);
        block* get_block(uintptr_t addr);
        block* get_block(uintptr_t addr, int order);
        // TODO: can I make this a block method?
        block* get_parent(block* b);

        // get address of buddy of block at address 'addr'
        // TODO: make it return uintptr_t.
        int get_buddy_addr(int order, uintptr_t addr);
        // TODO: make it private
        block t_[ORDER_COUNT][PAGES_COUNT];
};


struct page {
    bool free = 0;
    int order = MIN_ORDER;
};

// declare datastructure that keeps track of blocks of a given order
blocktable btable;
// declare a free_blocks of block structures, each linked by their link_ member
list<block, &block::link_> free_blocks[ORDER_COUNT];
page pages[PAGES_COUNT];

void blocktable::init() {
    block* b;
    for (int o = 0; o < ORDER_COUNT; o++) {
        for (int i = 0; i < PAGES_COUNT / (1 << o); i++) {
            // set begin and end addresses of block of memory of given o
            b = &t_[o][i];
            b->size_ = (1<<(o)) * PAGESIZE;
            b->first_ = i * b->size_;
            b->last_ = b->first_ + b->size_ - 1;
            b->buddy_addr_ = ((i % 2) == 0) ? b->last_ + 1 : b->first_ - b->size_;
            b->order_ = o + MIN_ORDER;
            b->index_ = i;
        }
    }
}

uintptr_t blocktable::block_number(int order, uintptr_t addr) {
    return addr / ((1<<(order - MIN_ORDER)) * PAGESIZE);
}

// TODO: there is probably room for improvement!
int blocktable::get_buddy_addr(int order, uintptr_t addr) {
    int i = block_number(order, addr);
    uintptr_t offset = (1 << (order - MIN_ORDER)) * PAGESIZE;
    return (i % 2 == 0) ? addr + offset : addr - offset;
}

// TODO: this only works if pa is 512 aligned
block* blocktable::get_block(uintptr_t addr) {
    // TODO: is it possible for some page in that block to have different order? assert!
    return get_block(addr, pages[addr / PAGESIZE].order);
}

block* blocktable::get_block(uintptr_t addr, int order) {
    // TODO: is it possible for some page in that block to have different order? assert!
    uintptr_t i = block_number(order, addr);
    return &t_[order - MIN_ORDER][i];
}

// TODO: make this a method of block*
block* blocktable::get_parent(block* b) {
    block* buddy = get_block(b->buddy_addr_);
    int p_order = b->order_ + 1;
     // TODO: extract b->index_ % 2 == 0  into left() method
    uintptr_t p_index = b->index_ % 2 == 0 ?  
        block_number(p_order, b->first_) :
        block_number(p_order, buddy->first_);
    return &t_[p_order - MIN_ORDER ][p_index];
}


// lists of free blocks per order
// list<block, &block::link_> free_blocks[ORDER_COUNT];
// keep track of all physical memory
// page pages[PAGES_COUNT];

////////////////////////////////////////////////////////////////////////////



void merge(uintptr_t block_addr) {

    //get block
    block* blk = btable.get_block(block_addr);


    // check if block is completely free
    // TODO: add iteration support to block_pages (e.g., va, next, etc);
    for(uintptr_t addr = blk->first_; addr < blk->last_; addr+=PAGESIZE) {
        if(pages[addr / PAGESIZE].free == 0) {
            return;
        }
    }

    // get buddy
    block* buddy = btable.get_block(blk->buddy_addr_);

    //check if buddy and block have the same order
    if(buddy->order_ != blk->order_) {
        return;
    }

    // check if buddy is completely free and not reserved
    // TODO: can I improve this?
    for(uintptr_t addr = buddy->first_; addr < buddy->last_; addr+=PAGESIZE) {
        if(pages[addr / PAGESIZE].free == 0) {
            return;
        }
    }


    // create new block to be pushed
    block* parent_blk = btable.get_parent(blk);

    // remove block and buddy from free_blocks of order
    free_blocks[blk->order_ - MIN_ORDER].erase(blk);
    free_blocks[blk->order_ - MIN_ORDER].erase(buddy);

    // push new block to free_blocks entry of order + 1
    free_blocks[blk->order_ + 1 - MIN_ORDER].push_back(parent_blk);

    //update pages
    for(uintptr_t addr = blk->first_; addr < blk->last_; addr+=PAGESIZE) {
        pages[blk->first_ / PAGESIZE].order = blk->order_ + 1;
    }
    for(uintptr_t addr = buddy->first_; addr < buddy->last_; addr+=PAGESIZE) {
        pages[buddy->first_ / PAGESIZE].order = blk->order_ + 1;
    }

    merge(parent_blk->first_);   
}


// init_kalloc
//    Initialize stuff needed by `kalloc`. Called from `init_hardware`,
//    after `physical_ranges` is initialized.
void init_kalloc() {
    btable.init();

    //Initialize free list's first order column and pages
    // TODO: make this a method of pages or free_blocks
    auto irqs = page_lock.lock();
    for(uintptr_t pa = 0; pa < physical_ranges.limit(); pa += PAGESIZE) {
        if(physical_ranges.type(pa) == mem_available) {
            //add block to free_blocks[0]
            free_blocks[0].push_back(btable.get_block(pa));
            // mark block as free and with order 12
            pages[pa / PAGESIZE].free = true;
            pages[pa / PAGESIZE].order = MIN_ORDER;
        } 
    }
    page_lock.unlock(irqs);
    
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
    block* blk = free_blocks[order - MIN_ORDER].pop_front();
    if (blk) {
        //use this block
        ptr = pa2kptr<void*>(blk->first_);
    } else {
        // find free block with order o > order, minimizing o.
        for(int o = order + 1; o < MAX_ORDER; o++) {
            // if found a block
            if((blk = free_blocks[o - MIN_ORDER].pop_front())) {
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

            // fix order of blocks if necessary
            if (left_blk->index_ % 2 != 0) {  
                block* tmp_blk = left_blk;
                left_blk = right_blk;
                right_blk = tmp_blk;  
            }

            //update pages orders
            pages[left_blk->first_ / PAGESIZE].order = o - 1;
            pages[right_blk->first_ / PAGESIZE].order = o - 1;

            //update free_blocks
            free_blocks[o - MIN_ORDER - 1].push_back(right_blk);
            blk = left_blk;
        }
    
        //use this block
        ptr = pa2kptr<void*>(blk->first_);
     }

    // set block's free status to false
    for (uintptr_t addr = blk->first_; addr < blk->last_; addr += PAGESIZE) {
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
    block* test = btable.get_block(block_addr, order);
    block* blk = &btable.t_[order - MIN_ORDER][btable.block_number(order, block_addr)];
    assert(test->first_ == blk->first_);
    assert(test->last_ == blk->last_);

    // update pages, setting block's pages to free
    for(uintptr_t addr = blk->first_; addr < blk->last_; addr+=PAGESIZE){
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
