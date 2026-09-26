/*
 * mstar_ge.h
 */

#ifndef INCLUDE_UAPI_DRM_MSTAR_GE_H_
#define INCLUDE_UAPI_DRM_MSTAR_GE_H_

/*
 * Get driver info from the kernel side, takes a pointer
 * to a struct mstar_ge_info.
 */
#define MSTAR_GE_IOCTL_INFO	0
/*
 * Queue a job into the GE queue, takes a pointer to a
 * struct mstar_ge_job_request.
 */
#define MSTAR_GE_IOCTL_QUEUE	1
/*
 * Query the status of a job, takes a job tag
 */
#define MSTAR_GE_IOCTL_QUERY	2

/*
 * Persistent buffer registration (ABI v2).
 *
 * The classic MSTAR_GE_IOCTL_QUEUE path attaches + maps + unmaps + detaches
 * both PRIME dma-bufs on every call, which dominates the per-op cost for the
 * small, long-lived surfaces DirectFB re-blits every frame. Registering a
 * buffer maps it ONCE (dma_buf_attach + dma_buf_map_attachment, bidirectional)
 * and hands back an opaque handle; MSTAR_GE_IOCTL_QUEUE2 then references the
 * handle so the hot path does no dma-buf work at all.
 *
 * Handles are scoped to the open file description and are dropped when it is
 * closed. Registering the same dma-buf (by identity, not fd number) twice
 * returns the same handle and takes an extra reference, so a caller can
 * register/unregister symmetrically without worrying about aliasing.
 *
 * MSTAR_GE_IOCTL_REGISTER_BUFFER   - takes a struct mstar_ge_buf_reg *
 * MSTAR_GE_IOCTL_UNREGISTER_BUFFER - takes a __u32 * (the handle)
 * MSTAR_GE_IOCTL_QUEUE2            - takes a struct mstar_ge_job_request2 *
 */
#define MSTAR_GE_IOCTL_REGISTER_BUFFER		3
#define MSTAR_GE_IOCTL_UNREGISTER_BUFFER	4
#define MSTAR_GE_IOCTL_QUEUE2			5

/*
 * Pre-compiled jobs (ABI v3).
 *
 * MSTAR_GE_IOCTL_COMPILE_JOB takes the same job description as QUEUE2 (an op
 * array plus registered buffer handles), runs all of the per-op derivation
 * and register value computation ONCE in the kernel, stores the resulting
 * register program keyed by a token and returns the token. FIRE_JOB replays
 * a stored program to the engine without re-deriving anything and without
 * touching the dma-bufs, blocking until it completes just like QUEUE/QUEUE2.
 * FREE_JOB drops a compiled job again.
 *
 * Tokens are scoped to the open file description and freed automatically on
 * close. A compiled job pins the registered buffers it references (their
 * device addresses are baked into the program): they stay mapped until the
 * job is freed, even if the caller unregisters its own handle first. At most
 * MSTAR_GE_MAX_COMPILED_JOBS compiled jobs may be live per file description;
 * COMPILE_JOB fails with EMFILE beyond that.
 *
 * Availability is signalled by MSTAR_GE_CAP_COMPILED_JOBS in
 * mstar_ge_info.caps; older kernels fail these ioctls with EINVAL.
 *
 * MSTAR_GE_IOCTL_COMPILE_JOB - takes a struct mstar_ge_compile_request *
 * MSTAR_GE_IOCTL_FIRE_JOB    - takes a struct mstar_ge_fire_request *
 * MSTAR_GE_IOCTL_FREE_JOB    - takes a __u32 * (the token)
 */
#define MSTAR_GE_IOCTL_COMPILE_JOB		6
#define MSTAR_GE_IOCTL_FIRE_JOB			7
#define MSTAR_GE_IOCTL_FREE_JOB			8

/*
 * ABI level of this header: 1 = INFO/QUEUE/QUERY, 2 = persistent buffer
 * registration + QUEUE2, 3 = pre-compiled jobs. Runtime feature discovery
 * should use mstar_ge_info.caps, not this constant.
 */
#define MSTAR_GE_ABI_VERSION			3

/* mstar_ge_info.caps bits */
#define MSTAR_GE_CAP_QUEUE			(1 << 0) /* always set (v1 reported caps == 1) */
#define MSTAR_GE_CAP_BUFFER_REGISTRATION	(1 << 1) /* ABI v2: ioctls 3/4/5 */
#define MSTAR_GE_CAP_COMPILED_JOBS		(1 << 2) /* ABI v3: ioctls 6/7/8 */


#define MSTAR_GE_ROTATION_0	(1 << 0)
#define MSTAR_GE_ROTATION_90	(1 << 1)
#define MSTAR_GE_ROTATION_180	(1 << 2)
#define MSTAR_GE_ROTATION_270	(1 << 3)
#define MSTAR_GE_ROTATION_MASK	0xf

#define MSTAR_GE_FLIP_SRC_V	(1 << 4)
#define MSTAR_GE_FLIP_DST_H	(1 << 5)
#define MSTAR_GE_FLIP_DST_V	(1 << 6)

/*
 * Do a plain (opaque) copy for a BITBLT instead of blending the source over
 * the destination with the source alpha channel. Without this the engine is
 * always programmed to alpha-blend, which is only equivalent to a copy when
 * the source has no alpha (e.g. RGB565) or is fully opaque; an opaque copy of
 * an ARGB/XRGB surface whose alpha isn't 0xff would otherwise be corrupted.
 * Also valid on a STRBLT: it forces the stretch to be an opaque copy instead
 * of inheriting the blend state of the previous blit.
 */
#define MSTAR_GE_BLIT_NO_BLEND	(1 << 7)

/*
 * BITBLT only: enable source colour keying. Source pixels that exactly match
 * mstar_ge_bitblt.colorkey (a raw source-format pixel value: RGB565 in bits
 * [15:0], ARGB8888 in [31:0]) leave the destination pixel untouched.
 * Usually combined with MSTAR_GE_BLIT_NO_BLEND for a keyed opaque copy.
 */
#define MSTAR_GE_BLIT_SRC_COLORKEY	(1 << 8)

/*
 * STRBLT only: use nearest-neighbour sampling for the scale instead of the
 * default bilinear filter.
 */
#define MSTAR_GE_STRBLT_NEAREST		(1 << 8)

/*
 * RECTFILL_GRADIENT flags: which axes interpolate. The colour starts at
 * start_argb on the first pixel of an enabled axis (left column for H, top
 * row for V) and reaches end_argb on the last pixel of that axis. When both
 * are set each axis independently walks the full start->end delta. The
 * per-pixel-step colour increment programmed into the hardware is
 * trunc((end - start) << 12 / (x1 - x0)) for R/G/B (s7.12) and
 * trunc((end - start) << 11 / (x1 - x0)) for A (s4.11) (y1 - y0 for the
 * vertical deltas); the hardware accumulates the increment per pixel step
 * and uses the integer part. At least one axis must be enabled.
 */
#define MSTAR_GE_RECTFILL_GRADIENT_H	(1 << 0)
#define MSTAR_GE_RECTFILL_GRADIENT_V	(1 << 1)

struct mstar_ge_info {
	__u32 caps;
};

/* The desired operation */
enum mstar_ge_op {
	MSTAR_GE_OP_INVALID,
	MSTAR_GE_OP_LINE,
	MSTAR_GE_OP_RECTFILL,
	MSTAR_GE_OP_BITBLT,
	MSTAR_GE_OP_STRBLT,
	MSTAR_GE_OP_RECTFILL_GRADIENT,
};

struct mstar_ge_color {
	unsigned int r, g, b, a;
};

/* Extra parameters for a LINE */
struct mstar_ge_line_params {
	unsigned int x0, y0, x1, y1;
	struct mstar_ge_color start_color;
};

/* Extra parameters for a RECTFILL */
struct mstar_ge_rectfill_params {
	unsigned int x0, y0, x1, y1;
	struct mstar_ge_color start_color;
};

/*
 * Extra parameters for a RECTFILL_GRADIENT.
 *
 * This is deliberately a separate op with packed colours instead of extra
 * fields on mstar_ge_rectfill_params: it must not grow the mstar_ge_opdata
 * union (binaries built against older headers would then pass a different
 * op stride) and old RECTFILL callers don't zero the union tail, so a flags
 * field appended to the old struct could contain garbage.
 */
struct mstar_ge_rectfill_gradient_params {
	unsigned int x0, y0, x1, y1;
	/* colours packed as A << 24 | R << 16 | G << 8 | B */
	__u32 start_argb;
	__u32 end_argb;
	/* MSTAR_GE_RECTFILL_GRADIENT_* - at least one axis */
	__u32 flags;
};

/* Extra parameters for a BITBLT */
struct mstar_ge_bitblt {
	/* top left corner of the src */
	__u32 src_x0, src_y0;
	/* top left corner of the dst */
	__u32 dst_x0, dst_y0;
	/* bottom right corner of the src */
	__u32 dst_x1, dst_y1;

	__u32 flags;

	/*
	 * Source colour key as a raw source-format pixel value; only used
	 * when MSTAR_GE_BLIT_SRC_COLORKEY is set in flags.
	 */
	__u32 colorkey;
};

/* Extra parameters for a STRBLT */
struct mstar_ge_strblt {
	/* top left corner of the src */
	__u32 src_x0, src_y0;
	/* bottom right corner of the src */
	__u32 src_x1, src_y1;
	/* top left corner of the dst */
	__u32 dst_x0, dst_y0;
	/* bottom right corner of the src */
	__u32 dst_x1, dst_y1;

	__u32 flags;
};

struct mstar_ge_buf_cfg {
	__u32 width;
	__u32 height;
	__u32 pitch;
	__u32 fourcc;
};

struct mstar_ge_buf {
	union {
		/* PRIME fd of the buffer - valid for userspace callers */
		int fd;
		/* pointer to the buffer if allocated inside the kernel - not valid for userspace callers */
		void *buf;
	};
	struct mstar_ge_buf_cfg cfg;
};

struct mstar_ge_opdata {
	enum mstar_ge_op op;
	union {
		struct mstar_ge_line_params line;
		struct mstar_ge_rectfill_params rectfill;
		struct mstar_ge_rectfill_gradient_params rectfill_gradient;
		struct mstar_ge_bitblt bitblt;
		struct mstar_ge_strblt strblt;
	};
};

#define MSTAR_GE_MAX_JOBS	32

struct mstar_ge_job_request {
	/*
	 * Address to write the job tag into.
	 * Tag is used to track the progress of your
	 * job.
	 */
	unsigned long *tag;

	/* pointers to the operation(s) you want performed */
	const struct mstar_ge_opdata *ops;
	int num_ops;

	/*
	 * pointers to the buffer(s) you want the operations
	 * to happen on.
	 * You need at least for drawing, and 2 for blitting.
	 */
	const struct mstar_ge_buf *bufs;
	int num_bufs;
};

/*
 * ABI v2 - persistent buffer registration.
 *
 * These are laid out to stay identical between the 32-bit userspace and the
 * 32-bit kernel this driver runs under (SSD202D); the older QUEUE ABI already
 * embeds bare pointers/longs the same way, so v2 follows suit rather than
 * inventing a compat layer.
 */

/* MSTAR_GE_IOCTL_REGISTER_BUFFER argument */
struct mstar_ge_buf_reg {
	/* in: PRIME dma-buf fd to map persistently */
	__s32 fd;
	/* out: opaque handle for QUEUE2 / UNREGISTER (never 0 on success) */
	__u32 handle;
};

/* A buffer for QUEUE2, referenced by a handle from REGISTER_BUFFER */
struct mstar_ge_buf2 {
	__u32 handle;
	__u32 _pad;
	struct mstar_ge_buf_cfg cfg;
};

/* QUEUE2 flags (reserved, must be zero for now) */
#define MSTAR_GE_QUEUE2_FLAGS_ALL	0u

struct mstar_ge_job_request2 {
	/*
	 * Address to write the job tag into (see mstar_ge_job_request). May be
	 * NULL if the caller does not care about the tag.
	 */
	unsigned long *tag;

	/* operation(s) to perform */
	const struct mstar_ge_opdata *ops;
	__u32 num_ops;

	/* registered buffer(s): 1 for a fill/draw, 2 for a blit (src, dst) */
	__u32 num_bufs;
	const struct mstar_ge_buf2 *bufs;

	/* MSTAR_GE_QUEUE2_FLAGS_* - reserved */
	__u32 flags;
	__u32 _pad;
};

/*
 * ABI v3 - pre-compiled jobs. Laid out with bare pointers like the QUEUE/
 * QUEUE2 requests (32-bit userspace on the 32-bit SSD202D kernel).
 */

/* Cap on live compiled jobs per open file description */
#define MSTAR_GE_MAX_COMPILED_JOBS	64

/* MSTAR_GE_IOCTL_COMPILE_JOB argument */
struct mstar_ge_compile_request {
	/* in: operation(s) to compile, 1..MSTAR_GE_MAX_JOBS */
	const struct mstar_ge_opdata *ops;
	__u32 num_ops;

	/*
	 * in: registered buffer(s) the compiled job will operate on:
	 * 1 for a fill/draw (dst), 2 for a blit (src, dst). Same semantics
	 * as QUEUE2, but the job keeps a reference on the buffers until it
	 * is freed.
	 */
	__u32 num_bufs;
	const struct mstar_ge_buf2 *bufs;

	/* in: must be zero */
	__u32 flags;

	/* out: token identifying the compiled job (never 0 on success) */
	__u32 token;
};

/* MSTAR_GE_IOCTL_FIRE_JOB argument */
struct mstar_ge_fire_request {
	/* in: token from MSTAR_GE_IOCTL_COMPILE_JOB */
	__u32 token;
	/*
	 * in: must be zero. Reserved, e.g. for re-binding the registered
	 * buffers a compiled job references at fire time.
	 */
	__u32 flags;
};
#endif /* INCLUDE_UAPI_DRM_MSTAR_GE_H_ */
