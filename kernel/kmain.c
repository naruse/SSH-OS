/* SSH OS kernel main program. */
#include "kernel.h"
#include "hw.h"

void trap_selfcheck(void);
void machine_report(void);
void show_font(void);

static void title_bar(void)
{
    int i;

    con_goto(0, 0);
    con_set_inverse(1);
    for (i = 0; i < CON_COLS; i++)
        con_putc(' ');
    con_goto(1, 0);
    kputs("SSH OS " SSHOS_VERSION);
    con_set_inverse(0);
    con_goto(0, 2);
    serial_puts("\n");                  /* the screen moved; start a new serial line too */
}

void machine_report(void)
{
    kprintf("Memory        %lu KB of RAM, video memory at %08lX\n",
            machine.mem_top / 1024, (u32)machine.screen);
    kprintf("Kernel        %08lX-%08lX, %lu bytes\n", (u32)__kernel_start,
            (u32)__kernel_end, (u32)(__kernel_end - __kernel_start));
    kprintf("Boot disk     drive %d, driver %d\n", machine.boot_drive, machine.boot_refnum);
    kprintf("ROM           version %04lX at %08lX\n", (u32)machine.rom_version,
            machine.rom_base);
    kprintf("ROM's heap    %08lX-%08lX, application heap from %08lX\n",
            machine.rom_sys_zone, machine.rom_sys_zone_end, machine.rom_appl_zone);

    /* Our hard-coded chip addresses should agree with what the ROM found. */
    kprintf("Serial chip   read %08lX, write %08lX  %s\n", SCC_RD_BASE, SCC_WR_BASE,
            (machine.rom_scc_rd == SCC_RD_BASE && machine.rom_scc_wr == SCC_WR_BASE)
                ? "(matches ROM)" : "(ROM DISAGREES)");
    kprintf("VIA chip      %08lX  %s\n", VIA_BASE,
            machine.rom_via == VIA_BASE ? "(matches ROM)" : "(ROM DISAGREES)");
    kprintf("VIA set-up    port B direction %02lX, ACR %02lX, PCR %02lX, enables %02lX\n",
            (u32)VIA(VIA_DDRB), (u32)VIA(VIA_ACR), (u32)VIA(VIA_PCR),
            (u32)VIA(VIA_IER) & VIA_INT_ALL);
}

void show_font(void)
{
    int c;

    /* Codes 128-159 are line drawing and symbols, 160-255 are Latin-1 (see
     * tools/model/charset.py). The samples use octal escapes for them. */
    kputs("  ");
    for (c = 32; c < 256; c++) {
        if (c == 127)
            continue;
        kputc((char)c);
        if (c == 79 || c == 126 || c == 175 || c == 223)
            kputs("\n  ");
    }
    kputs("\n\n  The quick brown fox jumps over the lazy dog. 0123456789\n"
          "  El ping\374ino \321and\372 pregunt\363: \277d\363nde est\341 el ni\361o? \241Aqu\355!\n\n"
          "  \213\200\200\200\200\200\200\200\200\210\200\200\200\200\200\200\214\n"
          "  \201 a\361o    \201 \217\226\225\224 \201\n"
          "  \206\200\200\200\200\200\200\200\200\212\200\200\200\200\200\200\207\n"
          "  \201 \227 \230 \231 \232\201 \237 \235 \233\201\n"
          "  \215\200\200\200\200\200\200\200\200\211\200\200\200\200\200\200\216\n");
}

/* Does this start go to the home screen (rather than the OSConsole or a
 * program named in the settings)? */
static int starts_at_home(void)
{
    const char *start = config_get("start");

    return !start || !start[0] || strcmp(start, "desk") == 0;
}

void kmain(void)
{
    machine_init();
    config_init();
    rng_init();
    con_init(machine.screen);
    gfx_init(machine.screen);
    serial_init(57600);
    trap_init();

    /* From here to the end of start-up the screen is the start-up picture;
     * what is printed goes to the serial port. */
    splash_start();
    kputs("Running on a Macintosh SE with no Mac OS. The ROM is no longer in use.\n\n");
    trap_selfcheck();
    via_init();
    cpu_irq_enable();
    timer_calibrate();
    splash_step(&icon_boot_chip);

    mouse_from_settings();
    adb_start_polling();
    mouse.x = SCREEN_W / 2;
    mouse.y = SCREEN_H / 2 + 40;
    splash_step(&icon_boot_keys);

    scsi_init();
    splash_step(&icon_boot_disk);

    snd_init();
    splash_step(&icon_boot_sound);

    machine_report();
    if (starts_at_home()) {
        desk_network_start();   /* the SSH OS disk with the settings, then the network */
        splash_step(&icon_boot_net);
    }
    splash_end();

    gfx_pointer(1);             /* the pointer is there in every program */
    if (!starts_at_home()) {
        con_clear();
        bar_draw();
        title_bar();
        kputs("Ready. Type help for a list of commands.\n");
    }
    serial_puts("SSHOS-BOOT-OK\n");     /* marker for the automated test */

#ifdef TEST_CRASH
    /* Deliberate address error, to exercise the crash screen. It has to be
     * assembly: given an odd constant address in C, the compiler reads it one
     * byte at a time and nothing faults. */
    __asm__ volatile("tst.w 0x100001");
#endif

    shell_run();
}
