/* SPDX-License-Identifier: GPL-2.0-only */

#include <linux/linkage.h>

struct pt_regs;

asmlinkage void bad_trap(struct pt_regs *fp);
asmlinkage void process_int(struct pt_regs *fp);
asmlinkage void process_int_oops(struct pt_regs *fp);
asmlinkage void process_int_autovec(struct pt_regs *fp);
