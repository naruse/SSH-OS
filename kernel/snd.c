/* Sound: a beep, and a tune.
 *
 * The SE plays, 60 times a second, a buffer of 370 samples that sits in
 * memory just above the screen (the ROM's SoundBase says where). Each
 * sample is the high byte of a word; the low bytes belong to the floppy
 * drive's speed control and are left alone. A buffer that holds a steady
 * wave and is never changed therefore gives a steady tone, which is all a
 * beep needs: ten periods of 37 samples fit the buffer exactly, about
 * 600 Hz. Volume is three bits of VIA port A; bit 7 of port B switches
 * the sound off (1) and on (0).
 *
 * A tune needs other notes than that one, and few of them fit the buffer a
 * whole number of times. So while a tune plays, the buffer is written
 * afresh at every tick, each time carrying the wave on from where the last
 * one stopped: the tick comes as the picture's blanking starts, which is
 * just before the SE begins the buffer again.
 */
#include "kernel.h"
#include "hw.h"

#define SAMPLES 370
#define PERIOD  37

static u8 volume = 5;
static volatile u32 off_at;     /* tick at which the beep ends; 0 = silent */
static const struct snd_note *tune;     /* the note of a tune being played; NULL = no tune */

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
    tune = NULL;
    fill(1);
    off_at = ticks + nticks;
    VIA(VIA_ORB) &= (u8)~PB_SOUND_OFF;
}

/* ------------------------------------------------------------------ tunes -- */

static u16 tune_left;           /* notes after it */
static u8 note_left;            /* ticks of it still to play */
static u16 phase;               /* where in its wave the last buffer stopped */

/* One buffer of a square wave that moves on by `step` a sample: a whole
 * wave is 65536, and the SE plays 370 * 60.15 samples a second. */
static void fill_note(u16 step)
{
    volatile u8 *p = (volatile u8 *)machine.sound_base;
    u16 ph = phase;
    int i;

    for (i = 0; i < SAMPLES; i++, p += 2) {
        *p = (ph & 0x8000) ? 0xE0 : 0x20;
        ph = (u16)(ph + step);
    }
    phase = ph;
}

/* Play these notes, one after another, and return at once. A note with
 * step 0 is a rest. */
void snd_tune(const struct snd_note *notes, int count)
{
    if (!volume || !machine.sound_base || count <= 0)
        return;
    cpu_irq_disable();
    off_at = 0;
    tune = notes;
    tune_left = (u16)(count - 1);
    note_left = notes[0].ticks;
    phase = 0;
    cpu_irq_enable();
    VIA(VIA_ORB) &= (u8)~PB_SOUND_OFF;
}

int snd_playing(void)
{
    return tune != NULL;
}

void snd_stop(void)
{
    cpu_irq_disable();
    tune = NULL;
    quiet();
    cpu_irq_enable();
}

/* From the 60 Hz interrupt: the next stretch of a tune, or the end of a
 * beep when its time is up. */
void snd_tick(void)
{
    if (tune) {
        while (note_left == 0) {
            if (tune_left == 0) {
                tune = NULL;
                quiet();
                return;
            }
            tune++;
            tune_left--;
            note_left = tune->ticks;
        }
        /* the last tick of a note is silent, so that two of the same pitch
         * are heard as two */
        if (tune->step && note_left > 1)
            fill_note(tune->step);
        else
            fill(0);
        note_left--;
        return;
    }
    if (off_at && (s32)(ticks - off_at) >= 0)
        quiet();
}
