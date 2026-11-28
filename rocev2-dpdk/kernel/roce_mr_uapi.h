#ifndef OCS_ROCE_MR_UAPI_H
#define OCS_ROCE_MR_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define ROCE_MR_DEVICE "/dev/roce_mr"
#define ROCE_MR_ABI_VERSION 1U
#define ROCE_MR_ACCESS_LOCAL_WRITE (1U << 0)

/* Address and nonzero length must be multiples of the host page size. */
struct roce_mr_register {
	__aligned_u64 address;
	__aligned_u64 length;
	__u32 access;
	__u32 reserved;
};

struct roce_mr_info {
	__aligned_u64 length;
	__u32 access;
	__u32 abi_version;
};

/* A successful registration returns a new close-on-exec mapping descriptor. */
#define ROCE_MR_REGISTER _IOW(0xB7, 0, struct roce_mr_register)
#define ROCE_MR_GET_INFO _IOR(0xB7, 1, struct roce_mr_info)
/* Export the mapping descriptor's pinned pages as a new CLOEXEC DMA-BUF fd. */
#define ROCE_MR_EXPORT_DMABUF _IO(0xB7, 2)

#define ROCE_MR_GENL_NAME "roce_mr"
#define ROCE_MR_GENL_VERSION 1U
#define ROCE_MR_GENL_CMD_TICKET 1U
#define ROCE_MR_GENL_A_TICKET 1U
#define ROCE_MR_GENL_A_COOKIE 2U
#define ROCE_MR_GENL_A_PAD 3U
#define ROCE_MR_TICKET_SIZE 16U

struct roce_mr_export_ticket {
	__s32 worker_socket;
	__u32 portid;
	__aligned_u64 cookie;
};

struct roce_mr_ticket {
	__u8 bytes[ROCE_MR_TICKET_SIZE];
};

/* Export on the mapping FD to the peer of a connected AF_UNIX socket. */
#define ROCE_MR_EXPORT_TICKET _IOW(0xB7, 3, struct roce_mr_export_ticket)
/* Import once on the control FD. Returns a CLOEXEC mapping descriptor. */
#define ROCE_MR_IMPORT_TICKET _IOW(0xB7, 4, struct roce_mr_ticket)

#endif
