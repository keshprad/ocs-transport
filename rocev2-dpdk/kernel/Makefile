ifneq ($(KERNELRELEASE),)
obj-m := roce_mr.o
else
KDIR ?= /lib/modules/$(shell uname -r)/build

.PHONY: all check clean
all:
	$(MAKE) -C "$(KDIR)" M="$(CURDIR)" modules
check_mr: check_mr.c roce_mr_uapi.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror -o $@ $<
check: check_mr
	./check_mr
clean:
	$(MAKE) -C "$(KDIR)" M="$(CURDIR)" clean
	$(RM) check_mr
endif
