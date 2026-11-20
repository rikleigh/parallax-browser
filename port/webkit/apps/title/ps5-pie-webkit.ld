/*
 * ps5-native-app-boilerplate - Intermediate PIE linker layout.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ps5webkit copy (port/webkit/apps/title): WebKit data sections added.
 * Keeps code, immutable data, RELRO, and writable data in distinct address
 * ranges. The native converter replaces LLVM's generic dynamic metadata.
 *
 * the engine delta (Phase 1b task 4): PROVIDE the __eh_frame[_hdr]_{start,end} bounds
 * symbols. libunwind.a references them; the SDK's default link script defines
 * them but a custom `-T` script must too. ProsperoLight never hit this because
 * its unwind usage doesn't pull the frame-registration path the engine's does.
 */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)

PHDRS {
    text PT_LOAD FLAGS (0x5);
    ro PT_LOAD FLAGS (0x4);
    relro PT_LOAD FLAGS (0x6);
    data PT_LOAD FLAGS (0x6);
    dynamic PT_DYNAMIC FLAGS (0x6);
}

SECTIONS {
    .text 0 : ALIGN(CONSTANT(MAXPAGESIZE)) {
        QUAD(0xcccccccccccccccc)
        QUAD(0xcccccccccccccccc)
        *(.text .text.*)
        *(.plt .plt.*)
        /* The end of the code: the crash guard's bound for return addresses
         * where code can't be read (apps/common/crashguard.c). */
        PROVIDE_HIDDEN(_etext = .);
    } : text

    .eh_frame_hdr : ALIGN(CONSTANT(MAXPAGESIZE)) {
        PROVIDE_HIDDEN(__eh_frame_hdr_start = .);
        KEEP(*(.eh_frame_hdr))
        PROVIDE_HIDDEN(__eh_frame_hdr_end = .);
    } : ro
    .eh_frame : {
        PROVIDE_HIDDEN(__eh_frame_start = .);
        KEEP(*(.eh_frame))
        PROVIDE_HIDDEN(__eh_frame_end = .);
    } : ro
    .gcc_except_table : { *(.gcc_except_table .gcc_except_table.*) } : ro
    .rodata : { *(.rodata .rodata.*) } : ro

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .data.rel.ro : {
        *(.data.rel.ro .data.rel.ro.*)
    } : relro
    .got : { *(.got) } : relro
    .got.plt : { *(.got.plt) } : relro
    .preinit_array : {
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array))
        PROVIDE_HIDDEN(__preinit_array_end = .);
    } : relro
    .init_array : {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP(*(.init_array .ctors))
        PROVIDE_HIDDEN(__init_array_end = .);
    } : relro
    .fini_array : {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
        KEEP(*(.fini_array .dtors))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } : relro

    /* Kept only long enough for the converter to read and rewrite them. */
    .dynsym : { *(.dynsym) } : relro
    .gnu.hash : { *(.gnu.hash) } : relro
    .dynstr : { *(.dynstr) } : relro
    .rela.dyn : { *(.rela.dyn) } : relro
    .rela.plt : { *(.rela.plt) } : relro

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .dynamic : {
        *(.dynamic)
    } : data : dynamic
    .data : { *(.data .data.*) } : data
    /* WebKit: JSC's opcode table and WTF's config page are writable,
     * page-aligned sections that WTF later freezes with mprotect. Here, not
     * among RELRO, where the converter needs RELRO to end before data. */
    "__DATA,__jsc_opcodes" : ALIGN(16384) { KEEP(*("__DATA,__jsc_opcodes")) } : data
    "__DATA,__wtf_config" : ALIGN(16384) { KEEP(*("__DATA,__wtf_config")) } : data
    .tdata : { *(.tdata .tdata.*) } : data
    .tbss : { *(.tbss .tbss.*) } : data
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } : data
}
