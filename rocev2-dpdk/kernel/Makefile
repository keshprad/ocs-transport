ifneq ($(KERNELRELEASE),)
obj-m := roce_mr.o
ifneq ($(filter y m,$(CONFIG_INFINIBAND_USER_ACCESS)),)
obj-m += roce_uverbs.o
endif
else
KDIR ?= /lib/modules/$(shell uname -r)/build
PKG_CONFIG ?= pkg-config

.PHONY: all check check-dma check-ticket clean
all:
	$(MAKE) -C "$(KDIR)" M="$(CURDIR)" modules
check_mr: check_mr.c roce_mr_uapi.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror -o $@ $<
check_mr_ticket: check_mr_ticket.c roce_mr_uapi.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror -o $@ $<
check: check_mr
	./check_mr
check-ticket: check_mr_ticket
	./check_mr_ticket
check_mr_dmabuf: check_mr_dmabuf.c roce_mr_uapi.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		$(shell $(PKG_CONFIG) --cflags libibverbs) -o $@ $< \
		$(shell $(PKG_CONFIG) --libs libibverbs)
check-dma: check_mr_dmabuf
	./check_mr_dmabuf $(RDMA_DEVICE)
clean:
	$(MAKE) -C "$(KDIR)" M="$(CURDIR)" clean
	$(RM) check_mr check_mr_dmabuf check_mr_ticket
endif
