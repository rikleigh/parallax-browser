# ps5wk-relaunch.elf: the browser title's restart helper (ps5wk-relaunch.c),
# packaged in the title by apps/title/package.sh.
ELF := ps5wk-relaunch.elf

include ../../../../spikes/common/payload.mk

LDLIBS := -lkernel_sys -lSceSystemService -lSceUserService

$(ELF): ps5wk-relaunch.c
	$(CC) $(CFLAGS) -o $@ ps5wk-relaunch.c $(LDLIBS)
