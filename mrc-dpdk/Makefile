CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O3 -g
DPDK_CFLAGS = $(shell $(PKG_CONFIG) --cflags libdpdk)
DPDK_LIBS = $(shell $(PKG_CONFIG) --libs libdpdk)
WARNINGS = -Wall -Wextra -Werror

.PHONY: all check clean
all: mrc_bench mrc_write

mrc_bench: src/mrc_bench.c src/dpdk_io.c src/dpdk_io.h
mrc_write: src/mrc_write.c src/dpdk_io.c src/mrc_qp.c src/mrc_wire.c src/dpdk_io.h src/mrc_qp.h src/mrc_wire.h

mrc_bench mrc_write:
	$(PKG_CONFIG) --exists libdpdk
	$(CC) $(CPPFLAGS) $(DPDK_CFLAGS) $(CFLAGS) -std=gnu11 $(WARNINGS) $(LDFLAGS) -o $@ $(filter %.c,$^) $(DPDK_LIBS) $(LDLIBS)

tests/test_mrc_qp: tests/test_mrc_qp.c src/mrc_qp.c src/mrc_qp.h
tests/test_mrc_wire: tests/test_mrc_wire.c src/mrc_wire.c src/mrc_wire.h

tests/test_mrc_qp tests/test_mrc_wire:
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc -std=c11 $(WARNINGS) -pedantic $(LDFLAGS) -o $@ $(filter %.c,$^) $(LDLIBS)

check: tests/test_mrc_qp tests/test_mrc_wire
	./tests/test_mrc_qp
	./tests/test_mrc_wire

clean:
	$(RM) mrc_bench mrc_write tests/test_mrc_qp tests/test_mrc_wire
