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
	size_t temp;
	/*
	 * Alignment of src and dest differ so it's impossible
	 * to do a few byte or word operations at first then
	 * switch to longs as one of the pointers will still
	 * be misaligned.
	 */
	bool wonky = (((long) to) & 1) + (((long) from) & 1) == 1;

	if (!n)
		return xto;

	if (wonky)
		forward_fallback(to, from, n);
	else
		forward_aligned_src(to, from, n);

	return xto;
}
EXPORT_SYMBOL(memcpy);
