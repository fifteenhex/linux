// SPDX-License-Identifier: GPL-2.0
/*
 * Watchdog driver for the ELTEC EUROCOM-17 (m68k dual-68040 VME SBC).
 *
 * The board watchdog (HW manual 3.8) is a single write-triggered register at
 * $FEC5.0000:
 *   - it is DISABLED until the first write, which arms it;
 *   - every subsequent write pets it;
 *   - if it is not petted within the time-out period (jumper J1401: 100 ms,
 *     min 70 / max 140, or 1.6 s) a reset pulse is generated;
 *   - it CANNOT be disabled by software -- only a reset (of any kind) turns it
 *     off again.
 * A watchdog-caused reset lights the front-panel hex display's left decimal
 * point; software reads that as bit 7 of the system CIO's port A ($FEC3.0002)
 * reading 0, which lets us report WDIOF_CARDRESET.
 *
 * Because the hardware time-out is very short, the watchdog framework's worker
 * auto-pings the hardware (at max_hw_heartbeat_ms/2) between the far less
 * frequent userspace keepalives, and -- since there is no way to stop the
 * hardware -- keeps pinging after /dev/watchdog is closed.  A genuine CPU/bus
 * hang stops that worker, so the hardware times out and resets the board: the
 * self-recovery we want when poking at fragile interrupt paths.
 */
#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/io.h>
#include <linux/irqflags.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/watchdog.h>

/* System CIO port A, bit 7: 0 = the last reset was a watchdog reset. */
#define E17_WDT_PA7_PHYS	0xfec30002
#define E17_WDT_PA7		0x80

/*
 * Hardware time-out (jumper J1401: 100 ms or 1.6 s).  The framework pings at
 * max_hw_heartbeat_ms/2, so this must not exceed the real hardware period.
 * Default 1600 to match J1401 on the 1.6 s setting: the core then pings every
 * 800 ms.  This board is HZ_PERIODIC (CONFIG_HZ=100) with no high-res clockevent
 * and no NOHZ, so the 100 Hz tick never stops and drives the tick-based petting
 * worker well within 1.6 s at boot and in idle.  The 100 ms jumper, by contrast,
 * cannot be armed from boot: there is a >100 ms interrupts-off window during boot
 * the tick-based petter cannot cover, so at 100 ms pass e17_wdt.arm_on_boot=0
 * e17_wdt.hw_margin_ms=100 and drive /dev/watchdog from userspace instead.
 */
static unsigned int hw_margin_ms = 1600;
module_param(hw_margin_ms, uint, 0444);
MODULE_PARM_DESC(hw_margin_ms,
	"Hardware time-out in ms; core pings at half this (default 1600 = 1.6s jumper)");

static bool nowayout = WATCHDOG_NOWAYOUT;
module_param(nowayout, bool, 0444);
MODULE_PARM_DESC(nowayout,
	"Watchdog cannot be stopped once started (default " __MODULE_STRING(WATCHDOG_NOWAYOUT) ")");

/*
 * Boot-time arming is ON by default (J1401 on 1.6 s): the kernel arms the
 * watchdog and pets it from boot until userspace opens /dev/watchdog.  The 1.6 s
 * period leaves plenty of margin over the boot >100 ms interrupts-off window that
 * reset-loops the tighter 100 ms jumper.  On a board jumpered to 100 ms this MUST
 * be turned off (e17_wdt.arm_on_boot=0), or the board reset-loops at boot.
 */
static bool arm_on_boot = true;
module_param(arm_on_boot, bool, 0444);
MODULE_PARM_DESC(arm_on_boot,
	"Arm the watchdog at boot so the kernel pets it until userspace opens "
	"/dev/watchdog (needs CONFIG_WATCHDOG_HANDLE_BOOT_ENABLED; default y -- "
	"set 0 if J1401 is on the 100 ms setting)");

/*
 * Pet the hardware directly from a hard-IRQ hrtimer (which, on this
 * clockevent-less board, runs straight from the periodic tick), in addition to
 * the watchdog core's SCHED_FIFO ping kthread.  The kthread hop makes the core's
 * petting depend on the kthread being scheduled, which a non-preemptible
 * CPU-bound stretch (e.g. around the init handoff) can delay past the 1.6 s
 * period and reset a perfectly healthy board.  The hard-IRQ pet has no such
 * dependency: as long as the tick runs the watchdog is fed, so it only fires
 * when the tick itself dies -- a CPU0-wedged / interrupts-off-forever hang,
 * exactly the catastrophic case that needs a reset.  (Trade-off: it no longer
 * enforces userspace liveness across /dev/watchdog; acceptable here, where the
 * watchdog exists for kernel-hang self-recovery.)  Set 0 to rely solely on the
 * core's kthread petting.
 */
static bool hardpet = true;
module_param(hardpet, bool, 0444);
MODULE_PARM_DESC(hardpet,
	"pet the watchdog from a tick-driven hard-IRQ timer, not just the core's "
	"kthread (default y; survives kthread starvation)");

static bool selftest;
module_param(selftest, bool, 0444);
MODULE_PARM_DESC(selftest,
	"At probe, arm the watchdog and hang to prove it resets the board "
	"(self-terminating: confirms via CARDRESET on the next boot)");

struct e17_wdt {
	struct watchdog_device	wdd;
	void __iomem		*trigger;	/* $FEC5.0000: any write pets */
	struct hrtimer		hardpet_timer;	/* tick-driven hard-IRQ petter */
	ktime_t			hardpet_interval;
};

static int e17_wdt_ping(struct watchdog_device *wdd)
{
	struct e17_wdt *w = watchdog_get_drvdata(wdd);

	writeb(0, w->trigger);
	return 0;
}

/* Hard-IRQ (tick-driven) pet: independent of the core's ping kthread. */
static enum hrtimer_restart e17_wdt_hardpet(struct hrtimer *t)
{
	struct e17_wdt *w = container_of(t, struct e17_wdt, hardpet_timer);

	writeb(0, w->trigger);
	hrtimer_forward_now(t, w->hardpet_interval);
	return HRTIMER_RESTART;
}

static void e17_wdt_hardpet_cancel(void *data)
{
	struct e17_wdt *w = data;

	hrtimer_cancel(&w->hardpet_timer);
}

static int e17_wdt_start(struct watchdog_device *wdd)
{
	/*
	 * The first write arms the hardware; from then on it cannot be disabled,
	 * so flag it HW-running -- the core will keep pinging it even across a
	 * /dev/watchdog close rather than (impossibly) stopping it.
	 */
	set_bit(WDOG_HW_RUNNING, &wdd->status);
	return e17_wdt_ping(wdd);
}

/* No .stop: the hardware has no software disable.  See watchdog_stop(). */
static const struct watchdog_ops e17_wdt_ops = {
	.owner	= THIS_MODULE,
	.start	= e17_wdt_start,
	.ping	= e17_wdt_ping,
};

static const struct watchdog_info e17_wdt_info = {
	.identity	= "ELTEC E17 watchdog",
	.options	= WDIOF_KEEPALIVEPING | WDIOF_SETTIMEOUT |
			  WDIOF_MAGICCLOSE | WDIOF_CARDRESET,
};

static int e17_wdt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct e17_wdt *w;
	void __iomem *pa7;
	int ret;

	w = devm_kzalloc(dev, sizeof(*w), GFP_KERNEL);
	if (!w)
		return -ENOMEM;

	w->trigger = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(w->trigger))
		return PTR_ERR(w->trigger);

	w->wdd.info		= &e17_wdt_info;
	w->wdd.ops		= &e17_wdt_ops;
	w->wdd.parent		= dev;
	w->wdd.min_timeout	= 1;
	w->wdd.max_timeout	= 0xffff;
	w->wdd.timeout		= 30;			/* userspace keepalive window */
	w->wdd.max_hw_heartbeat_ms = hw_margin_ms;
	watchdog_set_drvdata(&w->wdd, w);
	watchdog_init_timeout(&w->wdd, 0, dev);
	watchdog_set_nowayout(&w->wdd, nowayout);

	/*
	 * Reset cause: system CIO PA7 == 0 means the previous reset was ours.
	 * devm_ioremap (not a requested region) so it does not clash with the
	 * CIO driver mapping the same chip.
	 */
	pa7 = devm_ioremap(dev, E17_WDT_PA7_PHYS, 1);
	if (pa7 && !(readb(pa7) & E17_WDT_PA7))
		w->wdd.bootstatus |= WDIOF_CARDRESET;

	/*
	 * On by default: mark it running so the framework arms it (via .ping) and
	 * keeps petting it from the kernel until userspace opens /dev/watchdog --
	 * requires CONFIG_WATCHDOG_HANDLE_BOOT_ENABLED (else the core leaves it
	 * unpetted, but since our hardware only arms on the first write it simply
	 * stays disarmed rather than resetting).
	 */
	if (arm_on_boot)
		set_bit(WDOG_HW_RUNNING, &w->wdd.status);

	ret = devm_watchdog_register_device(dev, &w->wdd);
	if (ret)
		return ret;

	dev_info(dev, "registered (hw time-out %u ms%s)\n", hw_margin_ms,
		 (w->wdd.bootstatus & WDIOF_CARDRESET) ?
		 ", last reset was watchdog" : "");

	/*
	 * Tick-driven hard-IRQ petter (see hardpet).  Arm the hardware now and pet
	 * it every quarter-period from a REL_HARD hrtimer, which on this
	 * clockevent-less board is serviced from the periodic tick in hard-IRQ
	 * context -- so petting no longer depends on the core's SCHED_FIFO kthread
	 * being scheduled.  Only started when the watchdog is armed at boot.
	 */
	if (arm_on_boot && hardpet) {
		writeb(0, w->trigger);		/* arm + first pet */
		w->hardpet_interval = ms_to_ktime(max(hw_margin_ms / 4, 1u));
		hrtimer_setup(&w->hardpet_timer, e17_wdt_hardpet, CLOCK_MONOTONIC,
			      HRTIMER_MODE_REL_HARD);
		hrtimer_start(&w->hardpet_timer, w->hardpet_interval,
			      HRTIMER_MODE_REL_HARD);
		ret = devm_add_action_or_reset(dev, e17_wdt_hardpet_cancel, w);
		if (ret)
			return ret;
		dev_info(dev, "hard-IRQ petting every %u ms\n", hw_margin_ms / 4);
	}

	/*
	 * Optional self-test (e17_wdt.selftest=1): arm the watchdog, then hang with
	 * interrupts off so nothing can pet it -- the hardware must then reset the
	 * board.  Self-terminating: the reset sets CARDRESET, so on the next boot we
	 * see it, report success and do NOT hang again.
	 */
	if (selftest) {
		if (w->wdd.bootstatus & WDIOF_CARDRESET) {
			dev_info(dev, "SELFTEST PASS: the watchdog reset the board\n");
		} else {
			dev_warn(dev, "SELFTEST: arming + hanging to force a watchdog reset...\n");
			e17_wdt_start(&w->wdd);
			local_irq_disable();
			mdelay(hw_margin_ms * 5);   /* > time-out; no pet can occur */
			local_irq_enable();
			dev_err(dev, "SELFTEST FAIL: still running after %u ms\n",
				hw_margin_ms * 5);
		}
	}
	return 0;
}

static const struct of_device_id e17_wdt_of_match[] = {
	{ .compatible = "eltec,e17-wdt" },
	{ }
};
MODULE_DEVICE_TABLE(of, e17_wdt_of_match);

static struct platform_driver e17_wdt_driver = {
	.probe	= e17_wdt_probe,
	.driver	= {
		.name		= "e17-wdt",
		.of_match_table	= e17_wdt_of_match,
	},
};
module_platform_driver(e17_wdt_driver);

MODULE_DESCRIPTION("ELTEC EUROCOM-17 watchdog");
MODULE_LICENSE("GPL");
