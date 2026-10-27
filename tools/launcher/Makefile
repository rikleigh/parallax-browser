ELF := launcher.elf

include ../../spikes/common/payload.mk

LDLIBS := -lkernel_sys -lSceSystemService -lSceUserService

$(ELF): launcher.c
	$(CC) $(CFLAGS) -o $@ launcher.c $(LDLIBS)
