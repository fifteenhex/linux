/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/types.h>

void config_eltec_e17(void);
int eltec_e17_parse_bootinfo(const struct bi_record *record);
bool e17_breadcrumb_prev(u8 out[4]);

#ifdef CONFIG_SMP
void e17_smp_init_cpus(void);
#endif
