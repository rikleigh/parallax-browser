/* Embeds server.elf so the title can hand it to elfldr. */
    .section .rodata
    .balign 16
    .global server_elf_start
    .global server_elf_end
server_elf_start:
    .incbin "server.elf"
server_elf_end:
    .section .note.GNU-stack,"",@progbits
