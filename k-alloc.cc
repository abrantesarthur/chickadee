#include "kernel.hh"
#include "k-vmiter.hh"
#include "k-lock.hh"
#include "k-pages.hh"

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
    spinlock_guard guard(page_lock);

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
                assert(pages.has_order(p, o));
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
            assert(pages.has_order(p, p->order));
            assert(pages.has_order(b, p->order));
            pages.freeblocks_push(b);
        }
    }

    // found block
    ptr = pa2kptr<void*>(p->first());

     // at this point, block should have the desired order
    assert(pages.has_order(p, order));

    // set block's status to allocated
    pages.allocate(p);

    // tell sanitizers the allocated page is accessible
    asan_mark_memory(ka2pa(ptr), p->size(), false);
    // initialize to `int3`
    memset(ptr, 0xCC, p->size());
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

// kfree_mem(p)
//      Free the user-accessible memory of process 'p'
void kfree_mem(proc* p) {
    // assumes that process 'p' is no longer in the ptable
    // to avoid synchronization conflicts with memviewer

    // free user-accessible memory
    for(vmiter it(p, 0); it.low(); it.next()) {
        if(it.user() && it.pa() != CONSOLE_ADDR) {
            it.kfree_page();
        }
    }
}

// kfree_pagetable
//      Free the 'pagetable'
void kfree_pagetable( x86_64_pagetable* pagetable) {
    // assumes that process 'p' is no longer in the ptable
    // to avoid synchronization conflicts with memviewer
    
    for(ptiter it(pagetable); it.low(); it.next()) {
        it.kfree_ptp();
    }
    kfree(reinterpret_cast<void*>(pagetable));
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