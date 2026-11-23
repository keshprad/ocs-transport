CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O3 -g
WARNINGS = -Wall -Wextra -Werror
CORE = src/roce_wire.c src/roce_rc.c
CORE_HEADERS = src/roce_wire.h src/roce_rc.h ../common/roce_icrc.h
LAB = src/roce_lab.c
LAB_HEADERS = src/roce_lab.h
APP = src/roce_app.c
APP_HEADERS = src/roce_app.h src/roce_doorbell.h kernel/roce_mr_uapi.h
DPDK_IO = ../mrc-dpdk/src/dpdk_io.c
DPDK_CFLAGS = $(shell $(PKG_CONFIG) --cflags libdpdk)
DPDK_LIBS = $(shell $(PKG_CONFIG) --libs libdpdk)
VERBS_CFLAGS = $(shell $(PKG_CONFIG) --cflags libibverbs)
VERBS_LIBS = $(shell $(PKG_CONFIG) --libs libibverbs)

.PHONY: all check check-memif check-peer check-app clean
all: roce_write roce_verbs_write roce_app_write

roce_write: src/roce_write.c $(CORE) $(CORE_HEADERS) $(LAB) $(LAB_HEADERS) $(APP) $(APP_HEADERS) $(DPDK_IO) ../mrc-dpdk/src/dpdk_io.h
	$(PKG_CONFIG) --exists libdpdk
	$(CC) $(CPPFLAGS) $(DPDK_CFLAGS) $(CFLAGS) -pthread -std=gnu11 $(WARNINGS) $(LDFLAGS) -o $@ $(filter %.c,$^) $(DPDK_LIBS) $(LDLIBS)

roce_verbs_write: src/roce_verbs_write.c $(CORE) $(CORE_HEADERS) $(LAB) $(LAB_HEADERS)
	$(PKG_CONFIG) --exists libibverbs
	$(CC) $(CPPFLAGS) $(VERBS_CFLAGS) $(CFLAGS) -std=gnu11 $(WARNINGS) $(LDFLAGS) -o $@ $(filter %.c,$^) $(VERBS_LIBS) $(LDLIBS)

roce_app_write: src/roce_app_write.c $(CORE) $(CORE_HEADERS) $(LAB) $(LAB_HEADERS) $(APP) $(APP_HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread -std=gnu11 $(WARNINGS) $(LDFLAGS) -o $@ $(filter %.c,$^) $(LDLIBS)

tests/test_roce: tests/test_roce.c $(CORE) $(CORE_HEADERS)
tests/roce_wire_cli: tests/roce_wire_cli.c src/roce_wire.c src/roce_wire.h ../common/roce_icrc.h
tests/test_doorbell: tests/test_doorbell.c src/roce_doorbell.h
tests/test_roce tests/roce_wire_cli tests/test_doorbell:
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc -std=c11 $(WARNINGS) -pedantic $(LDFLAGS) -o $@ $(filter %.c,$^) $(LDLIBS)

check: tests/test_roce tests/roce_wire_cli tests/test_doorbell
	./tests/test_roce
	./tests/test_doorbell
	python3 tests/check_wire.py

check-memif: roce_write
	python3 tests/check_memif.py

check-peer: roce_write roce_app_write
	$(MAKE) -C ../mrc-dpdk tests/mrc_memif_relay
	python3 tests/check_peer.py

check-app: roce_write roce_app_write
	python3 tests/check_app_memif.py

clean:
	$(RM) roce_write roce_verbs_write roce_app_write tests/test_roce tests/roce_wire_cli tests/test_doorbell
