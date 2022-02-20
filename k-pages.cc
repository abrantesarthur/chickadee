#include "kernel.hh"
#include "k-pages.hh"

uint32_t pageset::index(uintptr_t addr) {
    assert(addr < physical_ranges.limit());
    assert(addr % PAGESIZE == 0);
    return addr / PAGESIZE;
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

void pageset::init() {
    for(uintptr_t pa = 0; pa < physical_ranges.limit(); pa += PAGESIZE) {
        if(physical_ranges.type(pa) == mem_available) {
            ps_[index(pa)].status = pg_free;
            ps_[index(pa)].addr = pa;
            freeblocks_push(&ps_[index(pa)]);
        }
    }
}

void pageset::try_merge_all() {
    for(int i = 0; i < PAGES_COUNT; i++) { 
        try_merge(&ps_[i]);
    }
}

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