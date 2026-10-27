# Software-decode fallback payload; needs build-ffmpeg.sh first.
ELF := sw_decode.elf
include ../../common/payload.mk
FF := $(ROOT)/toolchain/ffmpeg-sw
CFLAGS += -std=gnu11 -I$(FF)/include -Wno-deprecated-declarations
LDLIBS := -L$(FF)/lib -lavformat -lavcodec -lavutil -ldav1d -lpthread -lkernel_sys
$(ELF): sw_decode.c
	$(CC) $(CFLAGS) -o $@ sw_decode.c $(LDLIBS)
