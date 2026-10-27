# Spike 3. hmd2_probe.elf is an elfldr payload (3a). gl_hmd2 (3b) links
# ps5-opengl and is packaged as a registered title: see README.md.
ELF := hmd2_probe.elf

include ../common/payload.mk

PSVR2 := ../common/psvr2/psvr2.c
LDLIBS := -lSceUserService -lSceVideoOut -lSceSysmodule -lkernel_sys

$(ELF): hmd2_probe.c $(PSVR2) ../common/psvr2/psvr2.h ../common/spike.h
	$(CC) $(CFLAGS) -o $@ hmd2_probe.c $(PSVR2) $(LDLIBS)
