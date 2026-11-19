| SSH OS kernel entry point and exception stubs.
|
| The boot block jumps to _start (the first byte of the kernel image) in
| supervisor mode with interrupts off.

        .equ    ScrnBase, 0x824         | ROM global: address of video memory

        .section .text.start, "ax"
        .globl  _start
_start:
        move.w  #0x2700,sr              | supervisor mode, all interrupts masked
        movea.l ScrnBase.w,sp           | stack grows down from just below the screen

        lea     __bss_start,a0          | zero the uninitialised data
        lea     __bss_end,a1
1:      cmpa.l  a1,a0
        bcc.s   2f
        clr.w   (a0)+
        bra.s   1b
2:
        jsr     kmain
        | kmain should never return; fall through and stop if it does.

        .globl  cpu_halt
cpu_halt:
        stop    #0x2700
        bra.s   cpu_halt

        .text
        .globl  cpu_irq_enable, cpu_irq_disable, cpu_idle
cpu_irq_enable:
        move.w  #0x2000,sr
        rts
cpu_irq_disable:
        move.w  #0x2700,sr
        rts
| cpu_irq_save: mask interrupts and return what the status register was;
| cpu_irq_restore puts that back. For code that may run before interrupts
| are let in at all, and must not let them in itself.
        .globl  cpu_irq_save, cpu_irq_restore
cpu_irq_save:
        moveq   #0,d0
        move.w  sr,d0
        move.w  #0x2700,sr
        rts
cpu_irq_restore:
        move.w  6(sp),sr
        rts
| Sleep until an interrupt arrives, then return (the handler runs first).
cpu_idle:
        stop    #0x2000
        rts

| Level 1 interrupt: the VIA (clock tick, keyboard and mouse bus).
| C code may change d0-d1/a0-a1, so those are what we save.
        .globl  irq_level1
irq_level1:
        movem.l d0-d1/a0-a1,-(sp)
        jsr     via_interrupt
        movem.l (sp)+,d0-d1/a0-a1
        rte

| ---------------------------------------------------------------------------
| Exception stubs. One per vector, each exactly TRAP_STUB_SIZE bytes, so C can
| compute the address of stub n. A stub saves every register, loads its vector
| number and joins trap_common, which calls trap_panic(vector, saved_regs).
|
| Stack on entry to trap_panic (lowest address first):
|   d0-d7, a0-a7 (16 longs), then the CPU's own exception frame.

        .text
        .equ    TRAP_FIRST, 2           | bus error
        .equ    TRAP_LAST, 47           | TRAP #15

        .globl  trap_stubs
trap_stubs:
        .set    vec, TRAP_FIRST
        .rept   TRAP_LAST - TRAP_FIRST + 1
        movem.l d0-d7/a0-a7,-(sp)       | 4 bytes
        moveq   #vec,d0                 | 2 bytes
        bra.w   trap_common             | 4 bytes
        .set    vec, vec + 1
        .endr
        .globl  trap_stubs_end
trap_stubs_end:

trap_common:
        move.w  #0x2700,sr
        move.l  sp,-(sp)                | argument 2: pointer to the saved registers
        move.l  d0,-(sp)                | argument 1: vector number
        jsr     trap_panic
        jmp     cpu_halt
