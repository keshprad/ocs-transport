CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O3 -g
DPDK_CFLAGS = $(shell $(PKG_CONFIG) --cflags libdpdk)
DPDK_LIBS = $(shell $(PKG_CONFIG) --libs libdpdk)
WARNINGS = -Wall -Wextra -Werror

.PHONY: all check clean
all: mrc_bench mrc_write

mrc_bench: src/mrc_bench.c src/dpdk_io.c src/dpdk_io.h
mrc_write: src/mrc_write.c src/dpdk_io.c src/mrc_qp.c src/mrc_wire.c src/mrc_cc.c src/mrc_wr.c src/mrc_connect.c src/mrc_oob.c src/mrc_ev.c src/mrc_mpr.c src/mrc_provider.c \
           src/dpdk_io.h src/mrc_qp.h src/mrc_wire.h src/mrc_cc.h src/mrc_wr.h src/mrc_connect.h src/mrc_oob.h src/mrc_ev.h src/mrc_mpr.h src/mrc_provider.h

mrc_bench mrc_write:
	$(PKG_CONFIG) --exists libdpdk
	$(CC) $(CPPFLAGS) $(DPDK_CFLAGS) $(CFLAGS) -std=gnu11 $(WARNINGS) $(LDFLAGS) -o $@ $(filter %.c,$^) $(DPDK_LIBS) $(LDLIBS)

tests/test_mrc_qp: tests/test_mrc_qp.c src/mrc_qp.c src/mrc_qp.h
tests/test_mrc_wire: tests/test_mrc_wire.c src/mrc_wire.c src/mrc_wire.h

tests/test_mrc_cc: tests/test_mrc_cc.c src/mrc_cc.c src/mrc_cc.h
tests/test_mrc_wr: tests/test_mrc_wr.c src/mrc_wr.c src/mrc_wire.c src/mrc_qp.c \
                   src/mrc_provider.c src/mrc_wr.h src/mrc_wire.h src/mrc_qp.h src/mrc_provider.h

tests/test_mrc_connect: tests/test_mrc_connect.c src/mrc_connect.c src/mrc_connect.h src/mrc_qp.h
tests/test_mrc_ev: tests/test_mrc_ev.c src/mrc_ev.c src/mrc_ev.h
tests/test_mrc_mpr: tests/test_mrc_mpr.c src/mrc_mpr.c src/mrc_qp.c src/mrc_connect.c src/mrc_mpr.h src/mrc_qp.h src/mrc_connect.h
tests/test_mrc_provider: tests/test_mrc_provider.c src/mrc_provider.c src/mrc_provider.h
mrc_write tests/test_mrc_wr tests/test_mrc_provider: CFLAGS += -pthread

tests/test_mrc_qp tests/test_mrc_wire tests/test_mrc_cc tests/test_mrc_wr tests/test_mrc_connect tests/test_mrc_ev tests/test_mrc_mpr tests/test_mrc_provider:
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc -std=c11 $(WARNINGS) -pedantic $(LDFLAGS) -o $@ $(filter %.c,$^) $(LDLIBS)

check: tests/test_mrc_qp tests/test_mrc_wire tests/test_mrc_cc tests/test_mrc_wr tests/test_mrc_connect tests/test_mrc_ev tests/test_mrc_mpr tests/test_mrc_provider
	./tests/test_mrc_qp
	./tests/test_mrc_wire
	./tests/test_mrc_cc
	./tests/test_mrc_wr
	./tests/test_mrc_connect
	./tests/test_mrc_ev
	./tests/test_mrc_mpr
	./tests/test_mrc_provider

clean:
	$(RM) mrc_bench mrc_write tests/test_mrc_qp tests/test_mrc_wire tests/test_mrc_cc tests/test_mrc_wr tests/test_mrc_connect tests/test_mrc_ev tests/test_mrc_mpr tests/test_mrc_provider
