CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O3 -g
DPDK_CFLAGS = $(shell $(PKG_CONFIG) --cflags libdpdk)
DPDK_LIBS = $(shell $(PKG_CONFIG) --libs libdpdk)
WARNINGS = -Wall -Wextra -Werror
MRC_INCLUDE_DIR ?=
MRC_HEADERS = $(if $(MRC_INCLUDE_DIR),$(addprefix $(MRC_INCLUDE_DIR)/,mrc.h mrc_api_ver.h mrc_ctl.h mrc_ctl_api_ver.h))
MRC_ABI_CFLAGS = -isystem "$(MRC_INCLUDE_DIR)" $(shell $(PKG_CONFIG) --cflags libibverbs)
MRC_ABI_LIBS = $(shell $(PKG_CONFIG) --libs libibverbs)
MRC_ABI_SRC = src/mrc_abi.c src/mrc_abi_async.c src/mrc_abi_hint.c src/mrc_abi_ctl.c src/mrc_abi_ev_format.c src/mrc_abi_ev_event.c \
              src/mrc_abi_ctl_port.c src/mrc_abi_qp.c src/mrc_abi_dpdk.c \
              src/mrc_abi_ep.c src/mrc_verbs_async.c $(MRC_ENDPOINT_SRC)
MRC_ABI_DEPS = src/mrc_abi.h src/mrc_dpdk.h src/mrc_verbs_async.h \
               $(MRC_ENDPOINT_HEADERS) $(MRC_HEADERS)

.PHONY: all check check-mrc-abi clean
all: mrc_bench mrc_write mrc_ctl_profile

mrc_bench: src/mrc_bench.c src/dpdk_io.c src/dpdk_io.h
MRC_ENDPOINT_SRC = src/mrc_endpoint.c src/dpdk_io.c src/mrc_qp.c src/mrc_wire.c src/mrc_cc.c src/mrc_wr.c \
                   src/mrc_ev.c src/mrc_ev_event_io.c src/mrc_mpr.c src/mrc_provider.c \
                   src/mrc_provider_qp.c src/mrc_ctl_store.c src/mrc_port_netlink.c
MRC_ENDPOINT_HEADERS = src/mrc_endpoint.h src/dpdk_io.h src/mrc_qp.h src/mrc_wire.h src/mrc_cc.h src/mrc_wr.h \
                       src/mrc_ev.h src/mrc_ev_event_io.h src/mrc_mpr.h src/mrc_provider.h src/mrc_ctl_store.h \
                       src/mrc_port_netlink.h

mrc_write: src/mrc_write.c src/mrc_connect.c src/mrc_oob.c src/mrc_connect.h src/mrc_oob.h \
           $(MRC_ENDPOINT_SRC) $(MRC_ENDPOINT_HEADERS)

tests/mrc_endpoint_application: tests/mrc_endpoint_application.c $(MRC_ENDPOINT_SRC) $(MRC_ENDPOINT_HEADERS)
tests/mrc_endpoint_application: CPPFLAGS += -Isrc
tests/mrc_endpoint_application: LDFLAGS += -Wl,--wrap=rte_eth_link_get_nowait

tests/mrc_memif_relay: tests/mrc_memif_relay.c src/dpdk_io.c src/dpdk_io.h
tests/mrc_memif_relay: CPPFLAGS += -Isrc

mrc_bench mrc_write tests/mrc_memif_relay tests/mrc_endpoint_application:
	$(PKG_CONFIG) --exists libdpdk
	$(CC) $(CPPFLAGS) $(DPDK_CFLAGS) $(CFLAGS) -std=gnu11 $(WARNINGS) $(LDFLAGS) -o $@ $(filter %.c,$^) $(DPDK_LIBS) $(LDLIBS)

tests/test_mrc_qp: tests/test_mrc_qp.c src/mrc_qp.c src/mrc_qp.h
tests/test_mrc_wire: tests/test_mrc_wire.c src/mrc_wire.c src/mrc_wire.h

tests/test_mrc_cc: tests/test_mrc_cc.c src/mrc_cc.c src/mrc_cc.h
tests/test_mrc_wr: tests/test_mrc_wr.c src/mrc_wr.c src/mrc_wire.c src/mrc_qp.c \
                   src/mrc_provider.c src/mrc_provider_qp.c src/mrc_wr.h src/mrc_wire.h src/mrc_qp.h src/mrc_provider.h

tests/test_mrc_connect: tests/test_mrc_connect.c src/mrc_connect.c src/mrc_connect.h src/mrc_qp.h src/mrc_wire.h
tests/test_mrc_ev: tests/test_mrc_ev.c src/mrc_ev.c src/mrc_ev.h
tests/test_mrc_mpr: tests/test_mrc_mpr.c src/mrc_mpr.c src/mrc_qp.c src/mrc_connect.c src/mrc_mpr.h src/mrc_qp.h src/mrc_connect.h src/mrc_wire.h
tests/test_mrc_provider: tests/test_mrc_provider.c src/mrc_provider.c src/mrc_provider_qp.c src/mrc_provider.h src/mrc_qp.h
mrc_ctl_profile: src/mrc_ctl_profile.c src/mrc_ctl_store.c src/mrc_ctl_store.h src/mrc_ev.h src/mrc_cc.h
tests/test_mrc_ctl_store: tests/test_mrc_ctl_store.c src/mrc_ctl_store.c src/mrc_ctl_store.h src/mrc_ev.h src/mrc_cc.h
tests/test_mrc_port_netlink: tests/test_mrc_port_netlink.c src/mrc_port_netlink.c src/mrc_port_netlink.h
mrc_write tests/test_mrc_wr tests/test_mrc_provider tests/mrc_endpoint_application: CFLAGS += -pthread

tests/test_mrc_qp tests/test_mrc_wire tests/test_mrc_cc tests/test_mrc_wr tests/test_mrc_connect tests/test_mrc_ev tests/test_mrc_mpr tests/test_mrc_provider mrc_ctl_profile tests/test_mrc_ctl_store tests/test_mrc_port_netlink:
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc -std=c11 $(WARNINGS) -pedantic $(LDFLAGS) -o $@ $(filter %.c,$^) $(LDLIBS)

libmrc-dpdk.so: $(MRC_ABI_SRC) $(MRC_ABI_DEPS)
	@test -n "$(MRC_INCLUDE_DIR)" || { echo "Set MRC_INCLUDE_DIR to the directory containing the OCP MRC headers" >&2; exit 1; }
	$(PKG_CONFIG) --exists libibverbs libdpdk
	$(CC) $(CPPFLAGS) $(MRC_ABI_CFLAGS) $(DPDK_CFLAGS) $(CFLAGS) -std=gnu11 $(WARNINGS) -pthread -fPIC -fvisibility=hidden $(LDFLAGS) -Wl,-z,defs -shared -o $@ $(filter %.c,$^) $(MRC_ABI_LIBS) $(DPDK_LIBS) $(LDLIBS)

tests/test_mrc_abi: tests/test_mrc_abi.c $(MRC_ABI_SRC) $(MRC_ABI_DEPS)
	@test -n "$(MRC_INCLUDE_DIR)" || { echo "Set MRC_INCLUDE_DIR to the directory containing the OCP MRC headers" >&2; exit 1; }
	$(PKG_CONFIG) --exists libibverbs libdpdk
	$(CC) $(CPPFLAGS) $(MRC_ABI_CFLAGS) $(DPDK_CFLAGS) $(CFLAGS) -Isrc -std=gnu11 $(WARNINGS) -pthread $(LDFLAGS) -Wl,--wrap=mrc_verbs_async_open -o $@ $(filter %.c,$^) $(MRC_ABI_LIBS) $(DPDK_LIBS) $(LDLIBS)

tests/mrc_abi_application: tests/mrc_abi_application.c src/dpdk_io.c src/dpdk_io.h src/mrc_dpdk.h libmrc-dpdk.so
	$(CC) $(CPPFLAGS) $(MRC_ABI_CFLAGS) $(DPDK_CFLAGS) $(CFLAGS) -Isrc -std=gnu11 $(WARNINGS) -pthread $(LDFLAGS) -o $@ $(filter %.c,$^) -L. -Wl,-rpath,'$$ORIGIN/..' -lmrc-dpdk $(MRC_ABI_LIBS) $(DPDK_LIBS) $(LDLIBS)

tests/test_mrc_ctl_abi: tests/test_mrc_ctl_abi.c src/mrc_dpdk.h libmrc-dpdk.so
	$(CC) $(CPPFLAGS) $(MRC_ABI_CFLAGS) $(CFLAGS) -Isrc -std=gnu11 $(WARNINGS) $(LDFLAGS) -o $@ $< -L. -Wl,-rpath,'$$ORIGIN/..' -lmrc-dpdk $(MRC_ABI_LIBS) $(LDLIBS)

check-mrc-abi: libmrc-dpdk.so tests/test_mrc_abi
	./tests/test_mrc_abi

check: tests/test_mrc_qp tests/test_mrc_wire tests/test_mrc_cc tests/test_mrc_wr tests/test_mrc_connect tests/test_mrc_ev tests/test_mrc_mpr tests/test_mrc_provider tests/test_mrc_ctl_store tests/test_mrc_port_netlink
	./tests/test_mrc_qp
	./tests/test_mrc_wire
	./tests/test_mrc_cc
	./tests/test_mrc_wr
	./tests/test_mrc_connect
	./tests/test_mrc_ev
	./tests/test_mrc_mpr
	./tests/test_mrc_provider
	./tests/test_mrc_ctl_store
	./tests/test_mrc_port_netlink

clean:
	$(RM) mrc_bench mrc_write mrc_ctl_profile tests/mrc_memif_relay tests/mrc_endpoint_application libmrc-dpdk.so tests/test_mrc_abi tests/test_mrc_ctl_abi tests/mrc_abi_application tests/test_mrc_qp tests/test_mrc_wire tests/test_mrc_cc tests/test_mrc_wr tests/test_mrc_connect tests/test_mrc_ev tests/test_mrc_mpr tests/test_mrc_provider tests/test_mrc_ctl_store tests/test_mrc_port_netlink
