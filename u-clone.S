#include "obj/u-asm.h"

/* The userland implementation is:
   pid_t sys_clone(int (*function)(void*), void* arg, char* stack_top).
   the kernel entry is:
   pid_t syscall_clone ()

   The parameters are passed in register from userland:
   rdi: function
   rsi: arg
   rdx:	stack_top

   The kernel expects:
   rax: system call number
*/

.text

// sys_clone(function, arg, stack_top)
.globl _Z9sys_clonePFiPvES_Pc
_Z9sys_clonePFiPvES_Pc:
    // save callee-saved registers before modifying them
    pushq %r12
    pushq %r13
    pushq %r14

    // save relevant caller-saved registers
    mov %rdi, %r12
    mov %rsi, %r13
    mov %rdx, %r14

    // call syscall_clone
    mov $SYSCALL_CLONE, %eax
    syscall

    // decide whether to return to new thread or to original thread
    testq %rax, %rax
    jz .Lthread_start

.Lpseudo_end:
    // restore modified registers
    popq %r14
    popq %r13
    popq %r12
    ret

.Lthread_start:
    // set the new stack
    mov %r14, %rbp
    mov %r14, %rsp
    // call the function with arg
    mov %r13, %rdi
    call *%r12

    // call sys_texit with the function's return value
    mov %rax, %rdi
    mov $SYSCALL_TEXIT, %rax
    syscall