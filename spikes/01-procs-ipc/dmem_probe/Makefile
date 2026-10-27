ELF := dmem_probe.elf
include ../../common/payload.mk
$(ELF): dmem_probe.c
	$(CC) $(CFLAGS) -I../.. -o $@ dmem_probe.c -lkernel_sys
