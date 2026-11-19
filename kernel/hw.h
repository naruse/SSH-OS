/* Macintosh SE hardware addresses and the few ROM values we borrow.
 *
 * Sources: Apple "Guide to the Macintosh Family Hardware" 2nd ed. and
 * Inside Macintosh. Byte-wide devices sit on one half of the data bus, so
 * they must only ever be accessed with byte reads and writes.
 */
#ifndef SSHOS_HW_H
#define SSHOS_HW_H

#include "types.h"

/* Video: 512 x 342, 1 bit per pixel, 1 = black, bit 7 is the left pixel. */
#define SCREEN_W        512
#define SCREEN_H        342
#define SCREEN_ROWBYTES 64

/* Zilog 8530 serial chip. Reads use even addresses, writes odd ones. */
#define SCC_RD_BASE     0x9FFFF8UL
#define SCC_WR_BASE     0xBFFFF9UL
#define SCC_B_CTL       0       /* printer port */
#define SCC_A_CTL       2       /* modem port */
#define SCC_B_DATA      4
#define SCC_A_DATA      6
#define SCC_PCLK_HZ     3672000UL

/* 6522 VIA: register n is at base + n * 0x200. */
#define VIA_BASE        0xEFE1FEUL
#define VIA(reg)        (*(volatile u8 *)(VIA_BASE + (reg) * 0x200UL))
#define VIA_ORB         0       /* port B data */
#define VIA_DDRB        2       /* port B direction, 1 = output */
#define VIA_T2CL        8       /* timer 2, low and high byte: counts down all the time */
#define VIA_T2CH        9
#define VIA_SR          10      /* shift register: ADB data byte */
#define VIA_ORA         15      /* port A data: bits 0-2 are the sound volume */
#define PB_SOUND_OFF    0x80    /* port B, out: 1 = sound off */
#define VIA_ACR         11      /* auxiliary control */
#define VIA_PCR         12      /* peripheral control */
#define VIA_IFR         13      /* interrupt flags */
#define VIA_IER         14      /* interrupt enables */

/* Port B bits on the SE. */
#define PB_ADB_INT      0x08    /* in: 0 = ADB transceiver flag (meaning depends on phase) */
#define PB_ADB_ST0      0x10    /* out: ADB transaction state, low bit */
#define PB_ADB_ST1      0x20    /* out: ADB transaction state, high bit */
#define PB_SCSI_MASK    0x40    /* out: 1 = SCSI interrupt masked */

/* Interrupt flag / enable bits. */
#define VIA_INT_VBL     0x02    /* CA1: vertical blanking, 60.15 Hz */
#define VIA_INT_SR      0x04    /* shift register: ADB byte done */
#define VIA_INT_ALL     0x7F
#define VIA_IER_SET     0x80    /* written with bits to enable; without it, bits are disabled */

/* Auxiliary control: shift register mode. */
#define ACR_SR_MASK     0x1C
#define ACR_SR_EXT_IN   0x0C    /* shift in, clocked by the ADB transceiver */
#define ACR_SR_OUT      0x10    /* set = shift out */

/* NCR 5380 SCSI chip. Register n is read at base + n * 0x10 and written at
 * that address + 1; the two sets of registers are different (see scsi.c). */
#define SCSI_BASE       0x580000UL

/* ROM low-memory globals. The ROM filled these in before it loaded us; we
 * read them once at start-up and never call the ROM. */
#define LM_MemTop       0x108   /* long: end of RAM */
#define LM_VIA          0x1D4   /* long: VIA base */
#define LM_SCCRd        0x1D8   /* long: SCC read base */
#define LM_SCCWr        0x1DC   /* long: SCC write base */
#define LM_Ticks        0x16A   /* long: the ROM's tick count */
#define LM_SoundBase    0x266   /* long: the sound buffer */
#define LM_Time         0x20C   /* long: clock time in seconds, read from the clock chip at start */
#define LM_BootDrive    0x210   /* word: drive number we booted from */
#define LM_SysZone      0x2A6   /* long: start of the ROM's system heap */
#define LM_ApplZone     0x2AA   /* long: start of the ROM's application heap */
#define LM_ROMBase      0x2AE   /* long: ROM base address */
#define LM_ScrnBase     0x824   /* long: video memory */
#define LM_BtDskRfn     0xB34   /* word: boot disk driver reference number */

#define PEEK8(addr)     (*(volatile u8 *)(addr))
#define PEEK16(addr)    (*(volatile u16 *)(addr))
#define PEEK32(addr)    (*(volatile u32 *)(addr))
#define POKE8(addr, v)  (*(volatile u8 *)(addr) = (v))

#endif
