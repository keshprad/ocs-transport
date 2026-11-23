#define _GNU_SOURCE

#include "roce_mr_uapi.h"

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

_Static_assert(sizeof(struct roce_mr_register) == 24, "registration ABI layout");
_Static_assert(sizeof(struct roce_mr_info) == 16, "information ABI layout");

static void require(int condition, const char *message)
{
	if (!condition)
		errx(1, "%s", message);
}

static int register_mr(int device, unsigned char *buffer, size_t length,
		       uint32_t access)
{
	struct roce_mr_register request = {
		.address = (uintptr_t)buffer, .length = length, .access = access,
	};
	struct roce_mr_info info;
	int fd = ioctl(device, ROCE_MR_REGISTER, &request);

	if (fd < 0)
		err(1, "register region");
	require(fcntl(fd, F_GETFD) == FD_CLOEXEC, "registration must return CLOEXEC");
	if (ioctl(fd, ROCE_MR_GET_INFO, &info))
		err(1, "get mapping info");
	require(info.length == length && info.access == access &&
		info.abi_version == ROCE_MR_ABI_VERSION, "incorrect mapping info");
	return fd;
}

static void check_read_only(int device, unsigned char *buffer, size_t length)
{
	struct roce_mr_register invalid = {
		.address = (uintptr_t)buffer + 1, .length = length,
	};
	int fd = register_mr(device, buffer, length, 0);
	const unsigned char *alias = mmap(NULL, length, PROT_READ, MAP_SHARED, fd, 0);

	if (alias == MAP_FAILED)
		err(1, "read-only mmap");
	require(alias[0] == 7 && alias[length - 1] == 8, "read-only alias has wrong bytes");
	require(mprotect((void *)alias, length, PROT_READ | PROT_WRITE) == -1 &&
		errno == EACCES, "read-only alias allowed write upgrade");
	require(mprotect((void *)alias, length, PROT_READ | PROT_EXEC) == -1 &&
		errno == EACCES, "mapping alias allowed execute upgrade");
	require(mmap(NULL, length, PROT_READ, MAP_PRIVATE, fd, 0) == MAP_FAILED,
		"mapping descriptor allowed a private mapping");
	require(ioctl(device, ROCE_MR_REGISTER, &invalid) == -1 && errno == EINVAL,
		"registration allowed an unaligned address");
	close(fd);
	require(alias[0] == 7, "closing descriptor invalidated a live mapping");
	munmap((void *)alias, length);
}

static int transfer_fd(int sock, int descriptor)
{
	union {
		struct cmsghdr align;
		char bytes[CMSG_SPACE(sizeof(int))];
	} control = {0};
	char byte = 'R';
	struct iovec iov = { .iov_base = &byte, .iov_len = 1 };
	struct msghdr message = {
		.msg_iov = &iov, .msg_iovlen = 1,
		.msg_control = control.bytes, .msg_controllen = sizeof(control.bytes),
	};
	struct cmsghdr *cmsg;

	if (descriptor >= 0) {
		cmsg = CMSG_FIRSTHDR(&message);
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;
		cmsg->cmsg_len = CMSG_LEN(sizeof(descriptor));
		memcpy(CMSG_DATA(cmsg), &descriptor, sizeof(descriptor));
		if (sendmsg(sock, &message, MSG_NOSIGNAL) != 1)
			err(1, "send mapping descriptor");
		return descriptor;
	}
	if (recvmsg(sock, &message, MSG_CMSG_CLOEXEC) != 1)
		err(1, "receive mapping descriptor");
	cmsg = CMSG_FIRSTHDR(&message);
	require(cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS &&
		cmsg->cmsg_len == CMSG_LEN(sizeof(descriptor)) && !(message.msg_flags & MSG_CTRUNC),
		"mapping descriptor message was malformed");
	memcpy(&descriptor, CMSG_DATA(cmsg), sizeof(descriptor));
	return descriptor;
}

static void check_child(int sock, size_t length)
{
	char byte;
	int fd = transfer_fd(sock, -1);
	unsigned char *alias = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

	if (alias == MAP_FAILED)
		err(1, "worker mmap");
	close(fd);
	require(alias[0] == 7 && alias[length - 1] == 8, "worker read the wrong pages");
	alias[0] = 11;
	alias[length - 1] = 12;
	require(write(sock, "W", 1) == 1 && read(sock, &byte, 1) == 1 && byte == 'D',
		"worker did not complete release handshake");
	require(alias[0] == 11 && alias[length - 1] == 12, "owner release invalidated worker pages");
	munmap(alias, length);
}

int main(void)
{
	long page_size = sysconf(_SC_PAGESIZE);
	size_t length;
	unsigned char *buffer;
	int device, fd, sock[2], status;
	pid_t child;
	char byte;

	require(page_size > 0, "could not determine host page size");
	length = (size_t)page_size * 2;
	device = open(ROCE_MR_DEVICE, O_RDWR | O_CLOEXEC);
	if (device < 0)
		err(1, "open " ROCE_MR_DEVICE);
	buffer = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (buffer == MAP_FAILED)
		err(1, "application mmap");
	buffer[0] = 7;
	buffer[length - 1] = 8;
	check_read_only(device, buffer, length);
	fd = register_mr(device, buffer, length, ROCE_MR_ACCESS_LOCAL_WRITE);
	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sock))
		err(1, "socketpair");
	child = fork();
	if (child < 0)
		err(1, "fork");
	if (!child) {
		close(sock[0]);
		close(device);
		close(fd);
		munmap(buffer, length);
		check_child(sock[1], length);
		return 0;
	}
	close(sock[1]);
	close(device);
	transfer_fd(sock[0], fd);
	close(fd);
	require(read(sock[0], &byte, 1) == 1 && byte == 'W', "worker did not write registered pages");
	require(buffer[0] == 11 && buffer[length - 1] == 12, "worker write did not reach application");
	munmap(buffer, length);
	require(write(sock[0], "D", 1) == 1, "could not release worker");
	close(sock[0]);
	require(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
		"worker failed");
	puts("Kernel registration, write permissions, and cross-process mapping lifetime verified");
	return 0;
}
