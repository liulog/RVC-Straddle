.section .text.entry
.globl _start
.option push
.option norelax
_start:
    csrw sie, zero
    csrci sstatus, 2
    la gp, __global_pointer$
    la sp, stack_top
    la t0, trap_entry
    csrw stvec, t0
    la t0, __bss_start
    la t1, __bss_end
1:
    bgeu t0, t1, 2f
    sd zero, 0(t0)
    addi t0, t0, 8
    j 1b
2:
    call experiment_main
3:
    wfi
    j 3b
.option pop

.balign 4
trap_entry:
    csrr a0, scause
    csrr a1, sepc
    csrr a2, stval
    tail experiment_trap

/* Preserve s0 because the independent code-page entry uses c.addi s0,-31. */
.globl call_code_page
call_code_page:
    addi sp, sp, -16
    sd ra, 0(sp)
    sd s0, 8(sp)
    jalr ra, a1, 0
    ld s0, 8(sp)
    ld ra, 0(sp)
    addi sp, sp, 16
    ret

/* Set known inputs for the overlapping streams and report the resulting a6.
 * a0=input, a1=entry, a2=&a6_after. Both streams preserve the stack pointer.
 */
.globl call_middle_entry
call_middle_entry:
    addi sp, sp, -16
    sd ra, 0(sp)
    sd a2, 8(sp)
    li t1, 0
    li a6, 64
    jalr ra, a1, 0
    ld t0, 8(sp)
    sd a6, 0(t0)
    ld ra, 0(sp)
    addi sp, sp, 16
    ret

.balign 4
.globl alternate_trap_entry
alternate_trap_entry:
    j trap_entry

.section .bss.stack,"aw",@nobits
.balign 16
stack_bottom:
    .skip 16384
stack_top:
