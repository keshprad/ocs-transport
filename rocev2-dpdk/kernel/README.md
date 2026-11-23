# Kernel memory registration

`roce_mr.ko` provides `/dev/roce_mr`. Build it against the running
kernel headers with `make -C rocev2-dpdk/kernel` from the repository
root. The device defaults to mode `0600`. Deployment can grant
access to the application account explicitly.

1. The application populates its page-aligned memory and calls
   `ROCE_MR_REGISTER`. The module pins only pages in the caller's
   address space and charges them against the registering address
   space's `RLIMIT_MEMLOCK` as a long-term pin.
2. The returned descriptor refers to that one region. The application
   passes it to the worker over a Unix socket with `SCM_RIGHTS`. The
   worker checks the ABI version, length and write permission via
   `ROCE_MR_GET_INFO` before mapping it with `MAP_SHARED`, offset zero
   and the registered length.
3. The application keeps the region alive through work completion.
   The kernel retains the pins until every mapping and descriptor of
   the region is released. Read-only registrations cannot be upgraded
   to a writable mapping.

The mapped alias supports CPU access to registered pages. Hardware
scatter/gather needs a DMA-BUF export of the pinned pages and NIC
registration against that export. The alias uses special PTEs to map
anonymous folios. Linux GUP rejects that alias for a second pin.
The descriptor handoff uses the Unix socket. Generic Netlink
registration events and uverbs integration are follow-up milestones.
[Linux GUP design](https://docs.kernel.org/core-api/pin_user_pages.html)

Once the module is loaded and the device is available to the test
account, run `make -C rocev2-dpdk/kernel check`. The test transfers
a mapping descriptor between two processes, verifies visibility in
both, and checks that closing the descriptor and releasing the
application mapping leaves the worker mapping live.
