/* SPDX-License-Identifier: GPL-2.0-only */

#include <linux/linkage.h>

struct pt_regs;

asmlinkage void process_int(struct pt_regs *fp);
