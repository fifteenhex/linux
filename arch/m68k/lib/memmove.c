/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file COPYING in the main directory of this archive
 * for more details.
 */

#include <linux/module.h>
#include <linux/string.h>

#include "memmove.h"

static inline void backward_aligned_src(void *dest, const void *src, size_t n) __attribute__((always_inline));
static inline void backward_aligned_src(void *dest, const void *src, size_t n)
{
	size_t temp;

	if ((long)dest & 1) {
		char *cdest = dest;
		const char *csrc = src;
		*--cdest = *--csrc;
		dest = cdest;
		src = csrc;
		n--;
	}
	if (n > 2 && (long)dest & 2) {
		short *sdest = dest;
		const short *ssrc = src;
		*--sdest = *--ssrc;
		dest = sdest;
		src = ssrc;
		n -= 2;
	}
	temp = n >> 2;
	if (temp) {
		long *ldest = dest;
		const long *lsrc = src;
		temp--;
		do
			*--ldest = *--lsrc;
		while (temp--);
		dest = ldest;
		src = lsrc;
	}
	if (n & 2) {
		short *sdest = dest;
		const short *ssrc = src;
		*--sdest = *--ssrc;
		dest = sdest;
		src = ssrc;
	}
	if (n & 1) {
		char *cdest = dest;
		const char *csrc = src;
		*--cdest = *--csrc;
	}
}

static inline void backward_fallback(void *dest, const void *src, size_t n) __attribute__((always_inline));
static inline void backward_fallback(void *dest, const void *src, size_t n)
{
	char *cdest = dest;
	const char *csrc = src;

	for(; n > 0; n--)
		*--cdest = *--csrc;
}

void *memmove(void *dest, const void *src, size_t n)
{
	void *xdest = dest;

	if (!n)
		return xdest;

	if (dest < src) {
		if (WONKY(dest, src))
			forward_fallback(dest, src, n);
		else
			forward_aligned_src(dest, src, n);
	} else {
		dest = (char *)dest + n;
		src = (const char *)src + n;
		if (WONKY(dest, src))
			backward_fallback(dest, src, n);
		else
			backward_aligned_src(dest, src, n);
	}
	return xdest;
}
EXPORT_SYMBOL(memmove);
