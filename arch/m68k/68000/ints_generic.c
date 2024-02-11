/*
 * ints.c - Generic interrupt controller support
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file COPYING in the main directory of this archive
 * for more details.
 *
 * Copyright 1996 Roman Zippel
 * Copyright 1999 D. Jeff Dionne <jeff@rt-control.com>
 */

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/cpu.h>
#include <asm/traps.h>
#include <asm/io.h>
#include <asm/machdep.h>

#include "ints.h"

//new
#include <linux/irqchip.h>

/* assembler routines */
asmlinkage void buserr(void);
asmlinkage irqreturn_t bad_interrupt(int, void *);

/* Processor exceptions */
asmlinkage irqreturn_t inthandler_cpuexception(void);
asmlinkage irqreturn_t inthandler2(void);
asmlinkage irqreturn_t inthandler3(void);
asmlinkage irqreturn_t inthandler4(void);
asmlinkage irqreturn_t inthandler5(void);
asmlinkage irqreturn_t inthandler6(void);
asmlinkage irqreturn_t inthandler7(void);
asmlinkage irqreturn_t inthandler8(void);
/* For auto vectors */
asmlinkage irqreturn_t inthandler_autovec(void);
asmlinkage irqreturn_t inthandler25(void);
asmlinkage irqreturn_t inthandler26(void);
asmlinkage irqreturn_t inthandler27(void);
asmlinkage irqreturn_t inthandler28(void);
asmlinkage irqreturn_t inthandler29(void);
asmlinkage irqreturn_t inthandler30(void);
asmlinkage irqreturn_t inthandler31(void);

/* Traps */
asmlinkage void system_call(void);
asmlinkage void system_call_68000(void);
asmlinkage irqreturn_t inthandler_badtrap(void);
asmlinkage irqreturn_t inthandler33(void);

asmlinkage irqreturn_t inthandler65(void);
asmlinkage irqreturn_t inthandler66(void);
asmlinkage irqreturn_t inthandler67(void);
asmlinkage irqreturn_t inthandler68(void);
asmlinkage irqreturn_t inthandler69(void);
asmlinkage irqreturn_t inthandler70(void);
asmlinkage irqreturn_t inthandler71(void);

/*
 * This function should be called during kernel startup to initialize
 * the machine vector table.
 */

#define VEC_PRIV 8

void __init trap_init(void)
{
	/* set up the vectors */
	for (int i = 72; i < 256; ++i)
		_ramvec[i] = (e_vector) bad_interrupt;

	/* ??? */
	_ramvec[65] = (e_vector) inthandler65;
	_ramvec[66] = (e_vector) inthandler66;
	_ramvec[67] = (e_vector) inthandler67;
	_ramvec[68] = (e_vector) inthandler68;
	_ramvec[69] = (e_vector) inthandler69;
	_ramvec[70] = (e_vector) inthandler70;
	_ramvec[71] = (e_vector) inthandler71;
}

asmlinkage void process_int_badtrap(struct pt_regs *fp)
{
	panic("Hit bad trap, NULL function pointer, bad stack?\n");
}

asmlinkage void process_int_oops(struct pt_regs *fp)
{
	int sig, si_code;

	switch (fp->vector) {
	case VEC_PRIV :
		si_code = ILL_PRVOPC;
		sig = SIGILL;
		break;
	}

	force_sig_fault(sig, si_code, 0);
}
static struct irq_domain *mc68000_irq_domain = NULL;

#define AUTOVECSTART 25
#define AUTOVECNUM 7

asmlinkage void process_int_autovec(struct pt_regs *fp)
{
	int irq = (fp->vector)/4;

	BUG_ON(irq < AUTOVECSTART || irq > (AUTOVECSTART + AUTOVECNUM));

	irq -= AUTOVECSTART;

	generic_handle_domain_irq(mc68000_irq_domain, irq);
}

void __init init_IRQ(void)
{
	irqchip_init();
}

#define MC68000_IRQ_NR 8

static struct irq_chip intc_irq_chip = {
	.name		= "mc680x0-intc-vect",
};

static int mc68000_intc_domain_map(struct irq_domain *domain,
		unsigned int irq, irq_hw_number_t hw)
{
	irq_set_chip_and_handler(irq, &intc_irq_chip, handle_simple_irq);
	irq_set_probe(irq);

	return 0;
}

static const struct irq_domain_ops mc68000_irq_domain_ops = {
	.xlate = irq_domain_xlate_onecell,
	.map = mc68000_intc_domain_map,
};

static int __init mc680x0_intc_of_init(struct device_node *dn,
				      struct device_node *parent)
{
	mc68000_irq_domain = irq_domain_add_simple(dn, MC68000_IRQ_NR,
			0,&mc68000_irq_domain_ops, NULL);

	if (!mc68000_irq_domain)
		return -ENODEV;

	return 0;
}

static int __init mc68010_intc_of_init(struct device_node *dn,
									   struct device_node *parent)
{
	_is68k = 0;

	_ramvec[VEC_SYS] = system_call;
	/* Catch bad stuff */
	for (int i = 2; i < 9; i++)
		_ramvec[i] = (e_vector) inthandler_cpuexception;

	/* Setup the handlers for auto vectored external interrupts */
	for (int i = 25; i < 32; i++)
		_ramvec[i] = (e_vector) inthandler_autovec;

	for (int i = 33; i < 48; i++)
		_ramvec[i] = (e_vector) inthandler_badtrap;

	return mc680x0_intc_of_init(dn, parent);
}
IRQCHIP_DECLARE(m68010_intc, "motorola,mc68010-intc-vect", mc68010_intc_of_init);

static int __init mc68000_intc_of_init(struct device_node *dn,
									   struct device_node *parent)
{
	_is68k = 1;

	_ramvec[VEC_SYS] = system_call_68000;
	/* Catch bad stuff */
	_ramvec[2] = (e_vector) inthandler2;
	_ramvec[3] = (e_vector) inthandler3;
	_ramvec[4] = (e_vector) inthandler4;
	_ramvec[5] = (e_vector) inthandler5;
	_ramvec[6] = (e_vector) inthandler6;
	_ramvec[7] = (e_vector) inthandler7;
	_ramvec[VEC_PRIV] = (e_vector) inthandler8;
	/* Setup the handlers for auto vectored external interrupts */
	_ramvec[25] = (e_vector) inthandler25;
	_ramvec[26] = (e_vector) inthandler26;
	_ramvec[27] = (e_vector) inthandler27;
	_ramvec[28] = (e_vector) inthandler28;
	_ramvec[29] = (e_vector) inthandler29;
	_ramvec[30] = (e_vector) inthandler30;
	_ramvec[31] = (e_vector) inthandler31;
	/* Traps */
	_ramvec[33] = (e_vector) inthandler33;

	return mc680x0_intc_of_init(dn, parent);
}
IRQCHIP_DECLARE(m68000_intc, "motorola,mc68000-intc-vect", mc68000_intc_of_init);

#define MC68000_IRQ_USER_NR 91
#define USERSTART 64

static struct irq_domain *mc68000_irq_user_domain = NULL;

asmlinkage void process_int_user(struct pt_regs *fp)
{
	int irq = (fp->vector)/4;

	BUG_ON(irq < USERSTART || irq > (USERSTART + MC68000_IRQ_USER_NR));

	irq -= USERSTART;

	generic_handle_domain_irq(mc68000_irq_user_domain, irq);
}

static struct irq_chip intc_user_irq_chip = {
	.name		= "mc680x0-intc-user",
};

static int mc68000_intc_user_domain_map(struct irq_domain *domain,
		unsigned int irq, irq_hw_number_t hw)
{
	irq_set_chip_and_handler(irq, &intc_user_irq_chip, handle_simple_irq);
	irq_set_probe(irq);

	return 0;
}

static const struct irq_domain_ops mc68000_irq_user_domain_ops = {
	.xlate = irq_domain_xlate_onecell,
	.map = mc68000_intc_user_domain_map,
};

static int __init mc680x0_intc_user_of_init(struct device_node *dn,
				      struct device_node *parent)
{
	mc68000_irq_user_domain = irq_domain_add_simple(dn, MC68000_IRQ_NR,
			0,&mc68000_irq_user_domain_ops, NULL);

	if (!mc68000_irq_user_domain)
		return -ENODEV;

	return 0;
}

static int __init mc68010_intc_user_of_init(struct device_node *dn,
									   struct device_node *parent)
{
	_is68k = 0;

	return mc680x0_intc_user_of_init(dn, parent);
}
IRQCHIP_DECLARE(m68010_intc_user, "motorola,mc68010-intc-user", mc68010_intc_user_of_init);

static int __init mc68000_intc_user_of_init(struct device_node *dn,
									   struct device_node *parent)
{
	_is68k = 1;

	return mc680x0_intc_user_of_init(dn, parent);
}
IRQCHIP_DECLARE(m68000_intc_user, "motorola,mc68000-intc-user", mc68000_intc_user_of_init);
