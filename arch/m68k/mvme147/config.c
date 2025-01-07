// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  arch/m68k/mvme147/config.c
 *
 *  Copyright (C) 1996 Dave Frascone [chaos@mindspring.com]
 *  Cloned from        Richard Hirst [richard@sleepie.demon.co.uk]
 *
 * Based on:
 *
 *  Copyright (C) 1993 Hamish Macdonald
 */

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/tty.h>
#include <linux/console.h>
#include <linux/linkage.h>
#include <linux/init.h>
#include <linux/major.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/rtc/m48t59.h>

#include <asm/bootinfo.h>
#include <asm/bootinfo-vme.h>
#include <asm/byteorder.h>
#include <asm/setup.h>
#include <asm/irq.h>
#include <asm/traps.h>
#include <asm/machdep.h>
#include <asm/mvme147hw.h>
#include <asm/config.h>

#include "mvme147.h"

static void mvme147_get_model(char *model);
extern void mvme147_reset (void);


int __init mvme147_parse_bootinfo(const struct bi_record *bi)
{
	uint16_t tag = be16_to_cpu(bi->tag);
	if (tag == BI_VME_TYPE || tag == BI_VME_BRDINFO)
		return 0;
	else
		return 1;
}

void mvme147_reset(void)
{
	pr_info("\r\n\nCalled mvme147_reset\r\n");
	m147_pcc->watchdog = 0x0a;	/* Clear timer */
	m147_pcc->watchdog = 0xa5;	/* Enable watchdog - 100ms to reset */
	while (1)
		;
}

static void mvme147_get_model(char *model)
{
	sprintf(model, "Motorola MVME147");
}

/*
 * This function is called during kernel startup to initialize
 * the mvme147 IRQ handling routines.
 */

static void __init mvme147_init_IRQ(void)
{
	m68k_setup_user_interrupt(VEC_USER, 192);
}

void __init config_mvme147(void)
{
	mach_init_IRQ		= mvme147_init_IRQ;
	mach_reset		= mvme147_reset;
	mach_get_model		= mvme147_get_model;

	/* Board type is only set by newer versions of vmelilo/tftplilo */
	if (!vme_brdtype)
		vme_brdtype = VME_TYPE_MVME147;
}

static struct resource m48t59_rsrc[] = {
	DEFINE_RES_MEM(MVME147_RTC_BASE, 0x800),
};

static struct m48t59_plat_data m48t59_data = {
	.type = M48T59RTC_TYPE_M48T02,
	.yy_offset = 70,
};

static int __init mvme147_platform_init(void)
{
	if (!MACH_IS_MVME147)
		return 0;

	platform_device_register_resndata(NULL, "rtc-m48t59", -1,
					  m48t59_rsrc, ARRAY_SIZE(m48t59_rsrc),
					  &m48t59_data, sizeof(m48t59_data));
	return 0;
}

arch_initcall(mvme147_platform_init);

static void scc_delay(void)
{
	__asm__ __volatile__ ("nop; nop;");
}

static void scc_write(char ch)
{
	do {
		scc_delay();
	} while (!(in_8(M147_SCC_A_ADDR) & BIT(2)));
	scc_delay();
	out_8(M147_SCC_A_ADDR, 8);
	scc_delay();
	out_8(M147_SCC_A_ADDR, ch);
}

void mvme147_scc_write(struct console *co, const char *str, unsigned int count)
{
	unsigned long flags;

	local_irq_save(flags);
	while (count--)	{
		if (*str == '\n')
			scc_write('\r');
		scc_write(*str++);
	}
	local_irq_restore(flags);
}
