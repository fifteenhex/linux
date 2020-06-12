#include <linux/clk.h>
#include <linux/clockchips.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqreturn.h>
#include <linux/sched_clock.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>

#include "timer-of.h"

#define REG_CTRL		0
#define CTRL_ENABLE		BIT(0)
#define CTRL_TRIG		BIT(1)
#define CTRL_CLEAR		BIT(2)
#define CTRL_CAPTURE		BIT(3)
#define CTRL_IRQ		BIT(8)
#define REG_COMPARE_LOW		0x8
#define REG_COMPARE_HIGH	0xc
#define REG_COUNTER_LOW		0x10
#define REG_COUNTER_HIGH	0x14

#define TIMER_SYNC_TICKS	3

struct msc313e_timer {
	struct timer_of oftimer;
	struct clocksource clksrc;
};

static struct clocksource *sched_clock;

#define to_msc313e_timer(ptr) container_of(ptr, struct msc313e_timer, clksrc)

static int msc313e_timer_clkevt_shutdown(struct clock_event_device *evt)
{
	struct timer_of *timer = to_timer_of(evt);
	u16 reg;

	printk("shutdown\n");

	reg = readw_relaxed(timer->of_base.base + REG_CTRL);
	reg &= ~CTRL_ENABLE;
	writew_relaxed(reg, timer->of_base.base + REG_CTRL);

	return 0;
}

static int msc313e_timer_clkevt_set_oneshot(struct clock_event_device *evt)
{
	struct timer_of *timer = to_timer_of(evt);
	u16 reg = 0;

	printk("one shot\n");

	//result = readw_relaxed(timer->oftimer.of_base.base + REG_COUNTER_LOW);

	return 0;
}

static int msc313e_timer_set_periodic(struct clock_event_device *evt)
{
	struct timer_of *timer = to_timer_of(evt);

	printk("periodic\n");

	writew_relaxed(CTRL_ENABLE | CTRL_IRQ,
			timer->of_base.base + REG_CTRL);
	return 0;
}

static int msc313e_timer_clkevt_next_event(unsigned long evt,
				   struct clock_event_device *clkevt)
{
	struct timer_of *to = to_timer_of(clkevt);

	printk("next\n");

	return 0;
}


static irqreturn_t msc313e_timer_clkevt_irq(int irq, void *dev_id)
{
	struct clock_event_device *evt = dev_id;

	evt->event_handler(evt);

	return IRQ_HANDLED;
}

static struct timer_of msc313_tick = {
	.flags = TIMER_OF_IRQ | TIMER_OF_CLOCK | TIMER_OF_BASE,

	.clkevt = {
		.name = "msc313_tick",
		.rating = 350,
		.features = CLOCK_EVT_FEAT_PERIODIC | CLOCK_EVT_FEAT_ONESHOT,
		.set_state_shutdown = msc313e_timer_clkevt_shutdown,
		.set_state_periodic = msc313e_timer_set_periodic,
		.set_state_oneshot = msc313e_timer_clkevt_set_oneshot,
		.tick_resume = msc313e_timer_clkevt_shutdown,
		.set_next_event = msc313e_timer_clkevt_next_event,
		.cpumask = cpu_possible_mask,
	},

	.of_irq = {
		.handler = msc313e_timer_clkevt_irq,
		.flags = IRQF_TIMER | IRQF_IRQPOLL,
	},
};

static u64 msc313e_timer_read(struct clocksource *cs)
{
	struct msc313e_timer *timer = to_msc313e_timer(cs);
	u64 result;

	result = readw_relaxed(timer->oftimer.of_base.base + REG_COUNTER_LOW);
	result |= readw_relaxed(timer->oftimer.of_base.base + REG_COUNTER_HIGH) << 16;

	return result & cs->mask;
}

static int msc313e_timer_enable(struct clocksource *cs){
	struct msc313e_timer *timer = to_msc313e_timer(cs);
	u16 reg;

	printk("enable\n");

	reg = readw_relaxed(timer->oftimer.of_base.base + REG_CTRL);
	reg |= CTRL_ENABLE;
	writew_relaxed(reg, timer->oftimer.of_base.base + REG_CTRL);

	return 0;
};

static u64 msc313e_timer_sched_clock_read(void)
{
	return msc313e_timer_read(sched_clock);
}

static void msc313e_timer_disable(struct clocksource *cs){
	struct msc313e_timer *timer = to_msc313e_timer(cs);
	u16 reg;

	printk("disable\n");

	reg = readw_relaxed(timer->oftimer.of_base.base + REG_CTRL);
	reg &= ~CTRL_ENABLE;
	writew_relaxed(reg, timer->oftimer.of_base.base + REG_CTRL);
}

static int __init msc313e_timer_probe(struct device_node *node)
{
	int ret;
	struct msc313e_timer *timer;
	bool tick = of_property_read_bool(node, "mstar,tick");
	bool schedclk = of_property_read_bool(node, "mstar,schedclk");

	if(tick){
		ret = timer_of_init(node, &msc313_tick);
		if(ret)
			goto out;
		clockevents_config_and_register(&msc313_tick.clkevt,
				timer_of_rate(&msc313_tick),
				TIMER_SYNC_TICKS, 0xffffffff);
		goto out;
	}

	timer = kzalloc(sizeof(*timer), GFP_KERNEL);
	if (!timer){
		ret = -ENOMEM;
		goto out;
	}

	timer->oftimer.flags = TIMER_OF_BASE | TIMER_OF_CLOCK;
	timer->oftimer.of_irq.handler = msc313e_timer_clkevt_irq;
	timer->oftimer.of_irq.flags = IRQF_TIMER | IRQF_IRQPOLL;
	ret = timer_of_init(node, &timer->oftimer);
	if (ret)
		goto out;

	if(schedclk){
		sched_clock = &timer->clksrc;
		msc313e_timer_enable(sched_clock);
		sched_clock_register(msc313e_timer_sched_clock_read, 32, timer_of_rate(&timer->oftimer));
		goto out;
	}

	timer->clksrc.name = node->name;
	timer->clksrc.rating = 200;
	timer->clksrc.read = msc313e_timer_read;
	timer->clksrc.mask = CLOCKSOURCE_MASK(32);
	timer->clksrc.flags = CLOCK_SOURCE_IS_CONTINUOUS;
	timer->clksrc.enable =  msc313e_timer_enable;
	timer->clksrc.disable =  msc313e_timer_disable;

	ret = clocksource_register_hz(&timer->clksrc, timer_of_rate(&timer->oftimer));

out:
	return ret;
}

TIMER_OF_DECLARE(msc313, "mstar,msc313e-timer",
		msc313e_timer_probe);
