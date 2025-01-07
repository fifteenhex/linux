#include <linux/clockchips.h>
#include <linux/clocksource.h>
#include <linux/interrupt.h>
#include <linux/of_irq.h>
#include <asm/mvme147hw.h>

#define PCC_TIMER_CLOCK_FREQ 160000
#define PCC_TIMER_CYCLES     (PCC_TIMER_CLOCK_FREQ / HZ)
#define PCC_TIMER_PRELOAD    (0x10000 - PCC_TIMER_CYCLES)

static u32 clk_total;

static u64 mvme147_read_clk(struct clocksource *cs)
{
	unsigned long flags;
	u8 overflow, tmp;
	u16 count;
	u32 ticks;

	local_irq_save(flags);
	tmp = m147_pcc->t1_cntrl >> 4;
	count = m147_pcc->t1_count;
	overflow = m147_pcc->t1_cntrl >> 4;
	if (overflow != tmp)
		count = m147_pcc->t1_count;
	count -= PCC_TIMER_PRELOAD;
	ticks = count + overflow * PCC_TIMER_CYCLES;
	ticks += clk_total;
	local_irq_restore(flags);

	return ticks;
}

static struct clocksource mvme147_clk = {
	.name   = "pcc",
	.rating = 250,
	.read   = mvme147_read_clk,
	.mask   = CLOCKSOURCE_MASK(32),
	.flags  = CLOCK_SOURCE_IS_CONTINUOUS,
};

static irqreturn_t mvme147_timer_int (int irq, void *dev_id)
{
	unsigned long flags;

	local_irq_save(flags);
	m147_pcc->t1_cntrl = PCC_TIMER_CLR_OVF | PCC_TIMER_COC_EN |
			     PCC_TIMER_TIC_EN;
	m147_pcc->t1_int_cntrl = PCC_INT_ENAB | PCC_TIMER_INT_CLR |
				 PCC_LEVEL_TIMER1;
	clk_total += PCC_TIMER_CYCLES;
	legacy_timer_tick(1);
	local_irq_restore(flags);

	return IRQ_HANDLED;
}

static bool called = false;

static int __init mvme147_pcc_timer_init(struct device_node *np)
{
	int irq;

	/* HAck to only call on the first timer for now */
	if (called)
		return 0;
	called = true;

	irq = of_irq_get(np, 0);
	if (irq < 0)
		return irq;
	if (!irq)
		return -ENODEV;

	if (request_irq(irq, mvme147_timer_int, IRQF_TIMER,
		"timer 1", NULL))
	pr_err("Couldn't register timer interrupt\n");

	/* Init the clock with a value */
	/* The clock counter increments until 0xFFFF then reloads */
	m147_pcc->t1_preload = PCC_TIMER_PRELOAD;
	m147_pcc->t1_cntrl = PCC_TIMER_CLR_OVF | PCC_TIMER_COC_EN |
			     PCC_TIMER_TIC_EN;
	m147_pcc->t1_int_cntrl = PCC_INT_ENAB | PCC_TIMER_INT_CLR |
				 PCC_LEVEL_TIMER1;

	clocksource_register_hz(&mvme147_clk, PCC_TIMER_CLOCK_FREQ);

        return 0;
}

TIMER_OF_DECLARE(mvme147_pcc, "motorola,mvme147-pcc-timer", mvme147_pcc_timer_init);
