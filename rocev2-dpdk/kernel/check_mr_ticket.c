#define _GNU_SOURCE

#include "roce_mr_uapi.h"

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

_Static_assert(sizeof(struct roce_mr_export_ticket) == 16, "export ABI layout");
_Static_assert(sizeof(struct roce_mr_ticket) == 16, "ticket ABI layout");

union nl_message {
	struct nlmsghdr header;
	unsigned char bytes[4096];
};

static void require(int condition, const char *message)
{
	if (!condition)
		errx(1, "%s", message);
}

static void receive_nl(int fd, union nl_message *message)
{
	struct sockaddr_nl sender;
	socklen_t size = sizeof(sender);
	ssize_t len = recvfrom(fd, message->bytes, sizeof(message->bytes), 0,
			       (struct sockaddr *)&sender, &size);

	if (len < 0)
		err(1, "receive netlink message");
	require(size == sizeof(sender) && sender.nl_pid == 0 &&
		len >= (ssize_t)(NLMSG_HDRLEN + GENL_HDRLEN) &&
		message->header.nlmsg_len == (size_t)len, "invalid kernel netlink message");
}

static void read_nl_attr(const union nl_message *message, uint16_t type,
			 void *value, size_t length)
{
	size_t offset = NLMSG_HDRLEN + GENL_HDRLEN;
	size_t remaining = message->header.nlmsg_len - offset;
	const unsigned char *data = message->bytes + offset;

	while (remaining >= NLA_HDRLEN) {
		struct nlattr attr;
		memcpy(&attr, data, sizeof(attr));
		require(attr.nla_len >= NLA_HDRLEN && (size_t)NLA_ALIGN(attr.nla_len) <= remaining,
			"invalid netlink attribute length");
		if ((attr.nla_type & NLA_TYPE_MASK) == type) {
			require((size_t)attr.nla_len == NLA_HDRLEN + length,
				"incorrect netlink attribute size");
			memcpy(value, data + NLA_HDRLEN, length);
			return;
		}
		data += NLA_ALIGN(attr.nla_len);
		remaining -= NLA_ALIGN(attr.nla_len);
	}
	errx(1, "missing netlink attribute %u", type);
}

static uint16_t resolve_family(int fd)
{
	struct sockaddr_nl kernel = { .nl_family = AF_NETLINK };
	struct {
		struct nlmsghdr header;
		struct genlmsghdr generic;
		struct nlattr attribute;
		char name[NLA_ALIGN(sizeof(ROCE_MR_GENL_NAME))];
	} request = {
		.header = {
			.nlmsg_len = sizeof(request), .nlmsg_type = GENL_ID_CTRL,
			.nlmsg_flags = NLM_F_REQUEST, .nlmsg_seq = 1,
		},
		.generic = { .cmd = CTRL_CMD_GETFAMILY, .version = 2 },
		.attribute = {
			.nla_len = NLA_HDRLEN + sizeof(ROCE_MR_GENL_NAME),
			.nla_type = CTRL_ATTR_FAMILY_NAME,
		},
		.name = ROCE_MR_GENL_NAME,
	};
	union nl_message message;
	uint16_t family;

	if (sendto(fd, &request, sizeof(request), 0,
		   (struct sockaddr *)&kernel, sizeof(kernel)) != sizeof(request))
		err(1, "resolve MR netlink family");
	receive_nl(fd, &message);
	require(message.header.nlmsg_type == GENL_ID_CTRL, "MR netlink family unavailable");
	read_nl_attr(&message, CTRL_ATTR_FAMILY_ID, &family, sizeof(family));
	return family;
}

static struct roce_mr_ticket receive_ticket(int nl, int peer, uint16_t family, uint64_t expected)
{
	union nl_message message;
	struct genlmsghdr generic;
	struct roce_mr_ticket ticket;
	uint64_t cookie;
	char ack;

	receive_nl(nl, &message);
	memcpy(&generic, message.bytes + NLMSG_HDRLEN, sizeof(generic));
	require(message.header.nlmsg_type == family && generic.cmd == ROCE_MR_GENL_CMD_TICKET &&
		generic.version == ROCE_MR_GENL_VERSION, "unexpected MR ticket notification");
	read_nl_attr(&message, ROCE_MR_GENL_A_TICKET, &ticket, sizeof(ticket));
	read_nl_attr(&message, ROCE_MR_GENL_A_COOKIE, &cookie, sizeof(cookie));
	require(cookie == expected, "MR notification did not preserve the worker cookie");
	require(send(peer, &ticket, sizeof(ticket), 0) == sizeof(ticket), "return observed ticket");
	require(read(peer, &ack, 1) == 1 && ack == 'G', "wait for ticket import signal");
	return ticket;
}

static void check_worker(const struct sockaddr_un *address, socklen_t address_len, size_t page_size)
{
	struct sockaddr_nl local = { .nl_family = AF_NETLINK };
	socklen_t local_len = sizeof(local);
	struct rlimit fd_limit, exhausted;
	struct roce_mr_ticket ticket;
	struct roce_mr_info info;
	int peer, nl, control, mapping;
	unsigned char *view;
	uint16_t family;

	nl = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC);
	if (nl < 0 || bind(nl, (struct sockaddr *)&local, sizeof(local)) ||
	    getsockname(nl, (struct sockaddr *)&local, &local_len))
		err(1, "bind worker netlink socket");
	family = resolve_family(nl);
	peer = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (peer < 0 || connect(peer, (const struct sockaddr *)address, address_len))
		err(1, "connect worker management socket");
	control = open(ROCE_MR_DEVICE, O_RDWR | O_CLOEXEC);
	if (control < 0)
		err(1, "open worker MR control device");
	require(send(peer, &local.nl_pid, sizeof(local.nl_pid), 0) == sizeof(local.nl_pid),
		"announce worker netlink portid");

	ticket = receive_ticket(nl, peer, family, UINT64_C(0xabcde01234567890));
	if (getrlimit(RLIMIT_NOFILE, &fd_limit))
		err(1, "read file descriptor limit");
	exhausted = fd_limit;
	exhausted.rlim_cur = 0;
	if (setrlimit(RLIMIT_NOFILE, &exhausted))
		err(1, "reduce file descriptor limit");
	require(ioctl(control, ROCE_MR_IMPORT_TICKET, &ticket) == -1 && errno == EMFILE,
		"import did not handle file descriptor exhaustion");
	if (setrlimit(RLIMIT_NOFILE, &fd_limit))
		err(1, "restore file descriptor limit");
	mapping = ioctl(control, ROCE_MR_IMPORT_TICKET, &ticket);
	if (mapping < 0)
		err(1, "import authorized ticket");
	require(fcntl(mapping, F_GETFD) == FD_CLOEXEC &&
		ioctl(mapping, ROCE_MR_GET_INFO, &info) == 0 &&
		info.length == page_size && info.access == ROCE_MR_ACCESS_LOCAL_WRITE,
		"import returned incorrect mapping metadata");
	require(ioctl(control, ROCE_MR_IMPORT_TICKET, &ticket) == -1 && errno == ENOENT,
		"ticket replay was accepted");
	view = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_SHARED, mapping, 0);
	if (view == MAP_FAILED)
		err(1, "map imported region");
	require(view[0] == 7, "imported region does not see application memory");
	view[0] = 11;
	require(!munmap(view, page_size) && !close(mapping), "release worker mapping");
	require(send(peer, "W", 1, 0) == 1, "acknowledge worker memory write");

	ticket = receive_ticket(nl, peer, family, 42);
	require(ioctl(control, ROCE_MR_IMPORT_TICKET, &ticket) == -1 && errno == ENOENT,
		"ticket remained valid after exporter released the descriptor");
}

int main(void)
{
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	long page_size = sysconf(_SC_PAGESIZE);
	struct roce_mr_export_ticket export = { .cookie = UINT64_C(0xabcde01234567890) };
	struct roce_mr_register registration;
	struct roce_mr_ticket ticket;
	int address_size, listener, control, mapping, status;
	unsigned char *buffer;
	socklen_t address_len;
	pid_t worker;
	char ack;

	alarm(10);
	require(page_size > 0, "invalid host page size");
	address_size = snprintf(address.sun_path + 1, sizeof(address.sun_path) - 1,
				"roce-mr-ticket-%ld", (long)getpid());
	require(address_size > 0 && (size_t)address_size < sizeof(address.sun_path) - 1,
		"invalid management address");
	address_len = offsetof(struct sockaddr_un, sun_path) + 1 + address_size;
	listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (listener < 0 || bind(listener, (struct sockaddr *)&address, address_len) || listen(listener, 1))
		err(1, "open management listener");
	worker = fork();
	if (worker < 0)
		err(1, "fork worker");
	if (!worker) {
		alarm(10);
		close(listener);
		check_worker(&address, address_len, (size_t)page_size);
		_exit(0);
	}
	export.worker_socket = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
	if (export.worker_socket < 0)
		err(1, "accept worker");
	close(listener);
	require(read(export.worker_socket, &export.portid, sizeof(export.portid)) == sizeof(export.portid),
		"receive worker netlink portid");
	control = open(ROCE_MR_DEVICE, O_RDWR | O_CLOEXEC);
	buffer = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (control < 0 || buffer == MAP_FAILED)
		err(1, "open application MR resources");
	buffer[0] = 7;
	registration = (struct roce_mr_register) {
		.address = (uintptr_t)buffer, .length = page_size, .access = ROCE_MR_ACCESS_LOCAL_WRITE,
	};
	mapping = ioctl(control, ROCE_MR_REGISTER, &registration);
	if (mapping < 0 || ioctl(mapping, ROCE_MR_EXPORT_TICKET, &export))
		err(1, "export application MR over netlink");
	require(read(export.worker_socket, &ticket, sizeof(ticket)) == sizeof(ticket), "observe ticket");
	require(ioctl(mapping, ROCE_MR_EXPORT_TICKET, &export) == -1 && errno == EBUSY,
		"more than one outstanding ticket accepted for a mapping FD");
	require(ioctl(control, ROCE_MR_IMPORT_TICKET, &ticket) == -1 && errno == EACCES,
		"ticket authorized a different process with the same UID");
	require(send(export.worker_socket, "G", 1, 0) == 1 &&
		read(export.worker_socket, &ack, 1) == 1 && ack == 'W' && buffer[0] == 11,
		"authorized worker write was not visible to the application");
	export.cookie = 42;
	if (ioctl(mapping, ROCE_MR_EXPORT_TICKET, &export))
		err(1, "re-export after consuming first ticket");
	require(read(export.worker_socket, &ticket, sizeof(ticket)) == sizeof(ticket) &&
		close(mapping) == 0 && send(export.worker_socket, "G", 1, 0) == 1,
		"cancel ticket by closing exporting descriptor");
	require(waitpid(worker, &status, 0) == worker && WIFEXITED(status) && WEXITSTATUS(status) == 0,
		"worker ticket checks failed");
	puts("Netlink MR delivery, process authorization, replay and close cancellation verified");
	return 0;
}
