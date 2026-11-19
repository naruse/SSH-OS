/* Hardware test: one screen that shows what the machine has and lets each
 * part be tried. Keyboard and mouse are shown live; the slower tests run
 * when their letter is pressed. Esc goes back.
 */
#include "kernel.h"
#include "hw.h"
#include "net.h"
#include "shell.h"

#define ROW_MEMORY      4
#define ROW_SCSI        6
#define ROW_NETWORK     11
#define ROW_KEYBOARD    14
#define ROW_MOUSE       15
#define ROW_SCREEN      17
#define ROW_SOUND       18
#define ROW_RESULT      21

/* Print one line of the screen, clearing what was there. */
static void at(int row, const char *text)
{
    con_goto(0, row);
    con_fill(0, row, CON_COLS, ' ', 0);
    kputs(text);
}

static void result(const char *text)
{
    at(ROW_RESULT, "  ");
    kputs(text);
    serial_puts("\n");
}

static void show_scsi(void)
{
    static const char *const types[] = {"disk", "tape", "printer", "processor", "WORM",
                                        "CD-ROM", "scanner", "optical"};
    u8 inq[36], id, type;
    char name[25];
    int row = ROW_SCSI, found = 0, i;

    at(row, "  SCSI       looking...");
    for (id = 0; id < 7 && row < ROW_NETWORK - 1; id++) {
        if (scsi_inquiry(id, inq) != 0)
            continue;
        type = inq[0] & 0x1F;
        for (i = 0; i < 24; i++)
            name[i] = (inq[8 + i] >= 32 && inq[8 + i] < 127) ? (char)inq[8 + i] : ' ';
        name[24] = 0;
        at(row, found ? "             " : "  SCSI       ");
        kprintf("ID %d  %s  %s", id, type == 3 && id == eth_adapter_id() ? "network" :
                type < 8 ? types[type] : "other", name);
        row++;
        found++;
    }
    if (!found)
        at(ROW_SCSI, "  SCSI       nothing answered on the bus");

}

static void show_network(void)
{
    at(ROW_NETWORK, "  Network    ");
    if (net.up && net.ip) {
        kputs("address ");
        shell_print_ip(net.ip);
        kputs(", gateway ");
        shell_print_ip(net.gateway);
        at(ROW_NETWORK + 1, "             P = ping the gateway");
    } else {
        kputs("not started (no adapter, or no address)");
    }
}

/* Write a different value into every free long word of memory, then read
 * them all back. Free memory is what lies between the kernel and the stack,
 * which sits just under the screen. */
static void test_memory(void)
{
    u32 *first = (u32 *)(((u32)__bss_end + 0x1003) & ~3UL);
    u32 *last = (u32 *)(((u32)machine.screen - 0x10000) & ~3UL);
    u32 *p, bad = 0, n = 0;

    result("Testing memory, about 15 seconds...");
    for (p = first; p < last; p++)
        *p = (u32)p ^ 0x5AA5C33CUL;
    for (p = first; p < last; p++, n++)
        if (*p != ((u32)p ^ 0x5AA5C33CUL))
            bad++;
    for (p = first; p < last; p++)
        *p = ~((u32)p ^ 0x5AA5C33CUL);
    for (p = first; p < last; p++)
        if (*p != ~((u32)p ^ 0x5AA5C33CUL))
            bad++;
    at(ROW_RESULT, "  ");
    if (bad)
        kprintf("MEMORY FAULTY: %lu of %lu K read back wrong.\n", bad / 256, n / 256);
    else
        kprintf("Memory good: %lu K written and read back twice.\n", n / 256);
}

static void wait_key(void)
{
    int ev;

    for (;;) {
        ev = kbd_get_event();
        if (ev >= 0 && !(ev & 0x8000) && (ev & 0xFF))
            return;
        cpu_idle();
    }
}

static void test_screen(void)
{
    static const int patterns[] = {GFX_BLACK, GFX_WHITE, GFX_GRAY, GFX_LIGHT, GFX_DARK};
    int i, x;

    for (i = 0; i < 5; i++) {
        gfx_fill(0, 0, SCREEN_W, SCREEN_H, patterns[i]);
        wait_key();
    }
    gfx_fill(0, 0, SCREEN_W, SCREEN_H, GFX_WHITE);      /* a grid, to judge the geometry */
    (void)0;
    for (x = 0; x < SCREEN_W; x += 32)
        gfx_fill(x, 0, 1, SCREEN_H, GFX_BLACK);
    for (x = 0; x < SCREEN_H; x += 32)
        gfx_fill(0, x, SCREEN_W, 1, GFX_BLACK);
    gfx_frame(0, 0, SCREEN_W, SCREEN_H, 1);
    wait_key();
    bar_draw();                 /* the patterns covered the whole screen, bar included */
}

static void draw_all(void)
{
    con_clear();
    at(1, "  Hardware test                                                        Esc = back");
    at(ROW_MEMORY, "  Memory     ");
    kprintf("%lu K                    M = test it", machine.mem_top / 1024);
    show_network();
    at(ROW_KEYBOARD, "  Keyboard   press any key");
    at(ROW_MOUSE, "  Mouse      move it, press the button");
    at(ROW_SCREEN, "  Screen     S = test patterns (any key for the next)");
    at(ROW_SOUND, "  Sound      B = beep, + and - = volume (now ");
    kprintf("%d of 7)", snd_volume());
    show_scsi();                /* last: asking every address on the bus takes a few seconds */
    serial_puts("\nHWTEST: ready\n");
}

void cmd_hwtest(void)
{
    u32 seen = mouse.moves - 1, keys = 0, rtt;
    int ev, err, r;
    u8 ch, raw;

    draw_all();
    for (;;) {
        net_poll();
        if (seen != mouse.moves) {
            seen = mouse.moves;
            con_goto(13, ROW_MOUSE);
            kprintf("x %3d  y %3d  button %s  clicks %lu     ", mouse.x, mouse.y,
                    mouse.button ? "down" : "up  ", mouse.clicks);
        }
        ev = kbd_get_event();
        bar_tick();
        r = bar_poll();
        if (r == BAR_HOME || r == BAR_CONSOLE)
            break;              /* the SSH OS menu: leave */
        if (ev < 0) {
            bar_idle();
            continue;
        }
        bar_work();
        if (ev & 0x8000)
            continue;
        ch = (u8)(ev & 0xFF);
        raw = (u8)((ev >> 8) & 0x7F);
        if (ch == 0x1B)
            break;
        keys++;
        con_goto(13, ROW_KEYBOARD);
        kprintf("key code %02lX", (u32)raw);
        if (ch >= 32 && ch != 127)
            kprintf(" '%c'", ch);
        kprintf(", %lu pressed so far        ", keys);
        if (ch == 'b' || ch == 'B') {
            snd_beep(20);
            result("Beep. Did you hear it?");
        } else if (ch == '+' || ch == '=' || ch == '-') {
            snd_set_volume(snd_volume() + (ch == '-' ? -1 : 1));
            snd_keep_volume();
            at(ROW_SOUND, "  Sound      B = beep, + and - = volume (now ");
            kprintf("%d of 7)", snd_volume());
            snd_beep(8);
            bar_draw();
        } else if (ch == 'm' || ch == 'M') {
            test_memory();
        } else if (ch == 's' || ch == 'S') {
            test_screen();
            draw_all();
        } else if ((ch == 'p' || ch == 'P') && net.up && net.ip) {
            result("Pinging the gateway...");
            err = net_ping(net.gateway, 1, 3 * 60, &rtt);
            at(ROW_RESULT, "  ");
            if (err)
                kprintf("No answer from the gateway: %s.\n", net_strerror(err));
            else
                kprintf("The gateway answered in %lu ms.\n", rtt * 1000 / 60);
        }
    }
    kbd_escape = 0;
}
