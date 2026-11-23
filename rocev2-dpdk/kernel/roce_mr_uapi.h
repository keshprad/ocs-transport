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

#endif
