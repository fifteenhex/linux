// SPDX-License-Identifier: GPL-2.0
/*
 * VIC068A interrupt controller for the ELTEC EUROCOM-17 (m68k dual-68040 VME).
 *
 * The VIC probes, maps its register block and creates an irq_domain that
 * cascades off the generic m68k user-vector controller (intc_user).  Devices
 * are migrated onto this domain one at a time so a mistake can't take out
 * boot/console in one go.  Migrated so far: ethernet (LIRQ7), keyboard (LIRQ1)
 * and the CD2401 console (LIRQ6, self-vectored).  Everything else still routes
 * through intc_user directly until its stage lands.
 *
 * Background: on this board the VIC's local inputs LIRQ1..LIRQ7 (keyboard,
 * timer, SCSI, video, LEB, CD2401 daisy-chain, ILACC ethernet) are delivered to
 * the CPU as m68k vectored interrupts.  The VIC's per-input control registers
 * LICRn (0x27 + (n-1)*4) carry a mask bit (0x80), a level field (bits 2:0) and a
 * read-only raw pin STATE bit (0x08).  Masking/acking a device IRQ therefore
 * means poking its LICRn -- which today is open-coded in config.c and the CD2401
 * driver.  This driver will centralise that behind irq_chip ops.
 *
 * m68k has no GENERIC_IRQ_MULTI_HANDLER and does not use of_irq_init(); like the
 * user-vector controller (arch/m68k/kernel/ints.c) we create our domain from an
 * initcall rather than IRQCHIP_DECLARE.
 */
#include <linux/init.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/irqdomain.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/printk.h>
#include <linux/spinlock.h>
#include <asm/irq.h>		/* IRQ_USER  */
#include <asm/traps.h>		/* VEC_USER  */

/* VIC068A local-interrupt-control registers, byte-wide, within the reg block. */
#define E17_VIC_LICR(lirq)	(0x27 + ((lirq) - 1) * 4)	/* LIRQ1..LIRQ7 */
#define E17_VIC_LICR_MASK	0x80	/* 1 = masked                        */
#define E17_VIC_LICR_VEC	0x30	/* sense bits: VIC supplies LIVBR|n   */
#define E17_VIC_LICR_STATE	0x08	/* r/o: raw device pin level         */
#define E17_VIC_LICR_LEVEL(l)	((l) & 0x07)

#define E17_VIC_NR_LIRQ		8	/* LIRQ0 unused; LIRQ1..LIRQ7        */

/*
 * Domain hwirq == CPU vector offset (vector - VEC_USER).  For a VIC-vectored
 * line that equals the LIRQ number (vector = LIVBR|lirq, LIVBR = VEC_USER); for
 * a self-vectored line (the CD2401 + CIO daisy chain on LIRQ6, which supply
 * their own vectors) it is the device vector's offset, so one LICR6 can gate
 * several hwirqs (CD2401 rxexc/modem/tx/rx = 0x50..0x53, the CIOs' CT vectors =
 * 0x60/0x68).  Sized to cover the whole user-vector window the VIC drives
 * (0x40..0x6f).
 */
#define E17_VIC_NR_HWIRQ	48

/*
 * Screaming-source guard.  Every VIC input is dispatched via handle_simple_irq
 * with no hardware ack/EOI, so a device whose handler cannot quench its (level-
 * held) request re-vectors as fast as the CPU can loop and hard-locks the board.
 * No legitimate source on this board reaches anywhere near this rate (~400k/s at
 * HZ=100), so if a LICR line exceeds it in one jiffy, mask that line and warn --
 * the board survives and the culprit LIRQ is named.
 */
#define E17_VIC_STORM		4000

struct e17_vic {
	void __iomem		*base;
	struct irq_domain	*domain;
	raw_spinlock_t		lock;		/* guards en_count[] + LICR writes */
	u8			licr[E17_VIC_NR_HWIRQ];	/* gating LIRQ per hwirq */
	u8			unmasked[E17_VIC_NR_LIRQ]; /* LICR value to unmask */
	u8			en_count[E17_VIC_NR_LIRQ]; /* # unmasked children  */
	unsigned long		storm_jiffy[E17_VIC_NR_LIRQ];
	u32			storm_count[E17_VIC_NR_LIRQ];
	bool			stormed[E17_VIC_NR_LIRQ];
};

static struct e17_vic e17_vic;

/*
 * irq_chip ops.  A single LICRn gates a whole daisy chain (LIRQ6 carries the
 * CD2401 and both CIOs, each on its own CPU vector -> its own hwirq/child IRQ).
 * So masking is REFCOUNTED per LICR: the line is driven masked only when the
 * last child is disabled, and unmasked when the first child is enabled --
 * otherwise disable_irq() on the CD2401 tx child would also mask console rx.
 * Per-source enable/disable is the device's own job (CD2401 IER, CIO IE/MIE).
 */
static void e17_vic_irq_mask(struct irq_data *d)
{
	unsigned int hw = irqd_to_hwirq(d);
	unsigned int lirq = e17_vic.licr[hw];
	unsigned long flags;

	raw_spin_lock_irqsave(&e17_vic.lock, flags);
	if (e17_vic.en_count[lirq] && --e17_vic.en_count[lirq] == 0)
		writeb(E17_VIC_LICR_MASK, e17_vic.base + E17_VIC_LICR(lirq));
	raw_spin_unlock_irqrestore(&e17_vic.lock, flags);
}

static void e17_vic_irq_unmask(struct irq_data *d)
{
	unsigned int hw = irqd_to_hwirq(d);
	unsigned int lirq = e17_vic.licr[hw];
	unsigned long flags;

	raw_spin_lock_irqsave(&e17_vic.lock, flags);
	if (e17_vic.en_count[lirq]++ == 0)
		writeb(e17_vic.unmasked[lirq],
		       e17_vic.base + E17_VIC_LICR(lirq));
	raw_spin_unlock_irqrestore(&e17_vic.lock, flags);
}

static struct irq_chip e17_vic_chip = {
	.name		= "VIC068A",
	.irq_mask	= e17_vic_irq_mask,
	.irq_unmask	= e17_vic_irq_unmask,
};

/*
 * Cascade.  A VIC local input is delivered to the CPU as the m68k vector
 * LIVBR|LIRQ (LIVBR = VEC_USER = 0x40), which the user-vector controller maps
 * to IRQ_USER+LIRQ.  intc_user is a *legacy* irqdomain (no hierarchy support),
 * so we hang off it the classic way: chain the per-line parent IRQ into this
 * domain.  parent hwirq == IRQ_USER+LIRQ, so LIRQ = hwirq - IRQ_USER.
 */
static void e17_vic_cascade(struct irq_desc *desc)
{
	irq_hw_number_t phw = irqd_to_hwirq(irq_desc_get_irq_data(desc));
	unsigned int hw = phw - IRQ_USER;
	unsigned int lirq = (hw < E17_VIC_NR_HWIRQ) ? e17_vic.licr[hw] : 0;
	unsigned long now = jiffies;

	/*
	 * Screaming-source guard (see E17_VIC_STORM): if a line's LICR fires an
	 * impossible number of times in one jiffy, its device's handler is not
	 * quenching a level-held request -- mask the LICR so the CPU escapes the
	 * storm instead of hard-locking, and report which line.  Kept masked until
	 * a driver re-enables it (its device is misbehaving anyway).
	 */
	if (lirq >= 1 && lirq < E17_VIC_NR_LIRQ) {
		if (now != e17_vic.storm_jiffy[lirq]) {
			e17_vic.storm_jiffy[lirq] = now;
			e17_vic.storm_count[lirq] = 0;
		}
		if (++e17_vic.storm_count[lirq] >= E17_VIC_STORM) {
			unsigned long flags;

			raw_spin_lock_irqsave(&e17_vic.lock, flags);
			writeb(E17_VIC_LICR_MASK, e17_vic.base + E17_VIC_LICR(lirq));
			e17_vic.en_count[lirq] = 0;
			e17_vic.stormed[lirq] = true;
			raw_spin_unlock_irqrestore(&e17_vic.lock, flags);
			pr_warn_ratelimited("e17-vic068a: LIRQ%u storming (>=%u/jiffy) -- masked; its device's ISR is not clearing the source\n",
					    lirq, E17_VIC_STORM);
			return;
		}
	}

	/*
	 * No chained_irq_enter()/exit() here: the parent is an m68k user vector
	 * whose chip (user_irq_chip) has no mask/ack/eoi ops -- the CPU IACK
	 * clears the vector and the SR interrupt-priority level masks equal and
	 * lower priorities until we return, so there is nothing to do at this
	 * level (and chained_irq_enter() would call a NULL chip->irq_mask).
	 * Dispatch straight into the VIC domain.
	 */
	generic_handle_domain_irq(e17_vic.domain, hw);
}

static int e17_vic_domain_map(struct irq_domain *dom, unsigned int virq,
			      irq_hw_number_t hw)
{
	unsigned int lirq = e17_vic.licr[hw];
	unsigned long flags;

	/*
	 * Default the gating line masked -- but only if no sibling on the same
	 * LICR is already unmasked (mapping the tx child must not mask live rx).
	 */
	raw_spin_lock_irqsave(&e17_vic.lock, flags);
	if (e17_vic.en_count[lirq] == 0)
		writeb(E17_VIC_LICR_MASK, e17_vic.base + E17_VIC_LICR(lirq));
	raw_spin_unlock_irqrestore(&e17_vic.lock, flags);

	irq_set_chip_and_handler(virq, &e17_vic_chip, handle_simple_irq);
	irq_set_chip_data(virq, &e17_vic);
	/*
	 * Mask at the VIC the instant the line is disabled (not lazily), so a
	 * driver's disable_irq()/free_irq() reliably gates the hardware before
	 * it tears the device down -- matching the old hand-rolled LICR mask.
	 */
	irq_set_status_flags(virq, IRQ_DISABLE_UNLAZY);

	/* Chain the CPU-vector parent (IRQ_USER+LIRQ) into this domain. */
	irq_set_chained_handler_and_data(IRQ_USER + hw, e17_vic_cascade,
					 &e17_vic);
	return 0;
}

/*
 * DT binding: #interrupt-cells = <3> -> <lirq level vector>.
 *   lirq   : VIC local input (LIRQ1..7) whose LICR gates the line.
 *   level  : CPU IPL the VIC raises for it.
 *   vector : 0 for a VIC-vectored line (the VIC supplies LIVBR|lirq, so the
 *            hwirq is the lirq); otherwise the device-supplied CPU vector of a
 *            self-vectored line (CD2401), whose hwirq is that vector's offset.
 * hwirq is therefore the vector offset in both cases, matching the cascade's
 * parent = IRQ_USER + hwirq (vector = VEC_USER + hwirq).
 */
static int e17_vic_domain_xlate(struct irq_domain *d, struct device_node *ctrlr,
				const u32 *intspec, unsigned int intsize,
				irq_hw_number_t *out_hwirq, unsigned int *out_type)
{
	u32 lirq, level, vec;
	irq_hw_number_t hw;
	u8 unmasked;

	if (intsize < 3)
		return -EINVAL;
	lirq  = intspec[0];
	level = intspec[1] & 0x07;
	vec   = intspec[2];
	if (lirq < 1 || lirq >= E17_VIC_NR_LIRQ)
		return -EINVAL;

	if (vec == 0)
		hw = lirq;			/* VIC-vectored: LIVBR|lirq */
	else if (vec >= VEC_USER)
		hw = vec - VEC_USER;		/* self-vectored: device vector */
	else
		return -EINVAL;
	if (hw >= E17_VIC_NR_HWIRQ)
		return -EINVAL;

	/*
	 * LICR value that unmasks this line: the CPU level, plus the "VIC
	 * supplies the vector" sense bits (0x30) for a VIC-vectored line -- a
	 * self-vectored line omits them so the VIC forwards the device's own
	 * vector.  The E17 daisy chains (notably LIRQ6: CD2401 + CIOs) are
	 * self-vectored and MUST run at level 5 (only level-5 CPU IACK cycles
	 * are routed to the chain -- HW manual); warn if the DT disagrees.
	 */
	unmasked = E17_VIC_LICR_LEVEL(level);
	if (vec == 0)
		unmasked |= E17_VIC_LICR_VEC;
	else if (level != 5)
		pr_warn("e17-vic068a: self-vectored LIRQ%u vec 0x%x at level %u (chain wants 5)\n",
			lirq, vec, level);

	/* All children sharing a LICR must agree on the unmask value. */
	if (e17_vic.en_count[lirq] || e17_vic.unmasked[lirq]) {
		if (e17_vic.unmasked[lirq] != unmasked)
			pr_warn("e17-vic068a: LIRQ%u children disagree on LICR (0x%x vs 0x%x)\n",
				lirq, e17_vic.unmasked[lirq], unmasked);
	}
	e17_vic.unmasked[lirq] = unmasked;
	e17_vic.licr[hw]       = lirq;
	*out_hwirq = hw;
	*out_type = IRQ_TYPE_NONE;
	return 0;
}

static const struct irq_domain_ops e17_vic_domain_ops = {
	.xlate	= e17_vic_domain_xlate,	/* cells = <LIRQ level> */
	.map	= e17_vic_domain_map,
};

static int __init e17_vic068a_init(void)
{
	struct device_node *np;

	np = of_find_compatible_node(NULL, NULL, "eltec,e17-vic068a");
	if (!np)
		return 0;			/* not this board */

	raw_spin_lock_init(&e17_vic.lock);
	e17_vic.base = of_iomap(np, 0);
	if (!e17_vic.base) {
		pr_err("e17-vic068a: cannot map registers\n");
		of_node_put(np);
		return -ENOMEM;
	}

	e17_vic.domain = irq_domain_create_linear(of_fwnode_handle(np),
						  E17_VIC_NR_HWIRQ,
						  &e17_vic_domain_ops, &e17_vic);
	if (!e17_vic.domain) {
		pr_err("e17-vic068a: cannot create irq domain\n");
		iounmap(e17_vic.base);
		of_node_put(np);
		return -ENOMEM;
	}

	pr_info("e17-vic068a: probed (regs %p, %d LIRQ lines) -- cascaded on intc_user\n",
		e17_vic.base, E17_VIC_NR_LIRQ);

	of_node_put(np);
	return 0;
}
/* After the user-vector controller (core), before device drivers. */
postcore_initcall(e17_vic068a_init);
