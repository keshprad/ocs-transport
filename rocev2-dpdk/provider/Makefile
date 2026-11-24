CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -g
WARNINGS = -Wall -Wextra -Werror
VERBS_CFLAGS = $(shell $(PKG_CONFIG) --cflags libibverbs)
VERBS_LIBS = $(shell $(PKG_CONFIG) --libs libibverbs)
PROVIDER = libroce_dpdk-rdmav34.so

.PHONY: all check clean
all: $(PROVIDER) check_provider

$(PROVIDER): roce_provider.c
	@test -n "$(RDMA_CORE_BUILD)" && test -f "$(RDMA_CORE_BUILD)/include/kernel-abi/ib_user_verbs.h" || \
		{ echo 'Set RDMA_CORE_BUILD to a v50.0 rdma-core build with the ibverbs target built.' >&2; exit 1; }
	$(CC) $(CPPFLAGS) -isystem "$(RDMA_CORE_BUILD)/include" $(CFLAGS) $(WARNINGS) \
		-std=gnu11 -fPIC -shared $(LDFLAGS) -Wl,-z,defs -o $@ $< $(VERBS_LIBS) $(LDLIBS)

check_provider: check_provider.c
	$(CC) $(CPPFLAGS) $(VERBS_CFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 $(LDFLAGS) -o $@ $< $(VERBS_LIBS) $(LDLIBS)

check: all
	RDMAV_DRIVERS="$(CURDIR)/libroce_dpdk" ./check_provider

clean:
	$(RM) $(PROVIDER) check_provider
