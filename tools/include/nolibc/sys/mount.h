/* SPDX-License-Identifier: LGPL-2.1 OR MIT */
/*
 * Mount definitions for NOLIBC
 * Copyright (C) 2017-2021 Willy Tarreau <w@1wt.eu>
 */

/* make sure to include all global symbols */
#include "../nolibc.h"

#ifndef _NOLIBC_SYS_MOUNT_H
#define _NOLIBC_SYS_MOUNT_H

#include "../sys.h"

#include <linux/mount.h>

/*
 * int mount(const char *source, const char *target,
 *           const char *fstype, unsigned long flags,
 *           const void *data);
 */
static __attribute__((unused))
int _sys_mount(const char *src, const char *tgt, const char *fst,
	       unsigned long flags, const void *data)
{
	return __nolibc_syscall5(__NR_mount, src, tgt, fst, flags, data);
}

static __attribute__((unused))
int mount(const char *src, const char *tgt,
	  const char *fst, unsigned long flags,
	  const void *data)
{
	return __sysret(_sys_mount(src, tgt, fst, flags, data));
}

/* New mount API available from kernel 5.2
 */

/*
 * int fsopen(const char *fsname, unsigned int flags);
 */
static __attribute__((unused))
int _sys_fsopen(const char *fsname, unsigned int flags)
{
	return __nolibc_syscall2(__NR_fsopen, fsname, flags);
}

static __attribute__((unused))
int fsopen(const char *fsname, unsigned int flags)
{
	return __sysret(_sys_fsopen(fsname, flags));
}

/*
 * int fsconfig(int fd, unsigned int cmd,
 *              const char *key, const void *val, int aux);
 */
static __attribute__((unused))
int _sys_fsconfig(int fd, unsigned int cmd,
		  const char *key, const void *val, int aux)
{
	return __nolibc_syscall5(__NR_fsconfig, fd, cmd, key, val, aux);
}

static __attribute__((unused))
int fsconfig(int fd, unsigned int cmd,
	     const char *key, const void *val, int aux)
{
	return __sysret(_sys_fsconfig(fd, cmd, key, val, aux));
}

/*
 * int fsmount(int fd, unsigned int flags, unsigned int attr_flags);
 */
static __attribute__((unused))
int _sys_fsmount(int fd, unsigned int flags, unsigned int attr_flags)
{
	return __nolibc_syscall3(__NR_fsmount, fd, flags, attr_flags);
}

static __attribute__((unused))
int fsmount(int fd, unsigned int flags, unsigned int attr_flags)
{
	return __sysret(_sys_fsmount(fd, flags, attr_flags));
}

/*
 * int move_mount(int from_dfd, const char *from_path,
 *               int to_dfd,   const char *to_path,
 *               unsigned int flags);
 */

static __attribute__((unused))
int _sys_move_mount(int from_dfd, const char *from_path,
		    int to_dfd,   const char *to_path,
		    unsigned int flags)
{
	return __nolibc_syscall5(__NR_move_mount,
			from_dfd, from_path, to_dfd, to_path, flags);
}

static __attribute__((unused))
int move_mount(int from_dfd, const char *from_path,
	       int to_dfd,   const char *to_path,
	       unsigned int flags)
{
	return __sysret(_sys_move_mount(from_dfd, from_path,
				       to_dfd, to_path, flags));
}

/*
 * int fspick(int dfd, const char *path, unsigned int flags);
 */
static __attribute__((unused))
int _sys_fspick(int dfd, const char *path, unsigned int flags)
{
	return __nolibc_syscall3(__NR_fspick, dfd, path, flags);
}

static __attribute__((unused))
int fspick(int dfd, const char *path, unsigned int flags)
{
	return __sysret(_sys_fspick(dfd, path, flags));
}

#endif /* _NOLIBC_SYS_MOUNT_H */
