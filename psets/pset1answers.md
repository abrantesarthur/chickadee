# CS 161 Problem Set 1 Answers

Leave your name out of this file. Put collaboration notes and credit in
`pset1collab.md`.

## Answers to written questions

##### A. Memory Allocator

1. 4096 bytes (i.e., one PAGESIZE)
2. The first address returned by `kalloc` is virtual address 0xFFFF'8000'0000'1000. This is a high-canonical address which corresponds to physical address 0x1000. The reason why `kalloc` skips the range 0x0000 to 0x1000 is that `init_physical_ranges` sets it as `mem_reserved`. Hence, as `kalloc` iterates looking for the first `mem_available` range, it returns the virtual address corresponding to 0x1000 as a first result.
3. The last address returned by `kalloc` is 0xFFFF'8000'001F'F000.
4.
5.
6.
7.
8.

##### B. Memory Viewer

1.
2.
3.
4.
5.
6.
7.

## Grading notes
