# CS 161 Problem Set 5 Answers

Leave your name out of this file. Put collaboration notes and credit in
`pset5collab.md`.

## Answers to written questions

## Questions

- 1. How many threads should we support?

## To do

#### Clone

- Before syscall

  1. save args in callee-saved registers and push them to the user-level stack of the calling thread.
  2. place the SYSCALL_TEXIT number in %rax, so when 'syscall' is executed, it jumps to the right place

- After syscall: set the stack frame

  1. push %rsp (this saves the original %rsp that is eventually reestored by sys_texit)
  2. push %rbp (this saves the original %rbp that is eventually reestored by sys_texit)
  3. set %rbp and %rsp to the stack_top argument
  4. store the first six arguments in the corresponding registers
  5. push the excess arguments in the stack so that 7th arg is at the top (note that that the top of the s)
  6. push the excess of six arguments in increasing order, so that 7th argument is at the top of the stack when we execute `callq` (note that when that happens, %rsp must be 16-byte aligned. H)
  7. should we save caller saved registers?
  8. push address of sys_texit, then jmp to function passed by

#### Questions

1. How do we make sure that the stack-frame is 16-byte aligned, as required by x86-64? That is, how do we make sure that, by the time we push the address of sys_texit to the stack, %rsp must be 16-byte aligned. Before pushing that address, One option is See https://edstem.org/us/courses/19658/discussion/1088363
2. How do we know how many arguments there are?

## Grading notes
