/* SPDX-License-Identifier: LGPL-2.1 OR MIT */
/*
 * fcntl definition for NOLIBC
 * Copyright (C) 2017-2021 Willy Tarreau <w@1wt.eu>
 */

/* make sure to include all global symbols */
#include "nolibc.h"

#ifndef _NOLIBC_FCNTL_H
#define _NOLIBC_FCNTL_H

#include "arch.h"
#include "types.h"
#include "sys.h"

/* For fallocate() modes */
#include <linux/falloc.h>

#define __nolibc_open_flags(_flags) ((_flags) | O_LARGEFILE)

#define __nolibc_open_mode(_flags)							\
({											\
	mode_t _mode;									\
	va_list args;									\
											\
	va_start(args, (_flags));							\
	_mode = va_arg(args, mode_t);							\
	va_end(args);									\
											\
	_mode;										\
})

/*
 * int openat(int dirfd, const char *path, int flags[, mode_t mode]);
 */

static __attribute__((unused))
int _sys_openat(int dirfd, const char *path, int flags, mode_t mode)
{
	return __nolibc_syscall4(__NR_openat, dirfd, path, flags, mode);
}

static __attribute__((unused))
int openat(int dirfd, const char *path, int flags, ...)
{
	return __sysret(_sys_openat(dirfd, path, __nolibc_open_flags(flags),
						 __nolibc_open_mode(flags)));
}

/*
 * int open(const char *path, int flags[, mode_t mode]);
 */

static __attribute__((unused))
int _sys_open(const char *path, int flags, mode_t mode)
{
	return __nolibc_syscall4(__NR_openat, AT_FDCWD, path, flags, mode);
}

static __attribute__((unused))
int open(const char *path, int flags, ...)
{
	return __sysret(_sys_open(path, __nolibc_open_flags(flags), __nolibc_open_mode(flags)));
}

/*
 * int creat(const char *path, mode_t mode);
 */

static __attribute__((unused))
int creat(const char *path, mode_t mode)
{
	return open(path, O_CREAT | O_WRONLY | O_TRUNC, mode);
}

/*
 * int fallocate(int fd, int mode, off_t offset, off_t size);
 */

#if !defined(_sys_fallocate)
static __attribute__((unused))
int _sys_fallocate(int fd, int mode, off_t offset, off_t size)
{
	/*
	 * For 32 bit machines __kernel_long_t will be 4, off_t will be 8
	 * and we need to split offset and size, for 64 machines we can use
	 * the values as-is.
	 */
	if (sizeof(__kernel_long_t) != 8)
		return __nolibc_syscall6(__NR_fallocate, fd, mode,
			__NOLIBC_LLARGPART(offset, 0), __NOLIBC_LLARGPART(offset, 1),
			__NOLIBC_LLARGPART(size, 0), __NOLIBC_LLARGPART(size, 1));
	else
		return __nolibc_syscall4(__NR_fallocate, fd, mode, offset, size);
}
#endif

static __attribute__((unused))
int fallocate(int fd, int mode, off_t offset, off_t size)
{
	return __sysret(_sys_fallocate(fd, mode, offset, size));
}

#endif /* _NOLIBC_FCNTL_H */
