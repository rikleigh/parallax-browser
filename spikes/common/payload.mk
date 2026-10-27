# Shared rules for spikes built as elfldr payloads. Include after setting
# ELF (output) and SRCS. `make send` delivers the payload to $(PS5_HOST) and
# keeps the socket open, so its stdout comes back here; the log is also saved
# to logs/ for FEASIBILITY.md.

SPIKE_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
ROOT := $(abspath $(SPIKE_DIR)/..)

ifndef PS5_PAYLOAD_SDK
    $(error PS5_PAYLOAD_SDK is undefined: run `. ./env.sh` from the repo root)
endif
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

CFLAGS += -Wall -Wextra -Werror -O2 -g -I$(SPIKE_DIR)

all: $(ELF)

send: $(ELF)
	$(ROOT)/tools/send.sh $(ELF)

clean:
	rm -f $(ELF) $(EXTRA_CLEAN)

.PHONY: all send clean
