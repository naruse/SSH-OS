/* VIA interrupts: the 60.15 Hz clock tick and the keyboard/mouse bus.
 *
 * The VIA is the only source we enable on interrupt level 1. On the SE the
 * SCSI chip can also pull level 1; it stays masked (port B bit 6) until there
 * is a SCSI driver to answer it.
 */
#include "kernel.h"
#include "hw.h"

volatile u32 ticks;
volatile u32 irq_stray;

void via_init(void)
{
    VIA(VIA_IER) = VIA_INT_ALL;                 /* disable every VIA interrupt */
    VIA(VIA_IFR) = VIA_INT_ALL;                 /* and forget any that were pending */

    VIA(VIA_DDRB) |= PB_SCSI_MASK;
    VIA(VIA_ORB) |= PB_SCSI_MASK;               /* SCSI interrupt masked */

    adb_init();

    VIA(VIA_IER) = VIA_IER_SET | VIA_INT_VBL | VIA_INT_SR;
}

/* Called from irq_level1 in start.S with interrupts masked. */
void via_interrupt(void)
{
    u8 pending = VIA(VIA_IFR) & VIA(VIA_IER) & VIA_INT_ALL;

    if (!pending) {
        irq_stray++;
        return;
    }
    if (pending & VIA_INT_VBL) {
        VIA(VIA_IFR) = VIA_INT_VBL;
        ticks++;
        gfx_pointer_tick();
        snd_tick();
        if (!(pending & VIA_INT_SR))
            adb_watch();
    }
    if (pending & VIA_INT_SR)
        adb_interrupt();                        /* reading the shift register clears the flag */

    /* Anything else was not asked for: switch it off so it cannot repeat. */
    pending &= (u8)~(VIA_INT_VBL | VIA_INT_SR);
    if (pending) {
        VIA(VIA_IER) = pending;
        VIA(VIA_IFR) = pending;
    }
}
