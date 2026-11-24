CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O3 -g
WARNINGS = -Wall -Wextra -Werror
CORE = src/roce_wire.c src/roce_rc.c
CORE_HEADERS = src/roce_wire.h src/roce_rc.h ../common/roce_icrc.h
LAB = src/roce_lab.c
LAB_HEADERS = src/roce_lab.h
MR = src/roce_mr_server.c src/roce_mr_ipc.c src/roce_mr_netlink.c
MR_HEADERS = src/roce_mr_server.h src/roce_mr_ipc.h src/roce_fd.h src/roce_mr_netlink.h kernel/roce_mr_uapi.h
CMA = src/roce_rdmacm.c src/roce_cma_ipc.c src/roce_native_connection.c
CMA_HEADERS = src/roce_cma_ipc.h src/roce_rdmacm_private.h src/roce_native_connection.h \
              src/roce_native_queue.h src/roce_doorbell.h src/roce_cm_wire.h $(LAB_HEADERS) $(CORE_HEADERS)
APP = src/roce_app.c src/roce_app_native.c src/roce_app_rdmacm.c $(MR) $(CMA)
APP_HEADERS = src/roce_app.h src/roce_app_native.h src/roce_app_rdmacm.h src/roce_doorbell.h \
              src/roce_native_queue.h $(MR_HEADERS) $(CMA_HEADERS)
DPDK_IO = ../mrc-dpdk/src/dpdk_io.c
CM = src/roce_cm_wire.c src/roce_cm_session.c
CM_HEADERS = src/roce_cm_wire.h src/roce_cm_session.h
CM_LAB = src/roce_cm_lab_offer.c
CM_LAB_HEADERS = src/roce_cm_lab_offer.h
DPDK_CFLAGS = $(shell $(PKG_CONFIG) --cflags libdpdk)
DPDK_LIBS = $(shell $(PKG_CONFIG) --libs libdpdk)
VERBS_CFLAGS = $(shell $(PKG_CONFIG) --cflags libibverbs)
VERBS_LIBS = $(shell $(PKG_CONFIG) --libs libibverbs)
CMA_CFLAGS = $(shell $(PKG_CONFIG) --cflags librdmacm)
CMA_LIBS = $(shell $(PKG_CONFIG) --libs librdmacm)

.PHONY: all check check-memif check-peer check-app clean
all: roce_write roce_verbs_write roce_app_write

roce_write: src/roce_write.c src/roce_cm_dpdk.c src/roce_cm_dpdk.h src/roce_cma_service.c src/roce_cma_service.h \
            $(CM) $(CM_HEADERS) $(CM_LAB) $(CM_LAB_HEADERS) $(CORE) $(CORE_HEADERS) $(LAB) $(LAB_HEADERS) $(APP) $(APP_HEADERS) \
            $(DPDK_IO) ../mrc-dpdk/src/dpdk_io.h
	$(PKG_CONFIG) --exists libdpdk
	$(CC) $(CPPFLAGS) $(DPDK_CFLAGS) $(VERBS_CFLAGS) $(CMA_CFLAGS) $(CFLAGS) -pthread -std=gnu11 $(WARNINGS) $(LDFLAGS) -o $@ $(filter %.c,$^) $(DPDK_LIBS) $(VERBS_LIBS) $(LDLIBS)

roce_verbs_write: src/roce_verbs_write.c $(CM_LAB) $(CM_LAB_HEADERS) $(CORE) $(CORE_HEADERS) $(LAB) $(LAB_HEADERS)
	$(PKG_CONFIG) --exists libibverbs librdmacm
	$(CC) $(CPPFLAGS) $(VERBS_CFLAGS) $(CMA_CFLAGS) $(CFLAGS) -std=gnu11 $(WARNINGS) $(LDFLAGS) -o $@ $(filter %.c,$^) $(VERBS_LIBS) $(CMA_LIBS) $(LDLIBS)

roce_app_write: src/roce_app_write.c $(CORE) $(CORE_HEADERS) $(LAB) $(LAB_HEADERS) $(APP) $(APP_HEADERS) $(CM_LAB_HEADERS)
	$(CC) $(CPPFLAGS) $(VERBS_CFLAGS) $(CMA_CFLAGS) $(CFLAGS) -pthread -std=gnu11 $(WARNINGS) $(LDFLAGS) -o $@ $(filter %.c,$^) $(VERBS_LIBS) $(LDLIBS)

libroce_rdmacm.so: $(CMA) $(CMA_HEADERS) src/roce_mr_ipc.c src/roce_mr_ipc.h src/roce_fd.h
	$(CC) $(CPPFLAGS) $(VERBS_CFLAGS) $(CMA_CFLAGS) $(CFLAGS) -pthread -std=gnu11 $(WARNINGS) \
		-fPIC -shared $(LDFLAGS) -Wl,-z,defs -o $@ $(filter %.c,$^) $(VERBS_LIBS) $(LDLIBS)

tests/test_roce: tests/test_roce.c $(CORE) $(CORE_HEADERS)
tests/test_roce_cm_wire: tests/test_roce_cm_wire.c src/roce_cm_wire.c src/roce_cm_wire.h src/roce_wire.c src/roce_wire.h ../common/roce_icrc.h
tests/test_roce_cm_session: tests/test_roce_cm_session.c $(CM) $(CM_HEADERS) $(CM_LAB) $(CM_LAB_HEADERS) \
                           $(CORE) $(CORE_HEADERS) $(LAB) $(LAB_HEADERS)
tests/test_roce_cma_ipc: tests/test_roce_cma_ipc.c src/roce_cma_ipc.c src/roce_cma_ipc.h src/roce_fd.h $(CM_HEADERS) $(CORE_HEADERS)
tests/roce_wire_cli: tests/roce_wire_cli.c src/roce_wire.c src/roce_wire.h ../common/roce_icrc.h
tests/test_doorbell: tests/test_doorbell.c src/roce_doorbell.h src/roce_native_queue.h $(LAB_HEADERS) $(CORE_HEADERS)
tests/test_roce tests/test_roce_cm_wire tests/test_roce_cm_session tests/test_roce_cma_ipc tests/roce_wire_cli tests/test_doorbell:
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc -std=c11 $(WARNINGS) -pedantic $(LDFLAGS) -o $@ $(filter %.c,$^) $(LDLIBS)

check: tests/test_roce tests/test_roce_cm_wire tests/test_roce_cm_session tests/test_roce_cma_ipc tests/roce_wire_cli tests/test_doorbell
	./tests/test_roce
	./tests/test_roce_cm_wire
	./tests/test_roce_cm_session
	./tests/test_roce_cma_ipc
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
	$(RM) roce_write roce_verbs_write roce_app_write libroce_rdmacm.so tests/test_roce tests/test_roce_cm_wire tests/test_roce_cm_session tests/test_roce_cma_ipc tests/roce_wire_cli tests/test_doorbell
