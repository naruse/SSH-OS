/* SSH OS kernel: shared declarations. */
#ifndef SSHOS_KERNEL_H
#define SSHOS_KERNEL_H

#include "types.h"

#define SSHOS_VERSION "0.1"

/* Values captured from the ROM's globals at start-up (machine.c). */
struct machine {
    u32 mem_top;        /* end of RAM = RAM size in bytes */
    u8 *screen;         /* video memory */
    u32 rom_base;
    u16 rom_version;
    u32 rom_via, rom_scc_rd, rom_scc_wr;
    u32 rom_sys_zone, rom_sys_zone_end, rom_appl_zone;
    s16 boot_drive, boot_refnum;
    u32 sound_base;             /* the buffer of sound samples */
    u32 rom_time, rom_ticks;    /* the ROM's clock and tick count when we took over */
};
extern struct machine machine;
void machine_init(void);

/* Linker script symbols. */
extern char __kernel_start[], __kernel_end[], __bss_start[], __bss_end[];

/* start.S */
void cpu_halt(void) __attribute__((noreturn));
void cpu_irq_enable(void);      /* let interrupts in */
void cpu_irq_disable(void);
void cpu_idle(void);            /* sleep until the next interrupt */
extern char irq_level1[];       /* VIA interrupt entry */

/* lib.c */
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);

/* config.c: settings, from the built-in config.txt and the SSH OS disk */
void config_init(void);
int  config_load(void);         /* read SETTINGS.TXT from the SSH OS disk */
int  config_store(const char *name, const char *value);   /* config_set, and saved on the disk */
const char *config_get(const char *name);       /* NULL if not set */
int  config_set(const char *name, const char *value);
const char *config_name(int i);                 /* for listing; NULL for an empty slot */
int  config_host_port(const char *name, char *host_out, int host_max, u16 *port,
                      u16 default_port);

/* sha256.c */
struct sha256 {
    u32 h[8];
    u32 total;
    u8  buf[64];
    u8  used;
};
void sha256_init(struct sha256 *s);
void sha256_update(struct sha256 *s, const void *data, u32 len);
void sha256_finish(struct sha256 *s, u8 *out32);
void hmac_sha256(const void *key, u32 key_len, const void *msg, u32 msg_len, u8 *out32);

/* x25519.c: key agreement and a stream cipher */
void x25519(u8 *out32, const u8 *scalar32, const u8 *point32);
void chacha20(const u8 *key32, const u8 *nonce12, u32 counter, u8 *data, u32 len);
/* If set, called now and then during the long calculations (x25519 and
 * the ed25519 functions), to keep the network answered and show progress. */
extern void (*crypto_idle)(void);

/* sha512.c */
struct sha512 {
    u32 h[8][2];                /* each word as high half, low half */
    u32 total;
    u8  buf[128];
    u8  used;
};
void sha512_init(struct sha512 *s);
void sha512_update(struct sha512 *s, const void *data, u32 len);
void sha512_finish(struct sha512 *s, u8 *out64);

/* ed25519.c: signatures. A key pair is a 32-byte seed (secret) and the
 * 32-byte public key worked out from it. */
void ed25519_public(u8 *pub32, const u8 *seed32);
void ed25519_sign(u8 *sig64, const u8 *msg, u32 len, const u8 *seed32, const u8 *pub32);
int  ed25519_verify(const u8 *sig64, const u8 *msg, u32 len, const u8 *pub32);    /* 1 = genuine */

/* rng.c: random numbers gathered from the timing of keys, mouse and network */
void rng_init(void);
void rng_stir(u32 value);       /* something just happened; may be called from an interrupt */
void rng_bytes(u8 *out, u32 n);

/* The Unicode characters of codes 0x80-0x9F (generated with the font). */
extern const unsigned short charset_extra[32];
/* Our character for each of U+2500-U+259F: box drawing of every weight. */
extern const unsigned char charset_box[160];

/* poly1305.c: the check sum of SSH's chacha20-poly1305 packets */
void poly1305(u8 *tag16, const u8 *key32, const u8 *msg, u32 len);

/* snd.c: a beep */
void snd_init(void);
void snd_from_settings(void);   /* take the volume from the setting "volume" */
void snd_keep_volume(void);     /* save it as that setting */
void snd_beep(u32 nticks);      /* returns at once */
int  snd_volume(void);          /* 0 (off) to 7 */
void snd_set_volume(int v);
void snd_tick(void);            /* from the 60 Hz interrupt */

/* wifi.c: the BlueSCSI's WiFi commands; each returns -1 if the adapter has none */
#define WIFI_NAME_MAX       32
#define WIFI_MAX_NETWORKS   10
struct wifi_network {
    char name[WIFI_NAME_MAX + 1];
    s8 signal;                  /* dBm: -50 is strong, -85 weak */
    u8 channel, secured;
};
int wifi_current(struct wifi_network *n);
int wifi_scan_start(void);
int wifi_scan_done(void);
int wifi_scan_results(struct wifi_network *list, int max);
int wifi_join(const char *name, const char *password);
int wifi_bars(int signal);      /* 1 to 4 */
int  wifi_restore(void);        /* back to the network in the settings; 1 if it moved there */
extern struct wifi_network wifi_now;    /* the network we are on; name empty if unknown (bar.c) */
void wifi_refresh(void);                /* ask the adapter again */

/* bar.c: the bar at the top of the screen, in every program */
#define BAR_VOL_LESS 300        /* the volume: a - button, */
#define BAR_VOL_X    318        /* "Vol 5", */
#define BAR_VOL_MORE 352        /* and a + button */
#define BAR_VOL_BTN  13         /* each is this wide and high */
#define BAR_MEM_X    66         /* "Mem", its gauge and figure: a click shows what uses it */
#define BAR_MEM_END  188
#define BAR_H        20
#define BAR_TITLE_W  58         /* the SSH OS menu title, at its left */
#define BAR_NET_X    380        /* the network icon and address */
#define BAR_CLOCK_X  474
void bar_draw(void);
void bar_tick(void);            /* call from every main loop: redraws the figures once a second */
void bar_idle(void);            /* nothing to do this turn: sleep until the next interrupt */
void bar_work(void);            /* this turn did something (for the load figure) */
void bar_highlight(int title, int network);     /* show the menu title or network item as open */
void ip_text(char *out, u32 ip);                /* a.b.c.d */
/* barmenu.c: what a click on the bar does. bar_poll() from every main loop. */
#define BAR_NONE    0           /* the bar was not clicked */
#define BAR_USED    1           /* something was opened and is closed again */
#define BAR_HOME    2           /* the menu's Home: go to the home screen */
#define BAR_CONSOLE 3           /* the menu's OSConsole: go to the command line */
int  bar_poll(void);
void bar_memory(void);          /* the panel of what uses the memory */
struct mem_part { const char *name; unsigned long bytes; };
extern const struct mem_part mem_parts[];   /* largest first (tools/mkmem.py) */
extern const int mem_nparts;
int  bar_menu(void);            /* open the SSH OS menu; returns as bar_poll */
void bar_panel(void);           /* open the network panel */
/* ask.c: SSH's questions, each in a small window. */
int  ask_password(const char *user, const char *host, char *line, int max);  /* length, or -1 */
int  ask_trust(const char *host, const char *print);                         /* 1 = trust it */
void ask_message(const char *title, const char *const *lines, int n);         /* with an OK button */
int  connect_run(char *target);     /* connect.c: SSH from the home screen; 1 = never got in */
extern void (*ssh_login_hook)(void);            /* ssh.c: called once logged in, */
extern void (*ssh_end_hook)(void);              /* and when the session is over */
extern u8 ssh_ended_well;                       /* the last session ended with nothing to report */
void ask_button(int x, int y, const char *s, int main_one);
int  kbd_line_key(char *line, int *len, int max, u8 c);     /* ssh.c: 1 added, -1 removed, 0 */
void bar_volume_step(void);     /* one step louder each time, then off */
void bar_volume_change(int by); /* louder or quieter, stopping at 0 and 7 */

/* console.c: 85 x 32 character text screen, below the bar */
#define CON_COLS 85
#define CON_ROWS 32
void con_init(u8 *screen);
void con_clear(void);
void con_putc(char c);
void con_set_inverse(int on);
void con_goto(int col, int row);
int  con_row(void);
void con_cursor_toggle(void);
/* Drawing at a given place without moving the text cursor (for programs
 * that manage the whole screen themselves). */
#define CON_INVERSE     1
#define CON_BOLD        2
#define CON_UNDERLINE   4
void con_draw(int col, int row, const u8 *text, int n, u8 attr);
void con_fill(int col, int row, int n, u8 ch, u8 attr);
void con_scroll(int top, int bottom, int lines);    /* rows top..bottom; negative = down */
void con_invert(int col, int row);                  /* flip one cell, e.g. for a cursor */

/* The terminal that SSH sessions draw on (vt.c). */
void vt_reset(void);
void vt_reset_modes(void);
void vt_write(const u8 *p, u32 n);
void vt_cursor(int on);
int  vt_app_keys(void);
int  vt_reply(u8 *out, int max);
void vt_end(void);
extern u8 vt_log;

/* gfx.c: drawing for programs with their own screen, and the mouse pointer */
#define GFX_WHITE   0
#define GFX_BLACK   1
#define GFX_GRAY    2           /* half the pixels black */
#define GFX_LIGHT   3           /* an eighth */
#define GFX_DARK    4           /* seven eighths */
#define GFX_INVERT  5
void gfx_init(u8 *screen);
void gfx_fill(int x, int y, int w, int h, int pattern);
void gfx_frame(int x, int y, int w, int h, int thickness);
void gfx_round_box(int x, int y, int w, int h, int pattern, int thickness);
void gfx_text(int x, int y, const char *s, int white, int scale, int bold);
int  gfx_text_width(const char *s, int scale);
void gfx_picture(int x, int y, const u16 *rows, int h, int white, int scale);
struct icon {
    unsigned char w, h;
    const unsigned char *bits;  /* one bit per pixel, rows padded to whole bytes */
};
void gfx_icon(int x, int y, const struct icon *icon, int white);
extern const struct icon icon_ssh, icon_hardware, icon_settings, icon_logo, icon_logo48;
#define SSHOS_AUTHOR "Juan Munoz Arango"
#define SSHOS_SOURCE "github.com/naruse/SSH-OS"
extern const struct icon icon_boot_chip, icon_boot_keys, icon_boot_disk, icon_boot_sound, icon_boot_net;

/* splash.c: the start-up screen. */
void splash_start(void);
void splash_step(const struct icon *icon);   /* one more part is ready */
void splash_end(void);
void desk_network_start(void);      /* desk.c: the SSH OS disk and the network, once */
extern const struct icon icon_wifi0, icon_wifi1, icon_wifi2, icon_wifi3, icon_wifi4;
void gfx_pointer(int on);       /* show or remove the mouse pointer */
void gfx_begin(void);           /* bracket all drawing while the pointer is on */
void gfx_end(void);
void gfx_pointer_tick(void);    /* from the 60 Hz interrupt */
void gfx_pointer_park(void);    /* off the screen until gfx_pointer_unpark; cheap to repeat */
void gfx_pointer_unpark(void);
void gfx_save(int x, int y, int w, int h);      /* keep that part of the screen... */
void gfx_restore(void);                         /* ...and put it back */

/* http.c */
int http_request(const char *method, u32 ip, u16 port, const char *host, const char *path,
                 const char *token, const char *body, char *out, u32 out_max, u32 *out_len,
                 int *status);

/* serial.c: modem port, polled */
void serial_init(u32 baud);
void serial_putc(char c);
void serial_puts(const char *s);  /* modem port only, not the screen */

/* kprintf.c: prints to the screen and the modem port */
void kputc(char c);
void kputs(const char *s);
void kputc_divert(void (*to)(char));
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* trap.c */
void trap_init(void);

/* via.c: interrupts and the 60.15 Hz clock tick */
extern volatile u32 ticks;
extern volatile u32 irq_stray;  /* level-1 interrupts with no VIA cause */
void via_init(void);
void via_interrupt(void);

/* timer.c: waiting and timeouts, counted in 60.15 Hz ticks. They also work
 * with interrupts off, when the tick stands still: then they fall back to
 * counting loop turns, calibrated against the tick at start-up. */
struct timeout {
    u32 start, limit, spins;
};
void timer_calibrate(void);     /* call once, with interrupts enabled */
void timeout_start(struct timeout *t, u32 nticks);
int  timeout_expired(struct timeout *t);    /* call on every turn of a waiting loop */
void delay_ticks(u32 nticks);
extern u32 spins_per_tick;

/* scsi.c: polled driver for the SCSI chip. The Mac is always device 7. */
#define SCSI_DATA_NONE  0
#define SCSI_DATA_IN    1       /* device to Mac */
#define SCSI_DATA_OUT   2       /* Mac to device */

#define SCSI_ERR_NO_DEVICE  (-1)    /* nothing answered at that ID */
#define SCSI_ERR_BUS_BUSY   (-2)    /* could not get the bus */
#define SCSI_ERR_TIMEOUT    (-3)    /* device stopped answering mid-command */
#define SCSI_ERR_BUS_FREE   (-4)    /* device left the bus mid-command */
#define SCSI_ERR_PHASE      (-5)    /* device asked for something unexpected */
#define SCSI_ERR_CHECK      (-6)    /* device reported an error; see scsi_sense */
#define SCSI_ERR_STATUS     (-7)    /* device returned some other bad status */

struct scsi_sense {
    u8 key, asc, ascq;          /* why the last command failed (SCSI_ERR_CHECK) */
};
extern struct scsi_sense scsi_sense;
extern u32 scsi_bus_resets;
extern u8 scsi_fast_reads;      /* 1 = let the chip do the handshake on reads */

void scsi_init(void);
void scsi_reset_bus(void);
/* One raw command. Returns the device's status byte (0 = good) or a negative
 * SCSI_ERR_ value. *moved, if not NULL, gets the number of data bytes moved. */
int scsi_command(u8 id, const u8 *cdb, u8 cdb_len, u8 dir, void *buf, u32 len, u32 *moved);
/* The same with error handling: 0 on success or a negative SCSI_ERR_ value. */
int scsi_exec(u8 id, const u8 *cdb, u8 cdb_len, u8 dir, void *buf, u32 len, u32 *moved);
int scsi_inquiry(u8 id, u8 *buf36);
int scsi_capacity(u8 id, u32 *blocks, u32 *block_size);
int scsi_read(u8 id, u32 block, u16 count, void *buf);          /* 512-byte blocks */
int scsi_write(u8 id, u32 block, u16 count, const void *buf);
const char *scsi_strerror(int err);

/* fs.c: the SSH OS disk, a FAT16 volume named SSH OS on a SCSI disk. Files
 * are in one folder, have short names (SETTINGS.TXT) and are read and
 * written whole. Each function returns 0 or a negative FS_ERR_ value. */
#define FS_ERR_NO_DISK      (-1)
#define FS_ERR_DISK         (-2)
#define FS_ERR_NOT_FOUND    (-3)
#define FS_ERR_FULL         (-4)
#define FS_ERR_NAME         (-5)
#define FS_ERR_DAMAGED      (-6)
#define FS_ERR_TOO_BIG      (-7)
#define FS_ERR_IN_USE       (-8)
#define FS_ERR_TOO_SMALL    (-9)
#define FS_BLANK    1           /* what fs_look() found: nothing at all, */
#define FS_SCRATCH  2           /* an SSH OS scratch disk (tools/mkscratch.py), */
#define FS_OURS   3           /* an SSH OS volume */
int  fs_mount(void);            /* look for the disk; call again after fs_look or fs_format */
int  fs_mounted(void);
int  fs_disk_id(void);          /* its SCSI ID, or -1 */
int  fs_read(const char *name, void *buf, u32 max, u32 *len);
int  fs_write(const char *name, const void *buf, u32 len);      /* replaces the file */
int  fs_delete(const char *name);
int  fs_list(int index, char *name13, u32 *size);       /* FS_ERR_NOT_FOUND after the last */
int  fs_space(u32 *total_kb, u32 *free_kb);
int  fs_look(u8 id);            /* FS_BLANK and so on, or FS_ERR_IN_USE: not ours to touch */
int  fs_format(u8 id);          /* only where fs_look() allows */
const char *fs_strerror(int err);

/* saved.c: what is remembered on the SSH OS disk besides the settings */
void saved_start(void);         /* find the disk and read everything; only the first call does anything */
int  hosts_known(const char *host, const char *print);  /* 1 yes, 0 never seen, -1 another key on record */
int  hosts_add(const char *host, const char *print);    /* 0 if it reached the disk */
#define RECENT_MAX  8
int  recent_count(void);
const char *recent_get(int i);          /* "user@host" or "user@host:port", newest first */
void recent_add(const char *target);
void recent_remove(int i);

/* dayna.c: the BlueSCSI's network adapter. The network stack itself is
 * declared in net.h. */
int  eth_init(void);            /* find the adapter and start it; 0 or NET_ERR_ */
void eth_shutdown(void);
int  eth_adapter_id(void);      /* its SCSI ID, or -1 */
extern u32 eth_poll_errors;

/* adb.c: keyboard and mouse bus */
void adb_init(void);
void adb_start_polling(void);
void adb_interrupt(void);
extern volatile u32 adb_packets, adb_timeouts;

/* kbd.c: key events. An event is (raw << 8) | character: raw is the ADB key
 * code with bit 7 set on release; character is 0 if the key has none, and
 * otherwise an SSH OS character code (accented letters are above 127).
 * KBD_EV_META is added when Option was held and did not change the
 * character. */
#define KBD_EV_META 0x10000UL
void kbd_key(u8 raw);
extern u8 kbd_option_meta;      /* 1 = Option never types accents, it is always Meta */
int  kbd_get_event(void);       /* -1 if none waiting */
extern volatile u8 kbd_escape;  /* set when Esc is pressed; clear it once acted on */
#define KBD_SHIFT   1
#define KBD_CONTROL 2
#define KBD_OPTION  4
#define KBD_COMMAND 8
u8   kbd_modifiers(void);       /* which of those keys are held right now */

/* mouse position and button, updated from interrupts */
struct mouse_state {
    volatile s16 x, y;
    volatile u8 button;
    volatile u32 moves;
    volatile u32 clicks;        /* counts presses, so a quick one is not missed */
    volatile s16 click_x, click_y;      /* where the last press happened */
};
extern struct mouse_state mouse;
void mouse_packet(u8 b0, u8 b1);
int  mouse_speed(void);         /* 1 to 7; 2 is one pixel for each step of the mouse */
void mouse_set_speed(int v);
void mouse_from_settings(void); /* take it from the setting "mouse" */
void mouse_keep_speed(void);    /* keep it on the SSH OS disk */
int  settings_run(void);        /* settings.c: the Settings window; returns as bar_poll */

/* shell.c */
void shell_run(void) __attribute__((noreturn));

#endif
