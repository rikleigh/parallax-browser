# Audio probe payloads (see audioprobe.c). `make` builds the silent probe and
# the tone variant.
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
CFLAGS += -Wall -O2
LDLIBS := -lSceAudioOut -lSceUserService

all: audioprobe.elf audioprobe-tone.elf
audioprobe.elf: audioprobe.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)
audioprobe-tone.elf: audioprobe.c
	$(CC) $(CFLAGS) -DTONE=1 -o $@ $< $(LDLIBS)
clean:
	rm -f audioprobe.elf audioprobe-tone.elf
