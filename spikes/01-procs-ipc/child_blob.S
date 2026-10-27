/* Embeds child.elf so the parent can hand it to elfldr. */
    .section .rodata
    .balign 16
    .global child_elf_start
    .global child_elf_end
child_elf_start:
    .incbin "child.elf"
child_elf_end:
    .section .note.GNU-stack,"",@progbits
