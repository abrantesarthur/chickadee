#ifndef CHICKADEE_PAGES_HH
#define CHICKADEE_PAGES_HH

static spinlock page_lock;  // protect pages object

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
    inline page* get_page(uintptr_t addr);     // returns page at addr
    inline page* get_block(uintptr_t addr);    // gets first page of the block
    inline page* get_buddy(page* p);   // get first page in buddy block
    inline page* get_parent(page* p);
    void increment_order(page*p);
    void increment_order_by(page*p, int v);
    void decrement_order(page*p);
    void set_status(page* p, pagestatus_t s);     // update block's status
    inline void free(page* p);     // free the block
    inline void allocate(page* p);     // allocate the block
    bool is_free(page* b);  // returns true if all pages within block are free
    bool has_order(page* b, int o);  // returns true if all pages within block have order o
    uint32_t index(uintptr_t addr);  // get the index of page at address addr

    inline void freeblocks_push(page* p);
    inline page* freeblocks_pop(int o);
    inline void freeblocks_erase(page* p, int o);

        // helper functions
    void print_block(page* p);
    void print_pageset();
    void print_freeblocks(unsigned count);



    private:
        page ps_[PAGES_COUNT];
        list<page, &page::link_> fbs_[ORDER_COUNT];
};

static pageset pages;

inline void pageset::freeblocks_push(page* p) {
    fbs_[p->order - MIN_ORDER].push_back(p);
}

inline page* pageset::freeblocks_pop(int o) {
    return fbs_[o - MIN_ORDER].pop_front();
}

inline void pageset::freeblocks_erase(page* p, int o) {
    fbs_[o - MIN_ORDER].erase(p);
}

inline void pageset::free(page* p) {
    set_status(p, pg_free);
}

inline void pageset::allocate(page* p) {
    set_status(p, pg_allocated);
}

inline page* pageset::get_buddy(page* p) {
    return &ps_[index(p->buddy())];
}

inline page* pageset::get_parent(page* p) {
    return &ps_[index(p->parent())];
}

inline page* pageset::get_page(uintptr_t addr) {
    return &ps_[index(addr)];
}

inline page* pageset::get_block(uintptr_t addr) {
    return &ps_[index(get_page(addr)->first())];
}

#endif