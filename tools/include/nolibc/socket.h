/* SPDX-License-Identifier: LGPL-2.1 OR MIT */
/*
 * socket function definitions for NOLIBC
 *
 * Copyright (C) 2025 Daniel Palmer<daniel@thingy.jp>
 */

/* make sure to include all global symbols */
#include "nolibc.h"

#ifndef _NOLIBC_SOCKET_H
#define _NOLIBC_SOCKET_H

#include "arch.h"
#include "sys.h"

#include <asm/socket.h>
#include <linux/in.h>
#define AF_INET     2

#define SOL_SOCKET   1
#define SO_REUSEADDR 2

/* Apparently mips has its own version of this?? */
enum sock_type {
	SOCK_STREAM	= 1,
	SOCK_DGRAM	= 2,
	SOCK_RAW	= 3,
	SOCK_RDM	= 4,
	SOCK_SEQPACKET	= 5,
	SOCK_DCCP	= 6,
	SOCK_PACKET	= 10,
};

struct sockaddr {
	uint16_t sa_family;
	char sa_data[14];
};

typedef uint32_t socklen_t;

static __inline__
uint16_t htons(uint16_t hostshort)
{
	return htobe16(hostshort);
}

static __inline__
uint32_t htonl(uint32_t hostlong)
{
	return htobe32(hostlong);
}

static __inline__
uint32_t ntohl(uint32_t netlong)
{
	return be32toh(netlong);
}

static __inline__
uint32_t ntohs(uint16_t netshort)
{
	return be16toh(netshort);
}

static __attribute__((unused))
int connect(int sockfd, struct sockaddr *addr, socklen_t addrlen)
{
	return __nolibc_syscall3(__NR_connect, sockfd, addr, addrlen);
}

static __attribute__((unused))
int sys_bind(int sockfd, struct sockaddr *addr, socklen_t addrlen)
{
	return __nolibc_syscall3(__NR_bind, sockfd, addr, addrlen);
}

static __attribute__((unused))
int bind(int sockfd, struct sockaddr *addr, socklen_t addrlen)
{
	return __sysret(sys_bind(sockfd, addr, addrlen));
}

static __attribute__((unused))
ssize_t sys_sendto(int socket, const void *message, size_t length,
           int flags, const struct sockaddr *dest_addr,
           socklen_t dest_len)

{
	return __nolibc_syscall6(__NR_sendto, socket, message, length, flags, dest_addr, dest_len);
}

static __attribute__((unused))
ssize_t sendto(int socket, const void *message, size_t length,
           int flags, const struct sockaddr *dest_addr,
           socklen_t dest_len)
{
	return __sysret(sys_sendto(socket, message, length, flags, dest_addr, dest_len));
}

static __attribute__((unused))
int sys_socket(int domain, int type, int protocol)
{
	return __nolibc_syscall3(__NR_socket, domain, type, protocol);
}

static __attribute__((unused))
int socket(int domain, int type, int protocol)
{
	return __sysret(sys_socket(domain, type, protocol));
}

static __attribute__((unused))
int listen(int sockfd, int backlog)
{
	return __nolibc_syscall2(__NR_listen, sockfd, backlog);
}

#ifdef __NR_accept
static __attribute__((unused))
int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
{
	return __nolibc_syscall3(__NR_accept, sockfd, addr, addrlen);
}
#endif

static __attribute__((unused))
int accept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags)
{
	return __nolibc_syscall4(__NR_accept4, sockfd, addr, addrlen, flags);
}

static __attribute__((unused))
int setsockopt(int sockfd, int level, int optname,
	       void *optval, socklen_t optlen)
{
	return __nolibc_syscall5(__NR_setsockopt, sockfd, level, optname, optval, optlen);
}

/* ssize_t recvfrom(int socket, void *restrict buffer, size_t length,
 *		    int flags, struct sockaddr *restrict address,
 *		    socklen_t *restrict address_len);
 */

static __attribute__((unused))
ssize_t sys_recvfrom(int socket, void *buffer, size_t length,
		     int flags, struct sockaddr *address,
		     socklen_t *address_len)
{
	return __nolibc_syscall6(__NR_recvfrom, socket, buffer, length, flags, address, address_len);
}

static __attribute__((unused))
ssize_t recvfrom(int socket, void *buffer, size_t length,
           int flags, struct sockaddr *address,
           socklen_t *address_len)
{
	return __sysret(sys_recvfrom(socket, buffer, length, flags, address, address_len));
}

/* ssize_t recv(int sockfd, void buf[size], size_t size,
 *              int flags);
 */

static __attribute__((unused))
ssize_t recv(int sockfd, void *buf, size_t size, int flags)
{
	return __sysret(sys_recvfrom(sockfd, buf, size, flags, NULL, NULL));
}

#endif
