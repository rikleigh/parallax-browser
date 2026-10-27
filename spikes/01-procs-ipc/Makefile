# Spike 1 is split into groups after the first run panicked the kernel
# (see procs_ipc.c). Run them in order: safe, spawn, jit. fork is opt-in.
ELF := procs_ipc_safe.elf procs_ipc_spawn.elf procs_ipc_jit.elf procs_ipc_fork.elf
EXTRA_CLEAN := child.elf procs_ipc.elf

include ../common/payload.mk

GROUP_safe := 1
GROUP_spawn := 2
GROUP_jit := 3
GROUP_fork := 4

procs_ipc_%.elf: procs_ipc.c child_blob.S child.elf ipc_common.h ../common/spike.h
	$(CC) $(CFLAGS) -DSPIKE01_GROUP=$(GROUP_$*) -o $@ procs_ipc.c child_blob.S

child.elf: child.c ipc_common.h
	$(CC) $(CFLAGS) -o $@ child.c

send-%: procs_ipc_%.elf
	$(ROOT)/tools/send.sh $<
