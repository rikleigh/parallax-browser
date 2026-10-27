# Spike 8 server payload. The title (client.c) is built by build-title.sh,
# which embeds server.elf. `make send` runs the server by hand.
ELF := server.elf

include ../common/payload.mk

ifdef PS5WK_LOG_HOST
CFLAGS += -DLOG_HOST=\"$(PS5WK_LOG_HOST)\" -DLOG_PORT=$(PS5WK_LOG_PORT)
endif

server.elf: server.c ipc_proto.h ../01-procs-ipc/ipc_common.h ../common/spike.h
	$(CC) $(CFLAGS) -o $@ server.c
