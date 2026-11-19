/* The settings file, copied into the kernel as it is. The Makefile puts the
 * chosen file at build/config.txt. */
        .section .rodata
        .global config_text, config_text_len
config_text:
        .incbin "config.txt"
config_text_end:
        .byte   0
        .even
config_text_len:
        .long   config_text_end - config_text
