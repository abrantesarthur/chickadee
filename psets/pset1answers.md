CS 161 Problem Set 1 Answers
============================
Leave your name out of this file. Put collaboration notes and credit in
`pset1collab.md`.

Answers to written questions
----------------------------

##### A. Memory Allocator
1. 4096 bytes
2. The first address returned by `kalloc` is byte 4096. The reason is that the first `mem_available` range is set by `init_physical_ranges` to start at byte 4096. Hence, as `kalloc` iterates looking for the first `mem_available` range, it returns 4096 as a first result.
3. The last address returned by `kalloc` is byte 2093056 (i.e., one `PAGE_SIZE` below 2MB).
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

Grading notes
-------------
