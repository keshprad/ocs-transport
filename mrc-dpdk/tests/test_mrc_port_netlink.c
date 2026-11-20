#define _DEFAULT_SOURCE
#include "mrc_port_netlink.h"

#include <assert.h>
#include <errno.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned short interface_flags(const char *name)
{
    struct ifreq request = {0};
    assert(strlen(name) < sizeof(request.ifr_name));
    strcpy(request.ifr_name, name);
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    assert(fd >= 0);
    assert(ioctl(fd, SIOCGIFFLAGS, &request) == 0);
    assert(close(fd) == 0);
    return (unsigned short)request.ifr_flags;
}

static void expect_admin_state(const char *name, bool expected)
{
    unsigned int index = if_nametoindex(name);
    bool enabled = !expected;
    assert(index && mrc_port_netlink_query(index, &enabled) == 0);
    assert(enabled == expected);
    assert(((interface_flags(name) & IFF_UP) != 0) == expected);
}

static void set_from_child(unsigned int index, bool enable)
{
    pid_t child = fork();
    assert(child >= 0);
    if (!child)
        _exit(mrc_port_netlink_set(index, enable) != 0);
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void test_veth(void)
{
    const char *first = "mrc-test-a", *second = "mrc-test-b";
    unsigned int first_index = if_nametoindex(first), second_index = if_nametoindex(second);
    assert(first_index && second_index && first_index != second_index);
    /* Limit mutation to the disposable fixture topology. */
    assert(geteuid() == 0);
    struct if_nameindex *interfaces = if_nameindex();
    assert(interfaces);
    unsigned int count = 0;
    for (const struct if_nameindex *entry = interfaces; entry->if_index; entry++) {
        assert(!strcmp(entry->if_name, "lo") || !strcmp(entry->if_name, first) ||
               !strcmp(entry->if_name, second));
        count++;
    }
    if_freenameindex(interfaces);
    assert(count == 3);

    expect_admin_state(first, false);
    expect_admin_state(second, false);
    const unsigned short stable_flags = (unsigned short)~(IFF_UP | IFF_RUNNING);
    unsigned short first_flags = interface_flags(first) & stable_flags;
    unsigned short second_flags = interface_flags(second) & stable_flags;
    set_from_child(first_index, true);
    expect_admin_state(first, true);
    expect_admin_state(second, false);
    assert(mrc_port_netlink_set(second_index, true) == 0);
    expect_admin_state(second, true);

    set_from_child(first_index, false);
    expect_admin_state(first, false);
    expect_admin_state(second, true);
    assert((interface_flags(first) & stable_flags) == first_flags);
    assert((interface_flags(second) & stable_flags) == second_flags);
    assert(mrc_port_netlink_set(first_index, true) == 0);
    expect_admin_state(first, true);
    expect_admin_state(second, true);
    puts("Isolated veth port administration passed across processes");
}

int main(int argc, char **argv)
{
    bool enabled;
    assert(mrc_port_netlink_query(0, &enabled) == -EINVAL);
    unsigned int lo = if_nametoindex("lo");
    assert(lo && mrc_port_netlink_query(lo, NULL) == -EINVAL);
    expect_admin_state("lo", (interface_flags("lo") & IFF_UP) != 0);
    if (argc == 1) {
        puts("Read-only port netlink tests passed");
        return 0;
    }
    assert(argc == 2 && !strcmp(argv[1], "--veth"));
    test_veth();
    return 0;
}
