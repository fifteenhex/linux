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

/* Some preprocessor hackery to get struct statfs to
 * always be the 64bit one.
 */
#define statfs __nolibc_kernel_statfs
#define statfs64 __nolibc_kernel_statfs64
#include <asm/statfs.h>
#undef statfs
#undef statfs64

#ifdef __NR_statfs64
#define statfs __nolibc_kernel_statfs64
#else
#define statfs __nolibc_kernel_statfs
#endif

/*
 * statfs(const char *path, struct statfs *buf);
 */

static __attribute__((unused))
int _sys_statfs(const char *path, struct statfs *buf)
{
#ifdef __NR_statfs64
	return __nolibc_syscall3(__NR_statfs64, path, sizeof(*buf), buf);
#else
	return __nolibc_syscall2(__NR_statfs, path, buf);
#endif
}

static __attribute__((unused))
int statfs(const char *path, struct statfs *buf)
{
	return __sysret(_sys_statfs(path, buf));
}

#endif /* _NOLIBC_SYS_STATFS_H */
