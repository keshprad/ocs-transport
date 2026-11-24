CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -g
WARNINGS = -Wall -Wextra -Werror
VERBS_CFLAGS = $(shell $(PKG_CONFIG) --cflags libibverbs)
VERBS_LIBS = $(shell $(PKG_CONFIG) --libs libibverbs)
PROVIDER = libroce_dpdk-rdmav34.so

.PHONY: all check check-mr clean
all: $(PROVIDER) check_provider check_provider_mr

$(PROVIDER): roce_provider.c ../src/roce_mr_ipc.c ../src/roce_mr_ipc.h ../src/roce_fd.h ../kernel/roce_mr_uapi.h
	@test -n "$(RDMA_CORE_BUILD)" && test -f "$(RDMA_CORE_BUILD)/include/kernel-abi/ib_user_verbs.h" || \
		{ echo 'Set RDMA_CORE_BUILD to a v50.0 rdma-core build with the ibverbs target built.' >&2; exit 1; }
	$(CC) $(CPPFLAGS) -isystem "$(RDMA_CORE_BUILD)/include" $(CFLAGS) $(WARNINGS) \
		-std=gnu11 -fPIC -shared $(LDFLAGS) -Wl,-z,defs -o $@ $(filter %.c,$^) $(VERBS_LIBS) $(LDLIBS)

check_provider: check_provider.c
	$(CC) $(CPPFLAGS) $(VERBS_CFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 $(LDFLAGS) -o $@ $< $(VERBS_LIBS) $(LDLIBS)

check_provider_mr: check_provider_mr.c ../src/roce_mr_server.c ../src/roce_mr_server.h \
                   ../src/roce_mr_ipc.c ../src/roce_mr_ipc.h ../src/roce_fd.h ../kernel/roce_mr_uapi.h
	$(CC) $(CPPFLAGS) $(VERBS_CFLAGS) $(CFLAGS) $(WARNINGS) -std=gnu11 -pthread $(LDFLAGS) \
		-o $@ $(filter %.c,$^) $(VERBS_LIBS) $(LDLIBS)

check: all
	ROCE_DPDK_MR_SOCKET= RDMAV_DRIVERS="$(CURDIR)/libroce_dpdk" ./check_provider

check-mr: all
	RDMAV_DRIVERS="$(CURDIR)/libroce_dpdk" ./check_provider_mr

clean:
	$(RM) $(PROVIDER) check_provider check_provider_mr
