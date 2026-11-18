# ps5wk-relaunch.elf: the browser title's restart helper (ps5wk-relaunch.c),
# packaged in the title by apps/title/package.sh. UPDATE_TESTING=1 (package.sh,
# for development builds: those with a log host) reads the update test
# hooks (port/webkit/UPDATES.md, "Testing"); a release never does.
ELF := ps5wk-relaunch.elf

include ../../../../spikes/common/payload.mk

LDLIBS := -lkernel_sys -lSceSystemService -lSceUserService
ifeq ($(UPDATE_TESTING),1)
CFLAGS += -DPS5WK_UPDATE_TESTING=1
endif

$(ELF): ps5wk-relaunch.c ../common/ps5update.c ../common/ps5update.h
	$(CC) $(CFLAGS) -o $@ ps5wk-relaunch.c ../common/ps5update.c $(LDLIBS)
