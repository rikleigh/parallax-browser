/* pad.S - ~100 MB of non-zero padding for the binary-size variant (-DPAD):
 * half in an executable section (like WebKit's text), half in rodata. The
 * files are random bytes made by build-title.sh (build/, never committed). */
    .section .text.ps5wk_pad, "ax", @progbits
    .balign 16384
    .globl ps5wk_pad_text
ps5wk_pad_text:
    .incbin PAD_TEXT_FILE
    .globl ps5wk_pad_text_end
ps5wk_pad_text_end:

    .section .rodata.ps5wk_pad, "a", @progbits
    .balign 16384
    .globl ps5wk_pad_ro
ps5wk_pad_ro:
    .incbin PAD_RO_FILE
    .globl ps5wk_pad_ro_end
ps5wk_pad_ro_end:
    .section .note.GNU-stack, "", @progbits
