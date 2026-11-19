| SSH OS boot block - stage 1 loader.
|
| The Macintosh SE ROM reads the first 1024 bytes of the startup disk into
| RAM. If they begin with 'LK' and the byte at offset 6 is 0x44, it does
| JSR 2(A6) into them (A6 = address of this block in RAM). We never return.
|
| What this does:
|   1. Draws the name on a white screen, straight into video memory.
|   2. Asks the ROM disk driver to read the kernel, which sits on the disk
|      right after this block (byte offset 1024), to KERNEL_ADDR.
|   3. Turns interrupts off and jumps to the kernel. From there on the ROM
|      is never called again.
|
| Rules for this file:
|   - Exactly 1024 bytes, position independent (PC-relative addressing only).
|   - The only ROM call is the one _Read.
|   - Screen: 512 x 342, 1 bit per pixel, 64 bytes per row, 1 = black,
|     most significant bit is the leftmost pixel.
|
| KERNEL_ADDR and KERNEL_BYTES (a multiple of 512) come from the Makefile.

| ROM low-memory globals.
        .equ    BootDrive, 0x210        | drive number we booted from
        .equ    ScrnBase, 0x824         | address of video memory
        .equ    BtDskRfn, 0xB34         | driver reference number of the boot disk

| I/O parameter block offsets (Inside Macintosh, Device Manager).
        .equ    ioCompletion, 12
        .equ    ioVRefNum, 22
        .equ    ioRefNum, 24
        .equ    ioBuffer, 32
        .equ    ioReqCount, 36
        .equ    ioPosMode, 44
        .equ    ioPosOffset, 46
        .equ    fsFromStart, 1
        .equ    _Read, 0xA002

        .equ    ROWBYTES, 64
        .equ    ROWS, 342

| Layout. X positions and widths are in bytes (8 pixels), Y and heights in rows.
| The name's place must agree with kernel/splash.c.
        .equ    BIG_XB, 20
        .equ    BIG_Y, 164
        .equ    BIG_VSCALE, 4
        .equ    LOGO_XB, 26
        .equ    LOGO_Y, 50
        .equ    BY_Y, 206
        .equ    FOOT_Y, 310
        .equ    STATUS_XB, 25
        .equ    STATUS_Y, 264

        .text
        .org    0

| ---------------------------------------------------------------- header --
#ifdef SCSI_BOOT
| Started from a SCSI disk (the BlueSCSI). Block 0 of that disk says a
| "driver" lives at some block; the ROM loads it and calls its first byte
| with d5 = the disk's SCSI ID. This is that driver: it never returns.
                bra.w   start
#else
bb_id:          .ascii  "LK"            | 0x00 signature
bb_entry:       bra.w   start           | 0x02 ROM jumps here
bb_version:     .word   0x4418          | 0x06 high byte 0x44 = run my code
bb_pageflags:   .word   0               | 0x08
                .org    0x7A            | 0x0A seven unused 16-byte file names
bb_cntfcbs:     .word   10              | 0x7A
bb_cntevts:     .word   20              | 0x7C
bb_128kheap:    .long   0x00004300      | 0x7E
bb_256kheap:    .long   0x00008000      | 0x82
bb_sysheap:     .long   0x00020000      | 0x86
#endif

| ------------------------------------------------------------------ code --
                .org    0x8A
start:
        | Interrupts stay on until the kernel is loaded: the ROM disk driver
        | needs them. a4 and a6 survive the ROM call.
        movea.l ScrnBase.w,a4           | a4 = video memory

        | A white screen. The kernel's start-up picture (kernel/splash.c)
        | draws the name again in this same place and adds the rest.
        movea.l a4,a0
        move.w  #ROWS-1,d0
        moveq   #0,d2
bg_row: bsr     fill_row
        dbra    d0,bg_row

        lea     txt_big(pc),a0
        lea     (BIG_Y*ROWBYTES+BIG_XB)(a4),a1
        moveq   #txt_big_wb,d0
        moveq   #BIG_VSCALE,d2
        bsr     blit

#ifdef SCSI_BOOT
        | The mark above it, where the kernel's start-up picture has it too:
        | 96 rows of 12 bytes.
        lea     logo(pc),a0
        lea     (LOGO_Y*ROWBYTES+LOGO_XB)(a4),a1
        moveq   #96-1,d1
logo_row:
        movea.l a1,a3
        moveq   #12-1,d0
logo_byte:
        move.b  (a0)+,(a3)+
        dbra    d0,logo_byte
        lea     ROWBYTES(a1),a1
        dbra    d1,logo_row

        | And the small print of that picture: who made it, the line at the
        | foot of the screen and the address under it. All that is left for
        | the kernel to add are the small pictures of what has come up.
        lea     txt_by(pc),a0
        lea     (BY_Y*ROWBYTES+txt_by_xb)(a4),a1
        moveq   #txt_by_wb,d0
        bsr     blit10
        lea     txt_who(pc),a0
        lea     ((BY_Y+13)*ROWBYTES+txt_who_xb)(a4),a1
        moveq   #txt_who_wb,d0
        bsr     blit10
        lea     txt_where(pc),a0
        lea     ((FOOT_Y+11)*ROWBYTES+txt_where_xb)(a4),a1
        moveq   #txt_where_wb,d0
        bsr     blit10
        lea     (FOOT_Y*ROWBYTES)(a4),a0
        moveq   #-1,d2
        bsr     fill_row
#endif

#ifdef SCSI_BOOT
        | Read the kernel, which starts at block KERNEL_BLOCK of this disk,
        | 64 blocks at a time, each try up to three times.
        movea.l #KERNEL_ADDR,a2         | a2 = where the next blocks go
        moveq   #KERNEL_BLOCK,d3        | d3 = next block
        move.l  #KERNEL_BYTES/512,d6    | d6 = blocks still to read
next_chunk:
        moveq   #64,d4                  | d4 = blocks this time
        cmp.l   d4,d6
        bcc.s   1f
        move.l  d6,d4
1:      movea.w #3,a3                   | a3 = tries left
2:      bsr     read_blocks
        beq.s   3f
        subq.w  #1,a3
        move.l  a3,d0
        bne.s   2b
        bra     disk_error
3:      add.l   d4,d3
        sub.l   d4,d6
        move.l  d4,d0
        lsl.l   #8,d0
        add.l   d0,d0
        adda.l  d0,a2
        tst.l   d6
        bne.s   next_chunk
#else
        | Read the kernel. Reuse the parameter block the ROM just used to
        | read these boot blocks; it sits right below them in memory.
        lea     -0x400(a6),a0
        clr.l   ioCompletion(a0)
        move.w  BootDrive.w,ioVRefNum(a0)
        move.w  BtDskRfn.w,ioRefNum(a0)
        move.l  #KERNEL_ADDR,ioBuffer(a0)
        move.l  #KERNEL_BYTES,ioReqCount(a0)
        move.w  #fsFromStart,ioPosMode(a0)
        move.l  #1024,ioPosOffset(a0)
        .word   _Read
        tst.w   d0
        bne.s   disk_error
#endif

        move.w  #0x2700,sr              | interrupts off: the machine is ours
        jmp     KERNEL_ADDR

disk_error:
        lea     txt_error(pc),a0
        bsr     show_status
halt:   bra.s   halt

#ifdef SCSI_BOOT
| read_blocks: read d4 blocks (1 to 64) starting at block d3 of the disk with
| SCSI ID d5 to a2, through the ROM's SCSI Manager, the same way the ROM read
| this code. Returns with Z set if it worked. Uses d0-d2, d7, a0-a1.
|
| The calls are Pascal ones: the result's place is pushed first, then the
| arguments, then which call it is; the ROM removes all but the result.
        .equ    _SCSIDispatch, 0xA815
        .equ    TIB, -32                | 20 bytes: what to do with the data
        .equ    CDB, -12                | 6 bytes: the command
        .equ    STAT, -6
        .equ    MSG, -4
read_blocks:
        link    a6,#-32
        lea     CDB(a6),a0
        move.b  #0x08,(a0)+             | READ(6)
        move.l  d3,d0
        swap    d0
        andi.b  #0x1F,d0
        move.b  d0,(a0)+
        move.w  d3,(a0)+
        move.b  d4,(a0)+
        clr.b   (a0)+
        subq.l  #2,sp                   | the result's place, used by every call
        move.w  #1,-(sp)                | SCSIGet
        .word   _SCSIDispatch
        move.w  (sp),d7
        bne.s   rb_done
        move.w  d5,-(sp)
        move.w  #2,-(sp)                | SCSISelect(id)
        .word   _SCSIDispatch
        move.w  (sp),d7
        bne.s   rb_done
        pea     CDB(a6)
        move.w  #6,-(sp)
        move.w  #3,-(sp)                | SCSICmd(command, 6)
        .word   _SCSIDispatch
        move.w  (sp),d7
        bne.s   rb_complete
        lea     TIB(a6),a0
        move.w  #1,(a0)+                | move this many bytes to here
        move.l  a2,(a0)+
        move.l  d4,d0
        lsl.l   #8,d0
        add.l   d0,d0
        move.l  d0,(a0)+
        move.w  #7,(a0)                 | and stop
        pea     TIB(a6)
        move.w  #5,-(sp)                | SCSIRead(what to do)
        .word   _SCSIDispatch
        move.w  (sp),d7
rb_complete:
        pea     STAT(a6)
        pea     MSG(a6)
        pea     600.w                   | wait up to 10 seconds
        move.w  #4,-(sp)                | SCSIComplete(status, message, wait)
        .word   _SCSIDispatch
        tst.w   d7
        bne.s   rb_done
        move.w  (sp),d7
        bne.s   rb_done
        move.w  STAT(a6),d7             | the disk's own verdict: 0 is good
rb_done:
        unlk    a6
        tst.w   d7
        rts
#endif

| show_status: draw a one-line message (bitmap at a0) under the name.
show_status:
        lea     (STATUS_Y*ROWBYTES+STATUS_XB)(a4),a1
        moveq   #txt_error_wb,d0
        moveq   #1,d2
        | falls through into blit

| blit: copy an 8-row bitmap to the screen.
|   a0 = bitmap, a1 = screen address of its top-left byte,
|   d0 = bytes per row, d2 = times to repeat each row (vertical scale).
| Uses d1, d3, d4, a2, a3.
blit:
        moveq   #8-1,d1
        bra.s   blit_row
| blit10: the same for a bitmap of 10 rows, each drawn once.
blit10:
        moveq   #1,d2
        moveq   #10-1,d1
blit_row:
        move.w  d2,d3
        subq.w  #1,d3
blit_repeat:
        movea.l a0,a2
        movea.l a1,a3
        move.w  d0,d4
        subq.w  #1,d4
blit_byte:
        move.b  (a2)+,(a3)+
        dbra    d4,blit_byte
        lea     ROWBYTES(a1),a1
        dbra    d3,blit_repeat
        adda.w  d0,a0
        dbra    d1,blit_row
        rts

| fill_row: write the long in d2 across one screen row at a0; a0 advances.
| Uses d1.
fill_row:
        moveq   #ROWBYTES/4-1,d1
fill_long:
        move.l  d2,(a0)+
        dbra    d1,fill_long
        rts

| ------------------------------------------------------------------ data --
#include "banner.inc"

#ifdef SCSI_BOOT
        .org    0xC00                   | six blocks: up to where the kernel starts
#else
        .org    0x400                   | assembler fails here if we overflow
#endif
