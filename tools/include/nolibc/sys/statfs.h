/* SPDX-License-Identifier: LGPL-2.1 OR MIT */
/*
 * statfs for NOLIBC
 * Copyright (C) 2026 Daniel Palmer <daniel@thingy.jp>
 */

/* make sure to include all global symbols */
#include "../nolibc.h"

#ifndef _NOLIBC_SYS_STATFS_H
#define _NOLIBC_SYS_STATFS_H

#include "../sys.h"

/* Some preprocessor hackery to get the kernel statfs
 * types and add the nolibc prefix
 */
#define statfs __nolibc_kernel_statfs
#define statfs64 __nolibc_kernel_statfs64
#define compat_statfs64 __nolibc_kernel_compat_statfs64
#include <asm/statfs.h>
#undef statfs
#undef statfs64
#undef compat_statfs64

/* Never use with system calls */
struct statfs {
	uint32_t f_type;
	uint32_t f_bsize;
	uint64_t f_blocks;
	uint64_t f_bfree;
	uint64_t f_bavail;
	uint64_t f_files;
	uint64_t f_ffree;
/*	fsid_t f_fsid; */
	uint32_t f_namelen;
	uint32_t f_frsize;
	uint32_t f_flags;
};

/*
 * statfs(const char *path, struct statfs *buf);
 */

static __attribute__((unused))
int _sys_statfs(const char *path, struct statfs *buf)
{
#ifdef __NR_statfs64
	struct __nolibc_kernel_statfs64 _buf;
#else
	struct __nolibc_kernel_statfs _buf;
#endif
	int ret;

#ifdef __NR_statfs64
	ret = __nolibc_syscall3(__NR_statfs64, path, sizeof(_buf), &_buf);
#else

	ret = __nolibc_syscall2(__NR_statfs, path, &_buf);
#endif

	if (!ret) {
		buf->f_type    = _buf.f_type;
		buf->f_bsize   = _buf.f_bsize;
		buf->f_blocks  = _buf.f_blocks;
		buf->f_bfree   = _buf.f_bfree;
		buf->f_bavail  = _buf.f_bavail;
		buf->f_files   = _buf.f_files;
		buf->f_ffree   = _buf.f_ffree;
/*		buf->f_fsid    = _buf.f_fsid; */
		buf->f_namelen = _buf.f_frsize;
		buf->f_flags   = _buf.f_flags;
	}

	return ret;
}

static __attribute__((unused))
int statfs(const char *path, struct statfs *buf)
{
	return __sysret(_sys_statfs(path, buf));
}

#endif /* _NOLIBC_SYS_STATFS_H */
