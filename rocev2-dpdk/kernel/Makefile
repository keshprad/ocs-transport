ifneq ($(KERNELRELEASE),)
obj-m := roce_mr.o
else
KDIR ?= /lib/modules/$(shell uname -r)/build
PKG_CONFIG ?= pkg-config

.PHONY: all check check-dma clean
all:
	$(MAKE) -C "$(KDIR)" M="$(CURDIR)" modules
check_mr: check_mr.c roce_mr_uapi.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror -o $@ $<
check: check_mr
	./check_mr
check_mr_dmabuf: check_mr_dmabuf.c roce_mr_uapi.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		$(shell $(PKG_CONFIG) --cflags libibverbs) -o $@ $< \
		$(shell $(PKG_CONFIG) --libs libibverbs)
check-dma: check_mr_dmabuf
	./check_mr_dmabuf $(RDMA_DEVICE)
clean:
	$(MAKE) -C "$(KDIR)" M="$(CURDIR)" clean
	$(RM) check_mr check_mr_dmabuf
endif
