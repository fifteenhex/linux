/* SPDX-License-Identifier: LGPL-2.1 OR MIT */
/*
 * sendfile for NOLIBC
 * Copyright (C) 2026 Daniel Palmer <daniel@thingy.jp>
 */

/* make sure to include all global symbols */
#include "../nolibc.h"

#ifndef _NOLIBC_SYS_SENDFILE_H
#define _NOLIBC_SYS_SENDFILE_H

#include "../sys.h"
#include <linux/unistd.h>

/*
 * ssize_t sendfile(int out_fd, int in_fd, off_t *offset, size_t count);
 */

static __attribute__((unused))
ssize_t _sys_sendfile(int out_fd, int in_fd, off_t *offset, size_t count)
{
#ifdef __NR_sendfile
	return __nolibc_syscall4(__NR_sendfile, out_fd, in_fd, offset, count);
#else
	return __nolibc_syscall4(__NR_sendfile64, out_fd, in_fd, offset, count);
#endif
}

static __attribute__((unused))
ssize_t sendfile(int out_fd, int in_fd, off_t *offset, size_t count)
{
	return __sysret(_sys_sendfile(out_fd, in_fd, offset, count));
}

#endif /* _NOLIBC_SYS_SENDFILE_H */
