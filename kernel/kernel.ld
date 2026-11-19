/* SSH OS kernel memory layout.
 *
 * The boot block loads the kernel image to KERNEL_ADDR and jumps to its first
 * byte, so .text.start (kernel/start.S) must come first.
 */
ENTRY(_start)

SECTIONS
{
    . = KERNEL_ADDR;
    __kernel_start = .;

    .text : {
        *(.text.start)
        *(.text .text.*)
    }
    .rodata : {
        *(.rodata .rodata.*)
    }
    .data : {
        *(.data .data.*)
        . = ALIGN(2);
    }

    /* Not stored in the image; start.S clears it. */
    __bss_start = .;
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(2);
    }
    __bss_end = .;
    __kernel_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame)
    }
}
