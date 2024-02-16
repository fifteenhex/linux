/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file COPYING in the main directory of this archive
 * for more details.
 */

#include <linux/module.h>
#include <linux/string.h>

#include "memmove.h"

void *memcpy(void *to, const void *from, size_t n)
{
	void *xto = to;

	if (!n)
		return xto;

	if (WONKY(to, from))
		forward_fallback(to, from, n);
	else
		forward_aligned_src(to, from, n);

	return xto;
}
EXPORT_SYMBOL(memcpy);
