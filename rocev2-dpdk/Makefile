CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O3 -g
WARNINGS = -Wall -Wextra -Werror
CORE = src/roce_wire.c src/roce_rc.c
CORE_HEADERS = src/roce_wire.h src/roce_rc.h ../common/roce_icrc.h
LAB = src/roce_lab.c
LAB_HEADERS = src/roce_lab.h
DPDK_IO = ../mrc-dpdk/src/dpdk_io.c
DPDK_CFLAGS = $(shell $(PKG_CONFIG) --cflags libdpdk)
DPDK_LIBS = $(shell $(PKG_CONFIG) --libs libdpdk)
VERBS_CFLAGS = $(shell $(PKG_CONFIG) --cflags libibverbs)
VERBS_LIBS = $(shell $(PKG_CONFIG) --libs libibverbs)

.PHONY: all check check-memif clean
all: roce_write roce_verbs_write

roce_write: src/roce_write.c $(CORE) $(CORE_HEADERS) $(LAB) $(LAB_HEADERS) $(DPDK_IO) ../mrc-dpdk/src/dpdk_io.h
	$(PKG_CONFIG) --exists libdpdk
	$(CC) $(CPPFLAGS) $(DPDK_CFLAGS) $(CFLAGS) -std=gnu11 $(WARNINGS) $(LDFLAGS) -o $@ $(filter %.c,$^) $(DPDK_LIBS) $(LDLIBS)

roce_verbs_write: src/roce_verbs_write.c $(CORE) $(CORE_HEADERS) $(LAB) $(LAB_HEADERS)
	$(PKG_CONFIG) --exists libibverbs
	$(CC) $(CPPFLAGS) $(VERBS_CFLAGS) $(CFLAGS) -std=gnu11 $(WARNINGS) $(LDFLAGS) -o $@ $(filter %.c,$^) $(VERBS_LIBS) $(LDLIBS)

tests/test_roce: tests/test_roce.c $(CORE) $(CORE_HEADERS)
tests/roce_wire_cli: tests/roce_wire_cli.c src/roce_wire.c src/roce_wire.h ../common/roce_icrc.h
tests/test_roce tests/roce_wire_cli:
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc -std=c11 $(WARNINGS) -pedantic $(LDFLAGS) -o $@ $(filter %.c,$^) $(LDLIBS)

check: tests/test_roce tests/roce_wire_cli
	./tests/test_roce
	python3 tests/check_wire.py

check-memif: roce_write
	python3 tests/check_memif.py

clean:
	$(RM) roce_write roce_verbs_write tests/test_roce tests/roce_wire_cli
