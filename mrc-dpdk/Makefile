CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O3 -g
DPDK_CFLAGS = $(shell $(PKG_CONFIG) --cflags libdpdk)
DPDK_LIBS = $(shell $(PKG_CONFIG) --libs libdpdk)

.PHONY: all clean
all: mrc_bench

mrc_bench: src/mrc_bench.c
	$(PKG_CONFIG) --exists libdpdk
	$(CC) $(CPPFLAGS) $(DPDK_CFLAGS) $(CFLAGS) -std=gnu11 -Wall -Wextra -Werror $(LDFLAGS) -o $@ $< $(DPDK_LIBS) $(LDLIBS)

clean:
	$(RM) mrc_bench
