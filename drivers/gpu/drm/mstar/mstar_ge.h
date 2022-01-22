// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef _MSTAR_GE_H_
#define _MSTAR_GE_H_

struct mstar_ge;

enum mstar_ge_op {
	MSTAR_GE_OP_BITBLT,
};

#define ROTATION_0	0
#define ROTATION_90	1
#define ROTATION_180	2
#define ROTATION_270	3

struct mstar_ge_bitblt {
	unsigned int rotation;
};

#define MSTAR_GE_IOCTL_INFO	0

struct mstar_ge_info {
	u32 caps;
};

struct mstar_ge_opdata {
	enum mstar_ge_op op;
	void *src, *dst;
	u32 src_width, src_height, dst_width, dst_height;
	u32 src_pitch, dst_pitch;
	u32 src_fourcc, dst_fourcc;
	union {
		struct mstar_ge_bitblt bitblt;
	};
};

struct mstar_ge_job {
	u32 tag;
	struct mstar_ge_opdata opdata;
	dma_addr_t src_addr, dst_addr;
	struct list_head queue;
};

static inline void mstar_ge_job_init(struct mstar_ge_job *job)
{
	INIT_LIST_HEAD(&job->queue);
}

#define mstar_ge_job_srcsz(j) (j->opdata.src_pitch * j->opdata.src_height)
#define mstar_ge_job_dstsz(j) (j->opdata.dst_pitch * j->opdata.dst_height)

int mstar_ge_queue_job(struct mstar_ge *ge, struct mstar_ge_job *job);

#endif /* _MSTAR_GE_H_ */
