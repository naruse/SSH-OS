/* Debug output on the modem port (SCC channel A), polled, no interrupts.
 *
 * 8 data bits, no parity, 1 stop bit. The baud-rate generator runs from the
 * chip's own 3.672 MHz clock (PCLK), so the rate does not depend on how the
 * ROM left the external clock select.
 *
 * Every register access is followed by a short pause: the 8530 needs recovery
 * time between accesses, and a real SE is less forgiving than an emulator.
 */
#include "kernel.h"
#include "hw.h"

#define RR0_TX_EMPTY    0x04

static int serial_alive;

static void scc_pause(void)
{
    volatile int i;

    for (i = 0; i < 2; i++)
        ;
}

static u8 scc_read_ctl(void)
{
    u8 v = PEEK8(SCC_RD_BASE + SCC_A_CTL);

    scc_pause();
    return v;
}

/* Write a value to write-register `reg` of channel A. */
static void scc_write_reg(u8 reg, u8 value)
{
    POKE8(SCC_WR_BASE + SCC_A_CTL, reg);
    scc_pause();
    POKE8(SCC_WR_BASE + SCC_A_CTL, value);
    scc_pause();
}

void serial_init(u32 baud)
{
    /* Time constant for a x16 clock: PCLK / (2 * 16 * baud) - 2, rounded. */
    u32 tc = (SCC_PCLK_HZ + 16 * baud) / (32 * baud) - 2;

    (void)scc_read_ctl();               /* make sure the next write selects a register */
    scc_write_reg(9, 0x80);             /* reset channel A */
    scc_pause();
    scc_pause();
    scc_write_reg(9, 0x00);             /* chip-wide interrupt enable off */
    scc_write_reg(4, 0x44);             /* x16 clock, 1 stop bit, no parity */
    scc_write_reg(3, 0xC0);             /* receive 8 bits, receiver still off */
    scc_write_reg(5, 0x62);             /* transmit 8 bits, RTS, transmitter still off */
    scc_write_reg(11, 0x50);            /* receive and transmit clock = baud generator */
    scc_write_reg(12, (u8)tc);
    scc_write_reg(13, (u8)(tc >> 8));
    scc_write_reg(14, 0x02);            /* generator source = PCLK, stopped */
    scc_write_reg(14, 0x03);            /* start the generator */
    scc_write_reg(15, 0x00);            /* no status interrupts */
    scc_write_reg(1, 0x00);             /* no interrupts at all */
    scc_write_reg(3, 0xC1);             /* receiver on */
    scc_write_reg(5, 0xEA);             /* DTR, transmit 8 bits, transmitter on, RTS */
    serial_alive = 1;
}

void serial_puts(const char *s)
{
    while (*s) {
        if (*s == '\n')
            serial_putc('\r');
        serial_putc(*s++);
    }
}

/* Send one byte. If the chip never reports room, give up on the port for
 * good so a missing or broken serial chip cannot stall the kernel. */
void serial_putc(char c)
{
    u32 tries = 50000;

    if (!serial_alive)
        return;
    while (!(scc_read_ctl() & RR0_TX_EMPTY)) {
        if (--tries == 0) {
            serial_alive = 0;
            return;
        }
    }
    POKE8(SCC_WR_BASE + SCC_A_DATA, (u8)c);
    scc_pause();
}
