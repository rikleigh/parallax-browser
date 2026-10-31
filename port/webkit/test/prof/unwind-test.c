// Host test for apps/common/ps5unwind.c: loads an ELF's .eh_frame_hdr and
// .eh_frame at their link offsets in one buffer, then answers "pc" queries
// (link addresses in hex, one per line on stdin) with the unwind rule:
//   <pc> <cfa reg> <cfa offset> <rbp offset or -> <ra offset>   or  <pc> fail <code>
// unwind-test.sh compares this with llvm-dwarfdump --eh-frame.
#include "../../apps/common/ps5unwind.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

// ELF64 headers (macOS has no <elf.h>).
typedef struct {
    unsigned char e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} Elf64_Ehdr;
typedef struct {
    uint32_t sh_name, sh_type;
    uint64_t sh_flags, sh_addr, sh_offset, sh_size;
    uint32_t sh_link, sh_info;
    uint64_t sh_addralign, sh_entsize;
} Elf64_Shdr;

int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <elf>\n", argv[0]);
        return 2;
    }
    int fd = open(argv[1], O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st))
        return 1;
    const unsigned char* file = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    const Elf64_Ehdr* eh = (const Elf64_Ehdr*)file;
    const Elf64_Shdr* sh = (const Elf64_Shdr*)(file + eh->e_shoff);
    const char* names = (const char*)file + sh[eh->e_shstrndx].sh_offset;
    const Elf64_Shdr *hdr = NULL, *frame = NULL;
    for (int i = 0; i < eh->e_shnum; i++) {
        if (!strcmp(names + sh[i].sh_name, ".eh_frame_hdr"))
            hdr = &sh[i];
        else if (!strcmp(names + sh[i].sh_name, ".eh_frame"))
            frame = &sh[i];
    }
    if (!hdr || !frame)
        return 1;
    size_t size = frame->sh_addr + frame->sh_size;
    unsigned char* image = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    memcpy(image + hdr->sh_addr, file + hdr->sh_offset, hdr->sh_size);
    memcpy(image + frame->sh_addr, file + frame->sh_offset, frame->sh_size);
    ps5unwind_info_t info;
    if (ps5unwind_init(&info, image + hdr->sh_addr, image + frame->sh_addr, image + frame->sh_addr + frame->sh_size)) {
        fprintf(stderr, "init failed\n");
        return 1;
    }
    fprintf(stderr, "%zu FDEs, text 0x%lx-0x%lx\n", info.count, (unsigned long)(info.textStart - (uintptr_t)image),
        (unsigned long)(info.textEnd - (uintptr_t)image));
    char line[64];
    while (fgets(line, sizeof line, stdin)) {
        unsigned long pc = strtoul(line, NULL, 16);
        ps5unwind_rule_t rule;
        int r = ps5unwind_rule(&info, (uintptr_t)image + pc, &rule);
        if (r)
            printf("%lx fail %d\n", pc, r);
        else if (rule.rbpSaved)
            printf("%lx %s %ld %ld %ld\n", pc, rule.cfaRegister == 7 ? "RSP" : "RBP", rule.cfaOffset, rule.rbpOffset, rule.raOffset);
        else
            printf("%lx %s %ld - %ld\n", pc, rule.cfaRegister == 7 ? "RSP" : "RBP", rule.cfaOffset, rule.raOffset);
    }
    return 0;
}
