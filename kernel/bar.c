/* The bar across the top of the screen, there in every program: the SSH OS
 * menu title, memory in use, how busy the processor is, the network and the
 * clock. The text screen starts below it (console.c).
 *
 * Nothing here runs from an interrupt. Each program's main loop calls
 * bar_tick() as it goes round, which redraws the figures once a second, and
 * bar_idle() when it has found nothing to do, which sleeps until the next
 * interrupt. The load figure is the share of time not spent in such idle
 * turns. (A turn that found nothing to do counts as idle from its start, not
 * only while asleep: the Snow emulator does not sleep at all there.)
 */
#include "kernel.h"
#include "hw.h"
#include "net.h"

#define COUNTS_PER_TICK 13023UL     /* of the VIA's timer 2, which runs at 783,360 a second */

static u8 title_open, net_open;     /* drawn black on white while their menu or panel is open */
static u32 idle_time, idle_then;    /* in timer counts */
static u32 drawn_at;                /* tick of the last redraw */
static u16 mark;                    /* timer value at the end of the last idle call */
static u8 worked;

/* Timer 2 counts down all the time and wraps every 84 thousandths of a second. */
static u16 timer_now(void)
{
    u8 high, low;

    do {
        high = VIA(VIA_T2CH);
        low = VIA(VIA_T2CL);
    } while (high != VIA(VIA_T2CH));
    return (u16)((high << 8) | low);
}

void bar_work(void)
{
    worked = 1;
}

void bar_idle(void)
{
    u16 before = timer_now(), after;

    gfx_pointer_unpark();       /* nothing is being drawn: the pointer may show */
    cpu_idle();
    after = timer_now();
    /* Idle since the last call if nothing was done in between; otherwise
     * only the sleep itself. Either is under the timer's wrap. */
    idle_time += (u16)((worked ? before : mark) - after);
    mark = after;
    worked = 0;
}

static void put_num(char *out, u32 v, int digits)
{
    while (digits--) {
        out[digits] = (char)('0' + v % 10);
        v /= 10;
    }
}

void ip_text(char *out, u32 ip)
{
    int i, n = 0;
    u32 part;

    for (i = 3; i >= 0; i--) {
        part = (ip >> (8 * i)) & 0xFF;
        if (part >= 100)
            out[n++] = (char)('0' + part / 100);
        if (part >= 10)
            out[n++] = (char)('0' + part / 10 % 10);
        out[n++] = (char)('0' + part % 10);
        if (i)
            out[n++] = '.';
    }
    out[n] = 0;
}

/* A gauge in white on the black bar: an outline with part of it filled. */
static void gauge(int x, u32 part, u32 whole)
{
    u32 w = whole ? part * 46 / whole : 0;

    gfx_fill(x, 6, 50, 8, GFX_WHITE);
    gfx_fill(x + 1, 7, 48, 6, GFX_BLACK);
    gfx_fill(x + 2, 8, (int)(w > 46 ? 46 : w), 4, GFX_WHITE);
}

static void text(int x, const char *s)
{
    gfx_text(x, 5, s, 1, 1, 0);
}

/* A small outlined button in the bar, holding a minus or a plus. */
static void button(int x, int plus)
{
    gfx_fill(x, 3, BAR_VOL_BTN, BAR_VOL_BTN, GFX_WHITE);
    gfx_fill(x + 1, 4, BAR_VOL_BTN - 2, BAR_VOL_BTN - 2, GFX_BLACK);
    gfx_fill(x + 3, 9, 7, 1, GFX_WHITE);
    if (plus)
        gfx_fill(x + 6, 6, 1, 7, GFX_WHITE);
}

static void draw_figures(u32 load)
{
    static const struct icon *const fans[4] = {&icon_wifi1, &icon_wifi2, &icon_wifi3, &icon_wifi4};
    u32 used = (u32)(__bss_end - __kernel_start) / 1024, total = machine.mem_top / 1024;
    u32 secs = machine.rom_time + ticks * 100 / 6015;
    char s[20];
    int up = net.up && net.ip;

    gfx_begin();
    gfx_fill(BAR_TITLE_W, 0, BAR_NET_X - BAR_TITLE_W, BAR_H, GFX_BLACK);
    text(70, "Mem");
    gauge(92, used, total);
    put_num(s, used, 3);
    memcpy(s + 3, "K", 2);
    text(146, s[0] == '0' ? s + 1 : s);
    text(196, "CPU");
    gauge(218, load, 100);
    put_num(s, load, 3);
    memcpy(s + 3, "%", 2);
    text(272, s + (load < 10 ? 2 : load < 100 ? 1 : 0));

    memcpy(s, "Vol ", 4);
    s[4] = (char)('0' + snd_volume());
    s[5] = 0;
    text(BAR_VOL_X, s);
    button(BAR_VOL_LESS, 0);
    button(BAR_VOL_MORE, 1);

    /* The network: its name and signal if the adapter tells, else our address. */
    gfx_fill(BAR_NET_X, 0, BAR_CLOCK_X - BAR_NET_X - 4, BAR_H, net_open ? GFX_WHITE : GFX_BLACK);
    gfx_icon(BAR_NET_X + 4, 4, !up ? &icon_wifi0 : !wifi_now.name[0] ? &icon_wifi4 :
             fans[wifi_bars(wifi_now.signal) - 1], !net_open);
    if (up && wifi_now.name[0]) {
        memcpy(s, wifi_now.name, 10);
        s[10] = 0;
    } else if (up) {
        ip_text(s, net.ip);
        s[10] = 0;              /* what fits; the panel has all of it */
    } else {
        memcpy(s, "offline", 8);
    }
    gfx_text(BAR_NET_X + 26, 5, s, !net_open, 1, 0);

    gfx_fill(BAR_CLOCK_X - 4, 0, SCREEN_W - BAR_CLOCK_X + 4, BAR_H, GFX_BLACK);
    put_num(s, secs % 86400 / 3600, 2);
    s[2] = ':';
    put_num(s + 3, secs % 3600 / 60, 2);
    s[5] = 0;
    text(BAR_CLOCK_X, s);
    gfx_end();
}

static u32 last_load;

struct wifi_network wifi_now;

void wifi_refresh(void)
{
    if (!(net.up && net.ip) || wifi_current(&wifi_now) != 0)
        wifi_now.name[0] = 0;
}

void bar_draw(void)
{
    gfx_begin();
    gfx_fill(0, 0, BAR_TITLE_W, BAR_H, title_open ? GFX_WHITE : GFX_BLACK);
    gfx_text(8, 5, "SSH OS", !title_open, 1, 1);
    gfx_end();
    draw_figures(last_load);
}

void bar_highlight(int title, int network)
{
    title_open = (u8)title;
    net_open = (u8)network;
    bar_draw();
}

void bar_tick(void)
{
    u32 span = ticks - drawn_at, idle;

    if (span < 60)
        return;
    idle = (idle_time - idle_then) / (COUNTS_PER_TICK / 100);      /* hundredths of a tick */
    last_load = idle >= span * 100 ? 0 : (span * 100 - idle) / span;
    drawn_at = ticks;
    idle_then = idle_time;
    draw_figures(last_load);
    worked = 1;                 /* drawing it was work */
}
