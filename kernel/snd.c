/* Sound: a beep.
 *
 * The SE plays, 60 times a second, a buffer of 370 samples that sits in
 * memory just above the screen (the ROM's SoundBase says where). Each
 * sample is the high byte of a word; the low bytes belong to the floppy
 * drive's speed control and are left alone. A buffer that holds a steady
 * wave and is never changed therefore gives a steady tone, which is all a
 * beep needs: ten periods of 37 samples fit the buffer exactly, about
 * 600 Hz. Volume is three bits of VIA port A; bit 7 of port B switches
 * the sound off (1) and on (0).
 */
#include "kernel.h"
#include "hw.h"

#define SAMPLES 370
#define PERIOD  37

static u8 volume = 5;
static volatile u32 off_at;     /* tick at which the beep ends; 0 = silent */

static void fill(int wave)
{
    volatile u8 *p = (volatile u8 *)machine.sound_base;
    int i;

    for (i = 0; i < SAMPLES; i++)
        p[2 * i] = wave ? ((i % PERIOD) < PERIOD / 2 ? 0xE0 : 0x20) : 0x80;
}

static void quiet(void)
{
    VIA(VIA_ORB) |= PB_SOUND_OFF;
    fill(0);
    off_at = 0;
}

void snd_from_settings(void)
{
    const char *v = config_get("volume");

    if (v && v[0] >= '0' && v[0] <= '7' && !v[1])
        volume = (u8)(v[0] - '0');
    snd_set_volume(volume);
}

void snd_keep_volume(void)
{
    char v[2] = {(char)('0' + volume), 0};

    config_store("volume", v);
}

void snd_init(void)
{
    VIA(VIA_DDRB) |= PB_SOUND_OFF;
    quiet();
    snd_from_settings();
}

int snd_volume(void)
{
    return volume;
}

void snd_set_volume(int v)
{
    volume = (u8)(v < 0 ? 0 : v > 7 ? 7 : v);
    VIA(VIA_ORA) = (u8)((VIA(VIA_ORA) & ~7) | volume);
}

/* Start a beep of that many ticks and return at once. */
void snd_beep(u32 nticks)
{
    if (!volume || !machine.sound_base)
        return;
    fill(1);
    off_at = ticks + nticks;
    VIA(VIA_ORB) &= (u8)~PB_SOUND_OFF;
}

/* From the 60 Hz interrupt: end the beep when its time is up. */
void snd_tick(void)
{
    if (off_at && (s32)(ticks - off_at) >= 0)
        quiet();
}
