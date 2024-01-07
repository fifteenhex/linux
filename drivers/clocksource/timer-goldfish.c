// SPDX-License-Identifier: GPL-2.0

#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/clocksource.h>
#include <linux/clockchips.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/goldfish.h>
#include <clocksource/timer-goldfish.h>

#include "timer-of.h"

struct goldfish_timer {
	struct clocksource cs;
	struct clock_event_device ced;
	struct resource res;
	void __iomem *base;
};

static struct goldfish_timer *ced_to_gf(struct clock_event_device *ced)
{
	return container_of(ced, struct goldfish_timer, ced);
}

static struct goldfish_timer *cs_to_gf(struct clocksource *cs)
{
	return container_of(cs, struct goldfish_timer, cs);
}

static u64 _goldfish_timer_read(void __iomem *base)
{
	u32 time_low, time_high;
	u64 ticks;

	/*
	 * time_low: get low bits of current time and update time_high
	 * time_high: get high bits of time at last time_low read
	 */
	time_low = gf_ioread32(base + TIMER_TIME_LOW);
	time_high = gf_ioread32(base + TIMER_TIME_HIGH);

	ticks = ((u64)time_high << 32) | time_low;

	return ticks;
}

static u64 goldfish_timer_read(struct clocksource *cs)
{
	struct goldfish_timer *timerdrv = cs_to_gf(cs);
	void __iomem *base = timerdrv->base;

	return _goldfish_timer_read(base);
}

static int _goldfish_timer_set_oneshot(void __iomem *base)
{
	gf_iowrite32(0, base + TIMER_ALARM_HIGH);
	gf_iowrite32(0, base + TIMER_ALARM_LOW);
	gf_iowrite32(1, base + TIMER_IRQ_ENABLED);

	u32 val = gf_ioread32(base + TIMER_IRQ_ENABLED);
	BUG_ON(!val);

	return 0;
}

static int goldfish_timer_set_oneshot(struct clock_event_device *evt)
{
	struct goldfish_timer *timerdrv = ced_to_gf(evt);
	void __iomem *base = timerdrv->base;

	return _goldfish_timer_set_oneshot(base);
}

static int goldfish_timer_set_oneshot_of(struct clock_event_device *evt)
{
	struct timer_of *timer = to_timer_of(evt);

	return _goldfish_timer_set_oneshot(timer_of_base(timer));
}

static int _goldfish_timer_shutdown(void __iomem *base)
{
	gf_iowrite32(0, base + TIMER_IRQ_ENABLED);

	return 0;
}

static int goldfish_timer_shutdown(struct clock_event_device *evt)
{
	struct goldfish_timer *timerdrv = ced_to_gf(evt);
	void __iomem *base = timerdrv->base;

	return _goldfish_timer_shutdown(base);
}

static int goldfish_timer_shutdown_of(struct clock_event_device *evt)
{
	struct timer_of *timer = to_timer_of(evt);

	return _goldfish_timer_shutdown(timer_of_base(timer));
}

static int _goldfish_timer_next_event(unsigned long delta,
				      void __iomem *base)
{
	u64 now;

	now = _goldfish_timer_read(base);

	now += delta;

	gf_iowrite32(upper_32_bits(now), base + TIMER_ALARM_HIGH);
	gf_iowrite32(lower_32_bits(now), base + TIMER_ALARM_LOW);

	return 0;
}

static int goldfish_timer_next_event(unsigned long delta,
				     struct clock_event_device *evt)
{
	struct goldfish_timer *timerdrv = ced_to_gf(evt);
	void __iomem *base = timerdrv->base;

	return _goldfish_timer_next_event(delta, base);
}

static int goldfish_timer_next_event_of(unsigned long delta,
				     struct clock_event_device *evt)
{
	struct timer_of *timer = to_timer_of(evt);

	return _goldfish_timer_next_event(delta, timer_of_base(timer));
}

static irqreturn_t _goldfish_timer_irq(struct clock_event_device *evt,
				       void __iomem *base)
{
	gf_iowrite32(1, base + TIMER_CLEAR_INTERRUPT);

	evt->event_handler(evt);

	return IRQ_HANDLED;
}

static irqreturn_t goldfish_timer_irq(int irq, void *dev_id)
{
	struct goldfish_timer *timerdrv = dev_id;
	struct clock_event_device *evt = &timerdrv->ced;
	void __iomem *base = timerdrv->base;

	return _goldfish_timer_irq(evt, base);
}

static irqreturn_t goldfish_timer_irq_of(int irq, void *dev_id)
{
	struct clock_event_device *evt = dev_id;
	struct timer_of *timer = to_timer_of(evt);

	return _goldfish_timer_irq(evt, timer_of_base(timer));
}

static struct clock_event_device goldfish_clockevent_device = {
	.name			= "goldfish_timer",
	.features		= CLOCK_EVT_FEAT_ONESHOT,
	.set_state_shutdown	= goldfish_timer_shutdown,
	.set_state_oneshot      = goldfish_timer_set_oneshot,
	.set_next_event		= goldfish_timer_next_event,
};

int __init goldfish_timer_init(int irq, void __iomem *base)
{
	struct goldfish_timer *timerdrv;
	int ret;

	timerdrv = kzalloc(sizeof(*timerdrv), GFP_KERNEL);
	if (!timerdrv)
		return -ENOMEM;

	timerdrv->base = base;

	timerdrv->ced = goldfish_clockevent_device;

	timerdrv->res = (struct resource){
		.name  = "goldfish_timer",
		.start = (unsigned long)base,
		.end   = (unsigned long)base + 0xfff,
	};

	ret = request_resource(&iomem_resource, &timerdrv->res);
	if (ret) {
		pr_err("Cannot allocate '%s' resource\n", timerdrv->res.name);
		return ret;
	}

	timerdrv->cs = (struct clocksource){
		.name		= "goldfish_timer",
		.rating		= 400,
		.read		= goldfish_timer_read,
		.mask		= CLOCKSOURCE_MASK(64),
		.flags		= 0,
		.max_idle_ns	= LONG_MAX,
	};

	clocksource_register_hz(&timerdrv->cs, NSEC_PER_SEC);

	ret = request_irq(irq, goldfish_timer_irq, IRQF_TIMER,
			  "goldfish_timer", timerdrv);
	if (ret) {
		pr_err("Couldn't register goldfish-timer interrupt\n");
		return ret;
	}

	clockevents_config_and_register(&timerdrv->ced, NSEC_PER_SEC,
					1, 0xffffffff);

	return 0;
}

static struct clock_event_device goldfish_clockevent_device_of = {
	.name			= "goldfish_timer",
	.features		= CLOCK_EVT_FEAT_ONESHOT,
	.set_state_shutdown	= goldfish_timer_shutdown_of,
	.set_state_oneshot      = goldfish_timer_set_oneshot_of,
	.set_next_event		= goldfish_timer_next_event_of,
};

int __init goldfish_timer_init_of(struct device_node *np)
{
	struct timer_of *to;
	int ret;

	to = kzalloc(sizeof(*to), GFP_KERNEL);
	if (!to)
		return -ENOMEM;

	to->flags = TIMER_OF_BASE | TIMER_OF_IRQ;
	to->of_irq.handler = goldfish_timer_irq_of;
	ret = timer_of_init(np, to);
	if (ret)
		return ret;

	goldfish_clockevent_device.cpumask = cpu_possible_mask;
	goldfish_clockevent_device.irq = to->of_irq.irq;
	to->clkevt = goldfish_clockevent_device_of;

	clockevents_config_and_register(&to->clkevt,
			NSEC_PER_SEC, 1, 0xffffffff);

	return 0;
}

TIMER_OF_DECLARE(goldfish, "google,goldfish-timer", goldfish_timer_init_of);
