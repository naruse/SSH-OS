| Copying memory, as fast as a 68000 can: twelve registers, 48 bytes, at a
| time. Scrolling the terminal is one copy of most of the screen, some
| 20 KB, for every line; that is where this earns its keep.
|
|   void *memcpy(void *dst, const void *src, size_t n);
|   void *memmove(void *dst, const void *src, size_t n);
|
| memmove copies backwards when the destination overlaps the source from
| above. Where one address is odd and the other even nothing but a byte at
| a time will do (the 68000 faults on a word at an odd address).

        .text
        .globl  memcpy
        .globl  memmove

memmove:
        move.l  4(sp),a1                | to
        move.l  8(sp),a0                | from
        move.l  12(sp),d0
        cmp.l   a0,a1
        bls     forward                 | to <= from
        move.l  a0,d1
        add.l   d0,d1
        cmp.l   d1,a1
        bcs     backward                | to < from + n: they overlap
        bra     forward

memcpy:
        move.l  4(sp),a1
        move.l  8(sp),a0
        move.l  12(sp),d0

forward:
        tst.l   d0
        beq     done
        move.w  a0,d1
        add.w   a1,d1
        btst    #0,d1
        bne     fwd_bytes               | one odd, one even
        move.w  a0,d1
        btst    #0,d1
        beq     1f
        move.b  (a0)+,(a1)+             | both odd: one byte makes them even
        subq.l  #1,d0
1:      cmp.l   #96,d0
        bcs     fwd_longs
        movem.l d2-d7/a2-a6,-(sp)
2:      movem.l (a0)+,d1-d7/a2-a6
        movem.l d1-d7/a2-a6,(a1)
        lea     48(a1),a1
        sub.l   #48,d0
        cmp.l   #48,d0
        bcc     2b
        movem.l (sp)+,d2-d7/a2-a6
fwd_longs:
        move.l  d0,d1
        lsr.l   #2,d1                   | under 24 of them
        beq     fwd_tail
        subq.w  #1,d1
3:      move.l  (a0)+,(a1)+
        dbra    d1,3b
fwd_tail:
        and.l   #3,d0
        beq     done
fwd_bytes:
        move.b  (a0)+,(a1)+
        subq.l  #1,d0
        bne     fwd_bytes
        bra     done

backward:
        add.l   d0,a0
        add.l   d0,a1
        move.w  a0,d1
        add.w   a1,d1
        btst    #0,d1
        bne     back_bytes
        move.w  a0,d1
        btst    #0,d1
        beq     1f
        move.b  -(a0),-(a1)
        subq.l  #1,d0
1:      cmp.l   #96,d0
        bcs     back_longs
        movem.l d2-d7/a2-a6,-(sp)
2:      lea     -48(a0),a0
        movem.l (a0),d1-d7/a2-a6
        movem.l d1-d7/a2-a6,-(a1)
        sub.l   #48,d0
        cmp.l   #48,d0
        bcc     2b
        movem.l (sp)+,d2-d7/a2-a6
back_longs:
        move.l  d0,d1
        lsr.l   #2,d1
        beq     back_tail
        subq.w  #1,d1
3:      move.l  -(a0),-(a1)
        dbra    d1,3b
back_tail:
        and.l   #3,d0
        beq     done
back_bytes:
        move.b  -(a0),-(a1)
        subq.l  #1,d0
        bne     back_bytes

done:
        move.l  4(sp),d0
        move.l  d0,a0
        rts
