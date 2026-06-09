// SPDX-License-Identifier: GPL-2.0

#include <asm/io.h>
#include <asm/vdp.h>
#include <linux/interrupt.h>
#include <linux/clocksource.h>
#include <linux/clockchips.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sched_clock.h>
#include <linux/of_irq.h>
#include "timer-of.h"

static u64 everdrive_timer_read_cnt(void)
{
	void *reg = (void *) 0xa130d6;

	return ioread16be(reg);
}

static u64 everdrive_clocksource_read(struct clocksource *cs)
{
	return everdrive_timer_read_cnt();
}

static struct clocksource everdrive_clocksource = {
	.name	= "everdrive-timer",
	.rating	= 150,
	.read	= everdrive_clocksource_read,
	.mask	= CLOCKSOURCE_MASK(16),
	.flags	= CLOCK_SOURCE_IS_CONTINUOUS,
};

#define VDP_REG_MODE1                   0x00U
#define VDP_MODE1_M4                    BIT(2)
#define VDP_MODE1_IE0                   BIT(4)

#define VDP_REG_MODE2                   0x01U
#define VDP_MODE2_M5                    BIT(2)
#define VDP_MODE2_IE1                   BIT(5)
#define VDP_MODE2_DE                    BIT(6)

/* The tick is driven by the VDP vertical-blank interrupt (NTSC ~60Hz). */
#define EVERDRIVE_VBLANK_HZ             60

static int everdrive_set_periodic(struct clock_event_device *evt)
{
	/* Enable the VDP vertical-blank interrupt that drives the tick. */
	vdp_register_set(VDP_REG_MODE2,
			 VDP_MODE2_M5 | VDP_MODE2_DE | VDP_MODE2_IE1);
	return 0;
}

static int everdrive_set_shutdown(struct clock_event_device *evt)
{
	/* Disable the vblank interrupt; leave display/mode bits as they are. */
	vdp_register_set(VDP_REG_MODE2, VDP_MODE2_M5 | VDP_MODE2_DE);
	return 0;
}

static struct clock_event_device everdrive_clockevent = {
	.name			= "everdrive-vblank",
	.features		= CLOCK_EVT_FEAT_PERIODIC,
	.rating			= 200,
	.set_state_periodic	= everdrive_set_periodic,
	.set_state_shutdown	= everdrive_set_shutdown,
	.cpumask		= cpu_possible_mask,
};

static irqreturn_t everdrive_timer_irq(int irq, void *dev_id)
{
	struct clock_event_device *evt = dev_id;

	/* Reading the VDP status register acknowledges the vblank IRQ. */
	vdp_read_status();

	evt->event_handler(evt);

	return IRQ_HANDLED;
}

static const char *irq_name = "everdrive-timer";

static int __init everdrive_timer_init_of(struct device_node *np)
{
	unsigned long rate = 1000;
	int ret;
	int irq;

	sched_clock_register(everdrive_timer_read_cnt, 16, rate);

	ret = clocksource_register_hz(&everdrive_clocksource, rate);
	if (ret)
		return ret;

	irq = of_irq_get(np, 0);
	if (irq < 0)
		return irq;

	/* dev_id is the clockevent so the IRQ handler can reach it. */
	ret = request_irq(irq, everdrive_timer_irq, IRQF_TIMER | IRQF_SHARED,
			  irq_name, &everdrive_clockevent);
	if (ret)
		return ret;

	everdrive_clockevent.irq = irq;

	/* Periodic-only: deltas are nominal (one vblank period per tick). The
	 * VDP interrupt is enabled by the set_state_periodic() callback when
	 * the tick framework selects this device. */
	clockevents_config_and_register(&everdrive_clockevent,
					EVERDRIVE_VBLANK_HZ, 1, 1);

	return 0;
}

TIMER_OF_DECLARE(everdrive, "krikzz,everdrive-timer", everdrive_timer_init_of);
