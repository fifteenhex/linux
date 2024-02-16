/*
 *
 */

#ifndef ARCH_M68K_LIB_MEM_H_
#define ARCH_M68K_LIB_MEM_H_

static inline void forward_aligned_src(void *dest, const void *src, size_t n) __attribute__((always_inline));
static inline void forward_aligned_src(void *dest, const void *src, size_t n)
{
	size_t temp;

	if ((long)dest & 1) {
		char *cdest = dest;
		const char *csrc = src;
		*cdest++ = *csrc++;
		dest = cdest;
		src = csrc;
		n--;
	}
	if (n > 2 && (long)dest & 2) {
		short *sdest = dest;
		const short *ssrc = src;
		*sdest++ = *ssrc++;
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
			*ldest++ = *lsrc++;
		while (temp--);
		dest = ldest;
		src = lsrc;
	}
	if (n & 2) {
		short *sdest = dest;
		const short *ssrc = src;
		*sdest++ = *ssrc++;
		dest = sdest;
		src = ssrc;
	}
	if (n & 1) {
		char *cdest = dest;
		const char *csrc = src;
		*cdest = *csrc;
	}
}

static inline void forward_fallback(void *dest, const void *src, size_t n) __attribute__((always_inline));
static inline void forward_fallback(void *dest, const void *src, size_t n)
{
	char *cdest = dest;
	const char *dsrc = src;

	for(; n > 0; n--)
		*cdest++ = *dsrc++;
}

#endif /* ARCH_M68K_LIB_MEM_H_ */
