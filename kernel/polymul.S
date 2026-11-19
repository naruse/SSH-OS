| The product at the heart of Poly1305: d = h * r, 9 limbs by 8, 17 out.
| Limbs are 16 bits, least significant first. In assembly because this is
| where SSH spends its time once logged in: every 16 bytes sent or received
| cost one of these, 72 multiplications, and the C compiler spends as long
| again on its loops and carries.
|
|   void poly_mul(u16 *d, const u16 *h, const u16 *r);
|
| One column of the product at a time: the sum of a column goes into d2,
| its carries are counted in d3, and at the end of the column the low word
| is stored and the rest carried on.

        .text
        .globl  poly_mul

        .macro  PROD i, j
        move.w  2*\i(a0),d0
        mulu.w  2*\j(a1),d0
        add.l   d0,d2
        addx.w  d4,d3
        .endm

        .macro  ENDCOL
        move.w  d2,(a2)+
        clr.w   d2
        swap    d2
        swap    d3
        add.l   d3,d2
        moveq   #0,d3
        .endm

poly_mul:
        movem.l d2-d4/a2,-(sp)
        movea.l 20(sp),a2               | d
        movea.l 24(sp),a0               | h
        movea.l 28(sp),a1               | r
        moveq   #0,d2
        moveq   #0,d3
        moveq   #0,d4

        PROD    0, 0
        ENDCOL
        PROD    0, 1
        PROD    1, 0
        ENDCOL
        PROD    0, 2
        PROD    1, 1
        PROD    2, 0
        ENDCOL
        PROD    0, 3
        PROD    1, 2
        PROD    2, 1
        PROD    3, 0
        ENDCOL
        PROD    0, 4
        PROD    1, 3
        PROD    2, 2
        PROD    3, 1
        PROD    4, 0
        ENDCOL
        PROD    0, 5
        PROD    1, 4
        PROD    2, 3
        PROD    3, 2
        PROD    4, 1
        PROD    5, 0
        ENDCOL
        PROD    0, 6
        PROD    1, 5
        PROD    2, 4
        PROD    3, 3
        PROD    4, 2
        PROD    5, 1
        PROD    6, 0
        ENDCOL
        PROD    0, 7
        PROD    1, 6
        PROD    2, 5
        PROD    3, 4
        PROD    4, 3
        PROD    5, 2
        PROD    6, 1
        PROD    7, 0
        ENDCOL
        PROD    1, 7
        PROD    2, 6
        PROD    3, 5
        PROD    4, 4
        PROD    5, 3
        PROD    6, 2
        PROD    7, 1
        PROD    8, 0
        ENDCOL
        PROD    2, 7
        PROD    3, 6
        PROD    4, 5
        PROD    5, 4
        PROD    6, 3
        PROD    7, 2
        PROD    8, 1
        ENDCOL
        PROD    3, 7
        PROD    4, 6
        PROD    5, 5
        PROD    6, 4
        PROD    7, 3
        PROD    8, 2
        ENDCOL
        PROD    4, 7
        PROD    5, 6
        PROD    6, 5
        PROD    7, 4
        PROD    8, 3
        ENDCOL
        PROD    5, 7
        PROD    6, 6
        PROD    7, 5
        PROD    8, 4
        ENDCOL
        PROD    6, 7
        PROD    7, 6
        PROD    8, 5
        ENDCOL
        PROD    7, 7
        PROD    8, 6
        ENDCOL
        PROD    8, 7
        ENDCOL
        move.w  d2,(a2)
        movem.l (sp)+,d2-d4/a2
        rts
