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

struct mstar_ge_job {
	enum mstar_ge_op op;
	dma_addr_t src_addr, dst_addr;
	u32 src_width, src_height, dst_width, dst_height;
	u32 src_pitch, dst_pitch;
	u32 src_fourcc, dst_fourcc;
	u32 tag;
	union {
		struct mstar_ge_bitblt bitblt;
	};
	struct list_head queue;
};

static inline void mstar_ge_job_init(struct mstar_ge_job *job)
{
	INIT_LIST_HEAD(&job->queue);
}

#define mstar_ge_job_srcsz(j) (j->src_pitch * j->src_height)
#define mstar_ge_job_dstsz(j) (j->src_pitch * j->src_height)

int mstar_ge_queue_job(struct mstar_ge *ge, struct mstar_ge_job *job);

#endif /* _MSTAR_GE_H_ */
