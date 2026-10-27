ELF := sysinfo.elf

include ../common/payload.mk

LDLIBS := -lkernel_sys

$(ELF): sysinfo.c ../common/spike.h
	$(CC) $(CFLAGS) -o $@ sysinfo.c $(LDLIBS)

# The SDK's own sample, to prove the toolchain and elfldr path first.
hello:
	$(MAKE) -C $(PS5_PAYLOAD_SDK)/samples/hello_world
	$(ROOT)/tools/send.sh $(PS5_PAYLOAD_SDK)/samples/hello_world/hello_world.elf
.PHONY: hello
