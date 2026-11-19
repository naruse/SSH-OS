/* Random numbers, for SSH's one-time keys and padding.
 *
 * The SE has nothing that produces randomness, so it is gathered: the
 * exact moment (to about a microsecond, from a free-running counter in the
 * VIA) of every key press, mouse movement and arriving network packet is
 * stirred into a pool, on top of the clock time the ROM read at start-up
 * and the settings, which hold secrets of this installation. Numbers are
 * drawn from the pool through SHA-256 and ChaCha20.
 *
 * This is weaker than a modern computer's source, most of all right after
 * start-up and in an emulator driven by a script, where the moments repeat.
 * The BlueSCSI's processor has a real source; it should take this over.
 */
#include "kernel.h"
#include "hw.h"

extern const char config_text[];
extern const u32 config_text_len;

static u32 pool[8];
static u8 where;
static u32 drawn;

/* Stir in a value and the moment it happened. Cheap enough for an
 * interrupt handler; a lost update when two calls collide does no harm. */
void rng_stir(u32 value)
{
    u32 moment = ((u32)VIA(VIA_T2CH) << 8) | VIA(VIA_T2CL);
    u8 i = where++ & 7;
    u32 x = pool[i];

    x = ((x << 5) | (x >> 27)) + value + (moment << 16) + ticks;
    pool[i] = x ^ moment;
    pool[(i + 3) & 7] += (x << 11) | (x >> 21);
}

void rng_init(void)
{
    struct sha256 s;
    u8 digest[32];
    int i;

    sha256_init(&s);
    sha256_update(&s, config_text, config_text_len);
    sha256_finish(&s, digest);
    for (i = 0; i < 8; i++)
        pool[i] ^= ((u32)digest[4 * i] << 24) | ((u32)digest[4 * i + 1] << 16) |
                   ((u32)digest[4 * i + 2] << 8) | digest[4 * i + 3];
    rng_stir(machine.rom_time);
    rng_stir(machine.rom_ticks);
}

void rng_bytes(u8 *out, u32 n)
{
    static const u8 nonce[12];
    struct sha256 s;
    u8 key[32], next[32];
    int i;

    rng_stir(++drawn);
    sha256_init(&s);
    sha256_update(&s, pool, sizeof pool);
    sha256_finish(&s, key);
    memset(out, 0, n);
    chacha20(key, nonce, 1, out, n);

    /* Move the pool on, so that what was just handed out cannot be worked
     * out again from the pool as it is afterwards. */
    memset(next, 0, sizeof next);
    chacha20(key, nonce, 0, next, sizeof next);
    for (i = 0; i < 8; i++)
        pool[i] = ((u32)next[4 * i] << 24) | ((u32)next[4 * i + 1] << 16) |
                  ((u32)next[4 * i + 2] << 8) | next[4 * i + 3];
    memset(key, 0, sizeof key);
    memset(next, 0, sizeof next);
}
