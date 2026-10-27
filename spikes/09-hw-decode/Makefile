# Spike 9 as an elfldr payload (the secondary data point: can a payload use
# the hardware decoder?). The title build is build-title.sh.
ELF := hw_decode.elf

include ../common/payload.mk

CFLAGS += -DPAYLOAD -std=gnu11
LDLIBS := -lSceSysmodule -lkernel_sys

$(ELF): hw_decode.c vdec2.c au.c vdec2.h au.h exports/videodec2_exports_1360.h ../common/spike.h
	$(CC) $(CFLAGS) -o $@ hw_decode.c vdec2.c au.c $(LDLIBS)
