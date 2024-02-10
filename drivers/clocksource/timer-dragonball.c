// SPDX-License-Identifier: GPL-2.0

#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/clocksource.h>
#include <linux/clockchips.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "timer-of.h"

#define REG_TCTL		0x0
#define REG_TCTL_TEN	BIT(0)
#define REG_TCTL_IRQEN	BIT(4)
#define REG_TCTL_FRR	BIT(8)
#define REG_TPRER		0x2
#define REG_TCMP		0x4
#define REG_TCR			0x6
#define REG_TCN			0x8
#define REG_TSTAT		0xa

static int dragonball_timer_set_oneshot(struct clock_event_device *evt)
{
	struct timer_of *timer = to_timer_of(evt);
	void *base = timer_of_base(timer);
	u16 tctl;

	tctl = readw(base + REG_TCTL);
	tctl &= ~(REG_TCTL_FRR | REG_TCTL_TEN);
	tctl |= REG_TCTL_IRQEN;
	writew(tctl, base + REG_TCTL);

	return 0;
}

static int dragonball_timer_shutdown(struct clock_event_device *evt)
{
	struct timer_of *timer = to_timer_of(evt);

	return 0;
}

static int dragonball_timer_next_event(unsigned long delta,
				     struct clock_event_device *evt)
{
	struct timer_of *timer = to_timer_of(evt);
	void *base = timer_of_base(timer);
	u16 tctl;

	BUG_ON(delta > 0xffff);

	/* Set compare value */
	writew(delta, base + REG_TCMP);

	/* Trigger */
	tctl = readw(base + REG_TCTL);
	tctl |= REG_TCTL_TEN;
	writew(tctl, base + REG_TCTL);

	return 0;
}

static irqreturn_t dragonball_timer_irq(int irq, void *dev_id)
{
	struct clock_event_device *evt = dev_id;
	struct timer_of *timer = to_timer_of(evt);
	void *base = timer_of_base(timer);
	u16 tctl;

	/* Clear the interrupt */
	readw(base + REG_TSTAT);
	writew(0, base + REG_TSTAT);

	/* Clear the enable bit */
	tctl = readw(base + REG_TCTL);
	tctl &= ~REG_TCTL_TEN;
	writew(tctl, base + REG_TCTL);

	evt->event_handler(evt);

	return IRQ_HANDLED;
}

static struct clock_event_device dragonball_clockevent_device = {
	.name				= "dragonball_timer",
	.features			= CLOCK_EVT_FEAT_ONESHOT,
	.set_state_shutdown	= dragonball_timer_shutdown,
	.set_state_oneshot	= dragonball_timer_set_oneshot,
	.set_next_event		= dragonball_timer_next_event,
};

int __init dragonball_timer_init_of(struct device_node *np)
{
	struct timer_of *to;
	void *base;
	u16 tctl;
	int ret;

	to = kzalloc(sizeof(*to), GFP_KERNEL);
	if (!to)
		return -ENOMEM;

	to->flags = TIMER_OF_BASE | TIMER_OF_IRQ; // TIMER_OF_CLOCK |
	to->of_irq.handler = dragonball_timer_irq;
	ret = timer_of_init(np, to);
	if (ret)
		return ret;

	/* Make sure the timer is disabled */
	base = timer_of_base(to);

	/* Reset the prescaler just incase someone else messed with it */
	writew(0, base + REG_TPRER);

	tctl = readw(base + REG_TCTL);
	tctl &= ~(REG_TCTL_TEN | REG_TCTL_IRQEN);
	writew(tctl, base + REG_TCTL);

	dragonball_clockevent_device.cpumask = cpu_possible_mask;
	dragonball_clockevent_device.irq = to->of_irq.irq;
	to->clkevt = dragonball_clockevent_device;

	clockevents_config_and_register(&to->clkevt,
			32768, 1, 0xffff);

	return 0;
}

TIMER_OF_DECLARE(dragonball, "motorola,mc68ez328-timer", dragonball_timer_init_of);
