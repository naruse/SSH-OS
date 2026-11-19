/* CPU exceptions: any exception the kernel does not expect ends in a crash
 * screen with enough detail to find the bug, then the machine stops. */
#include "kernel.h"

#define TRAP_FIRST      2
#define TRAP_LAST       47
#define TRAP_STUB_SIZE  10      /* must match the stubs in start.S */

extern char trap_stubs[], trap_stubs_end[];

static const char *vector_name(unsigned vec)
{
    static const char *const names[] = {
        [2] = "Bus error",
        [3] = "Address error",
        [4] = "Illegal instruction",
        [5] = "Divide by zero",
        [6] = "CHK out of bounds",
        [7] = "TRAPV overflow",
        [8] = "Privilege violation",
        [9] = "Trace",
        [10] = "Unimplemented instruction (A-line)",
        [11] = "Unimplemented instruction (F-line)",
        [15] = "Uninitialised interrupt",
        [24] = "Spurious interrupt",
    };

    if (vec >= 25 && vec <= 31)
        return "Unexpected interrupt";
    if (vec >= 32 && vec <= 47)
        return "Unexpected TRAP instruction";
    if (vec < sizeof names / sizeof names[0] && names[vec])
        return names[vec];
    return "Unknown exception";
}

static u16 get16(const u8 *p)
{
    return (u16)((p[0] << 8) | p[1]);
}

static u32 get32(const u8 *p)
{
    return ((u32)get16(p) << 16) | get16(p + 2);
}

/* Called from trap_common in start.S. `regs` points at d0-d7, a0-a7 as they
 * were when the exception hit; the CPU's exception frame follows them. */
void trap_panic(unsigned vec, u32 *regs)
{
    const u8 *frame = (const u8 *)(regs + 16);
    u16 sr;
    u32 pc;
    int i;

    kputc_divert(NULL);                 /* whatever hid the text: this must show */
    con_clear();
    con_set_inverse(1);
    kprintf(" SSH OS stopped: %s (vector %u) ", vector_name(vec), vec);
    con_set_inverse(0);
    kputs("\n\n");

    if (vec == 2 || vec == 3) {
        /* 68000 bus/address error frame: status word, access address,
         * instruction register, then SR and PC. The PC is only approximate. */
        u16 status = get16(frame);

        kprintf("  Access address  %08lX  (%s, %s)\n", get32(frame + 2),
                (status & 0x10) ? "read" : "write",
                (status & 0x08) ? "while handling another exception"
                                : "while running an instruction");
        kprintf("  Instruction     %04lX\n", (u32)get16(frame + 6));
        frame += 8;
    }
    sr = get16(frame);
    pc = get32(frame + 2);
    kprintf("  PC  %08lX%s\n", pc, (vec == 2 || vec == 3) ? "  (approximate)" : "");
    kprintf("  SR  %04lX\n\n", (u32)sr);

    for (i = 0; i < 8; i++)
        kprintf("  D%d  %08lX      A%d  %08lX\n", i, regs[i], i,
                i == 7 ? (u32)(frame + 6) : regs[8 + i]);

    kprintf("\n  Kernel is at %08lX-%08lX.\n", (u32)__kernel_start, (u32)__kernel_end);
    kputs("  The machine is halted. Restart it to continue.\n");
    serial_puts("SSHOS-PANIC\n");      /* marker for the automated test */
    cpu_halt();
}

/* Point every exception vector we handle at its stub. Vectors live at address
 * vector * 4; the 68000 has no way to move the table. */
void trap_init(void)
{
    volatile u32 *vectors = (volatile u32 *)0;
    unsigned vec;

    for (vec = TRAP_FIRST; vec <= TRAP_LAST; vec++)
        vectors[vec] = (u32)trap_stubs + (vec - TRAP_FIRST) * TRAP_STUB_SIZE;

    /* Vector 25 is interrupt level 1, the VIA. Everything else stays on a
     * panic stub: an interrupt we did not ask for is a bug worth seeing. */
    vectors[25] = (u32)irq_level1;
}

/* Build-time check that the stub size above matches start.S. */
void trap_selfcheck(void)
{
    u32 expect = (TRAP_LAST - TRAP_FIRST + 1) * TRAP_STUB_SIZE;

    if ((u32)(trap_stubs_end - trap_stubs) != expect)
        kprintf("WARNING: trap stub table is %lu bytes, expected %lu\n",
                (u32)(trap_stubs_end - trap_stubs), expect);
}
