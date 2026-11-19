/* Waiting and timeouts.
 *
 * Time is counted in ticks of the 60.15 Hz interrupt (16.6 ms each). A
 * waiting loop must also end when interrupts are off and the tick stands
 * still, so every timeout carries a second limit: a number of loop turns,
 * worked out from how many turns fit in one tick on this machine.
 */
#include "kernel.h"

/* Turns of a timeout_expired() loop per tick. Until timer_calibrate() has
 * measured it, assume a machine several times faster than the SE, so that
 * the fallback waits too long rather than too short. */
u32 spins_per_tick = 4000;

#define MAX_TICKS 36000         /* ten minutes; keeps the arithmetic in 32 bits */

void timeout_start(struct timeout *t, u32 nticks)
{
    if (nticks > MAX_TICKS)
        nticks = MAX_TICKS;
    t->start = ticks;
    t->limit = nticks + 1;      /* the current tick is already partly gone */
    /* Twice the measured count: with the tick running, the tick ends the
     * wait; the loop count is only there for when it is not. */
    t->spins = t->limit * spins_per_tick * 2;
}

int timeout_expired(struct timeout *t)
{
    if ((u32)(ticks - t->start) >= t->limit)
        return 1;
    if (t->spins == 0)
        return 1;
    t->spins--;
    return 0;
}

void delay_ticks(u32 nticks)
{
    struct timeout t;

    timeout_start(&t, nticks);
    while (!timeout_expired(&t))
        ;
}

/* Count how many loop turns fit between two ticks. */
void timer_calibrate(void)
{
    struct timeout t;
    u32 t0, n = 0;

    timeout_start(&t, 4);
    t0 = ticks;
    while (ticks == t0)                 /* wait for the start of a tick */
        if (timeout_expired(&t))
            return;                     /* the clock is not running: keep the default */

    timeout_start(&t, 4);
    t0 = ticks;
    while (ticks == t0 && !timeout_expired(&t))
        n++;
    if (ticks != t0 && n > 0)
        spins_per_tick = n;
}
