# Spike 5: libopenxr_psvr2, a minimal OpenXR runtime for WebKit's OpenXR backend.
#
#   make            host (macOS) build: dylib + static lib + smoke tests (mock backend)
#   make test       run the smoke test: dlopen + negotiate, then statically linked
#   make loader-test  run it through the official OpenXR loader
#                   (LOADER_DIR=<dir containing libopenxr_loader.dylib>)
#   make ps5        cross-compile runtime + PS5 backend + common/psvr2 into a static
#                   archive with prospero-clang (needs `. ./env.sh`); not runnable here
#
# The OpenXR headers are pinned in toolchain/khronos/include/openxr (SOURCE.txt).

SHELL    := /bin/bash
ROOT     := $(abspath ../..)
KHRONOS  := $(ROOT)/toolchain/khronos/include
COMMON   := $(abspath ../common)
B        := build

RT_SRCS  := runtime/runtime.c runtime/session.c runtime/swapchain.c runtime/input.c runtime/backend_mock.c
RT_HDRS  := $(wildcard runtime/*.h)
WARN     := -Wall -Wextra -Werror -Wno-missing-field-initializers
HOST_CFLAGS := -std=c11 -O2 -g $(WARN) -fvisibility=hidden -I$(KHRONOS) -Iruntime -D_DEFAULT_SOURCE

HOST     := $(B)/host
DYLIB    := $(HOST)/libopenxr_psvr2.dylib
HOST_A   := $(HOST)/libopenxr_psvr2.a
SMOKE    := $(HOST)/xr_smoke
SMOKE_ST := $(HOST)/xr_smoke_static
SMOKE_LD := $(HOST)/xr_smoke_loader

all: $(DYLIB) $(HOST_A) $(SMOKE) $(SMOKE_ST)

$(HOST)/%.o: runtime/%.c $(RT_HDRS) | $(HOST)
	$(CC) $(HOST_CFLAGS) -fPIC -c -o $@ $<

HOST_OBJS := $(patsubst runtime/%.c,$(HOST)/%.o,$(RT_SRCS))

$(DYLIB): $(HOST_OBJS)
	$(CC) -dynamiclib -o $@ $^ -install_name @rpath/libopenxr_psvr2.dylib

$(HOST_A): $(HOST_OBJS)
	rm -f $@ && ar rcs $@ $^

TEST_CFLAGS := -std=c11 -O2 -g $(WARN) -I$(KHRONOS) -D_DEFAULT_SOURCE

$(SMOKE): test/xr_smoke.c $(RT_HDRS) | $(HOST)
	$(CC) $(TEST_CFLAGS) -o $@ test/xr_smoke.c

$(SMOKE_ST): test/xr_smoke.c $(HOST_A) | $(HOST)
	$(CC) $(TEST_CFLAGS) -DXR_SMOKE_STATIC -o $@ test/xr_smoke.c $(HOST_A)

$(SMOKE_LD): test/xr_smoke.c | $(HOST)
	@test -n "$(LOADER_DIR)" || (echo "set LOADER_DIR to the directory of libopenxr_loader.dylib"; exit 1)
	$(CC) $(TEST_CFLAGS) -DXR_SMOKE_LOADER -o $@ test/xr_smoke.c -L$(LOADER_DIR) -lopenxr_loader \
	    -Wl,-rpath,$(LOADER_DIR)

# Logs land in logs/ (collect with: tools/results.sh spikes/05-openxr-runtime/logs/*.log)
STAMP = $(shell date +%Y%m%d-%H%M%S)

test: $(SMOKE) $(SMOKE_ST) $(DYLIB)
	@mkdir -p logs
	set -o pipefail; ./$(SMOKE) $(DYLIB) 2>&1 | tee logs/xr_smoke-dlopen-$(STAMP).log
	set -o pipefail; ./$(SMOKE_ST) --frames 120 2>&1 | tee logs/xr_smoke-static-$(STAMP).log

loader-test: $(SMOKE_LD) $(DYLIB)
	@mkdir -p logs
	set -o pipefail; XR_RUNTIME_JSON=$(CURDIR)/openxr_psvr2_host.json ./$(SMOKE_LD) --frames 120 2>&1 | \
	    tee logs/xr_smoke-loader-$(STAMP).log

$(HOST) $(B)/ps5:
	mkdir -p $@

# -- PS5 ----------------------------------------------------------------------
PS5_CFLAGS := -std=c11 -O2 -g $(WARN) -fvisibility=hidden -I$(KHRONOS) -Iruntime -I$(COMMON) \
              -DXRRT_HAVE_PSVR2=1
PS5_SRCS   := $(RT_SRCS) runtime/backend_psvr2.c
PS5_OBJS   := $(patsubst runtime/%.c,$(B)/ps5/%.o,$(PS5_SRCS)) $(B)/ps5/psvr2.o
PS5_A      := $(B)/ps5/libopenxr_psvr2.a

ps5: $(PS5_A)

$(B)/ps5/%.o: runtime/%.c $(RT_HDRS) | $(B)/ps5
	@test -n "$(PS5_PAYLOAD_SDK)" || (echo "run . ./env.sh first"; exit 1)
	$(PS5_PAYLOAD_SDK)/bin/prospero-clang $(PS5_CFLAGS) -c -o $@ $<

$(B)/ps5/psvr2.o: $(COMMON)/psvr2/psvr2.c $(COMMON)/psvr2/psvr2.h | $(B)/ps5
	$(PS5_PAYLOAD_SDK)/bin/prospero-clang $(PS5_CFLAGS) -c -o $@ $<

$(PS5_A): $(PS5_OBJS)
	rm -f $@ && $(PS5_PAYLOAD_SDK)/bin/prospero-ar rcs $@ $^

clean:
	rm -rf $(B)

.PHONY: all test loader-test ps5 clean
