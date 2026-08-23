// SPDX-License-Identifier: GPL-2.0
/*
 * Zilog Z8536 CIO driver for the ELTEC EUROCOM-17 (m68k dual-68040 VME).
 *
 * The board has two Z8536 CIOs on the 0xfec00000 I/O window:
 *
 *   System CIO ($FEC3.0000) - Port C drives the 4-bit 7-seg POST display,
 *                             Port B reads the config DIP switches, and its
 *                             three counter/timers are nominally "internal
 *                             control".  Register order: C(+0) B(+1) A(+2)
 *                             Control(+3)  -- the standard Z8536 layout.
 *   User   CIO ($FEC1.0000) - the tick (CT3, driven directly by config.c) plus
 *                             the parallel port.  Register order is DIFFERENT:
 *                             C(+0) A(+1) B(+2) Control(+3)  -- ports A and B
 *                             are swapped (HW manual Table 36; "same as the
 *                             EUROCOM-6").
 *
 * Only ports A/B differ; Port C (+0) and the Control port (+3) are identical on
 * both, and this driver only touches those two, so the swap does not affect it
 * today -- but the layout is carried in the match data so the driver is correct
 * for either CIO when we drive the User CIO's parallel port later.
 *
 * What this driver does now, bound to the System CIO (the one Linux otherwise
 * leaves idle):
 *   - stands up a 32-bit free-running CLOCKSOURCE from the linked CT1/CT2
 *     cascade (CT1 clocks CT2), at PCLK/2 = 2.5 MHz.  This mirrors the proven
 *     u-boot z8536_timer, except we do NOT MICR-reset the chip (u-boot only
 *     ever reset the *User* CIO; a full reset here would blow away Port C's
 *     7-seg output config, the DIP-switch port and any internal-control state),
 *     so CT1/CT2 are brought up with a read-modify-write instead.
 *   - takes ownership of Port C (the 7-seg): configures it as output and
 *     exposes e17_cio_display().  NOTE the earliest POST markers (head.S,
 *     smp_secondary.S, and config.c's E17_POST during setup_arch) necessarily
 *     stay as raw pokes -- they run long before this driver probes.
 *
 * A clockevent from a CT interrupt is future work: the CIO IRQ is on the VIC's
 * LIRQ6 self-vectored daisy chain (level 5, shared with the CD2401), which is
 * not wired up yet -- so a clocksource (no IRQ needed) is all we register here.
 */
#include <linux/clocksource.h>
#include <linux/e17_breadcrumb.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>

#include <asm/e17-cio.h>

/* Z8536 internal register numbers (reached through the control port). */
#define Z8536_CFG_CTRL		0x01	/* master configuration control      */
#define  Z8536_CFG_CT1E		BIT(6)	/* counter/timer 1 enable            */
#define  Z8536_CFG_CT2E		BIT(5)	/* counter/timer 2 enable            */
#define  Z8536_CFG_PCE_CT3E	BIT(4)	/* Port C & CT3 enable               */
#define  Z8536_CFG_LC_MASK	0x03	/* CT1/CT2 link control              */
#define  Z8536_CFG_LC_CLK	0x03	/* CT1 clocks CT2 (32-bit cascade)   */
#define Z8536_CT1_CMDSTAT	0x0a
#define Z8536_CT2_CMDSTAT	0x0b
#define  Z8536_CT_RCC		BIT(3)	/* read-counter-control (latch)      */
#define  Z8536_CT_GCB		BIT(2)	/* gate command bit                  */
#define  Z8536_CT_TCB		BIT(1)	/* trigger command bit               */
#define  Z8536_CT_CIP		BIT(0)	/* count-in-progress (status)        */
#define Z8536_CT1_VAL_MSB	0x10
#define Z8536_CT1_VAL_LSB	0x11
#define Z8536_CT2_VAL_MSB	0x12
#define Z8536_CT2_VAL_LSB	0x13
#define Z8536_CT1_RELOAD_MSB	0x16
#define Z8536_CT1_RELOAD_LSB	0x17
#define Z8536_CT2_RELOAD_MSB	0x18
#define Z8536_CT2_RELOAD_LSB	0x19
#define Z8536_CT1_MODE		0x1c
#define  Z8536_CT_MODE_CSC	BIT(7)	/* continuous (auto-reload)          */
#define Z8536_PC_DD		0x06	/* Port C data-direction (0 = output) */

/* CT3 + interrupt control, for the counter interrupt (LIRQ6, CTVEC=0x60). */
#define Z8536_MICR		0x00	/* master interrupt control          */
#define  Z8536_MICR_MIE		BIT(7)	/* master interrupt enable           */
#define  Z8536_MICR_NV		BIT(5)	/* no vector (0 = supply CTVEC)       */
#define Z8536_CTVEC		0x04	/* counter/timer interrupt vector    */
#define Z8536_CT3_CMDSTAT	0x0c
#define  Z8536_CT_IE		BIT(6)	/* interrupt enable                  */
#define  Z8536_CT_IP		BIT(5)	/* interrupt pending (status)        */
#define  Z8536_CMD_CLR_IPIUS	0x20	/* command: clear IP & IUS           */
#define  Z8536_CMD_SET_IE	0xc0	/* command: set IE                   */
#define Z8536_CT3_RELOAD_MSB	0x1a
#define Z8536_CT3_RELOAD_LSB	0x1b
#define Z8536_CT3_MODE		0x1e

#define E17_CIO_CTVEC		0x60	/* System CIO counter vector (DT-matched) */

/* PCLK is 5 MHz; the counters clock at PCLK/2. */
#define E17_CIO_CLK_HZ		2500000

/* Byte offsets of the two directly-addressed registers (same on both CIOs). */
struct e17_cio_layout {
	u8 portc;	/* Port C data register byte offset */
	u8 porta;	/* Port A data register byte offset */
	u8 portb;	/* Port B data register byte offset */
	u8 ctrl;	/* control port byte offset         */
};

static const struct e17_cio_layout e17_cio_layout_system = {
	.portc = 0, .portb = 1, .porta = 2, .ctrl = 3,	/* standard Z8536 */
};

static const struct e17_cio_layout e17_cio_layout_user = {
	.portc = 0, .porta = 1, .portb = 2, .ctrl = 3,	/* A/B swapped    */
};

struct e17_cio {
	void __iomem			*base;
	const struct e17_cio_layout	*layout;
	raw_spinlock_t			lock;	/* control-port protocol */
	struct clocksource		cs;
	int				irq;	/* CT interrupt (<=0 = none) */
};

/* The System CIO instance that owns the 7-seg, for e17_cio_display(). */
static struct e17_cio *e17_cio_display_dev;

/*
 * Control-port register access.  The Z8536 control port has a pointer/data
 * flip-flop: write the register number, then read or write its value.  Callers
 * hold ->lock so a read and a write can't interleave and desync the flip-flop.
 */
static u8 e17_cio_rd(struct e17_cio *c, u8 reg)
{
	writeb(reg, c->base + c->layout->ctrl);
	return readb(c->base + c->layout->ctrl);
}

static void e17_cio_wr(struct e17_cio *c, u8 reg, u8 val)
{
	writeb(reg, c->base + c->layout->ctrl);
	writeb(val, c->base + c->layout->ctrl);
}

/* --- clocksource: linked CT1/CT2 32-bit down counter, read up-counting ----- */

/*
 * Bound on the torn-read retry.  A genuine torn read settles in one retry; this
 * loop runs under a raw spinlock with interrupts OFF on every timekeeping read,
 * so if the CT2 latch ever fails to agree (a Z8536 latch hiccup) an unbounded
 * loop would spin forever with IRQs off -- the tick would stop and the watchdog
 * would reset the board.  Cap it: a once-off possibly-torn value (a ~26 ms clock
 * blip) is vastly preferable to a hang.
 */
#define E17_CIO_TORN_MAX	8

static u64 e17_cio_cs_read(struct clocksource *cs)
{
	struct e17_cio *c = container_of(cs, struct e17_cio, cs);
	unsigned int retry = E17_CIO_TORN_MAX;
	unsigned long flags;
	u8 m1, l1, m2, l2, m2b, l2b;

	/*
	 * CT2 is the high half, CT1 the low; each RCC latches its own counter and
	 * the latches are several bus cycles apart while CT1 wraps every ~26 ms.
	 * Latch/read high, then low, then high again: if the high half moved, the
	 * low half wrapped between them (a torn read is off by 0x10000) -- retry.
	 */
	raw_spin_lock_irqsave(&c->lock, flags);
	e17_breadcrumb(E17_BC_PH_CIO_CS);	/* Z8536 CIO bus reads: no-DTACK = whole-bus hang */
	do {
		e17_cio_wr(c, Z8536_CT2_CMDSTAT, Z8536_CT_RCC | Z8536_CT_GCB);
		m2 = e17_cio_rd(c, Z8536_CT2_VAL_MSB);
		l2 = e17_cio_rd(c, Z8536_CT2_VAL_LSB);
		e17_cio_wr(c, Z8536_CT1_CMDSTAT, Z8536_CT_RCC | Z8536_CT_GCB);
		m1 = e17_cio_rd(c, Z8536_CT1_VAL_MSB);
		l1 = e17_cio_rd(c, Z8536_CT1_VAL_LSB);
		e17_cio_wr(c, Z8536_CT2_CMDSTAT, Z8536_CT_RCC | Z8536_CT_GCB);
		m2b = e17_cio_rd(c, Z8536_CT2_VAL_MSB);
		l2b = e17_cio_rd(c, Z8536_CT2_VAL_LSB);
	} while ((m2b != m2 || l2b != l2) && --retry);
	e17_breadcrumb(E17_BC_PH_NONE);
	raw_spin_unlock_irqrestore(&c->lock, flags);

	if (unlikely(!retry))
		pr_warn_ratelimited("e17-cio: clocksource torn read did not settle in %d tries\n",
				    E17_CIO_TORN_MAX);

	/* The counters count down; invert so the clocksource counts up. */
	return ~(((u32)m2 << 24) | ((u32)l2 << 16) | ((u32)m1 << 8) | l1);
}

static int e17_cio_setup(struct e17_cio *c)
{
	unsigned int timeout;
	u8 cfg;

	raw_spin_lock(&c->lock);

	/*
	 * Sync the control-port pointer/data flip-flop and, if the chip is held
	 * in reset, take it out.  A control-port READ returns the flip-flop to
	 * the register-select state; while the chip is in reset any control-port
	 * write clears MICR.RESET.  On real hardware RMON has already brought the
	 * CIO up (Port C = 7-seg output, DIP-switch Port B, ...), so this only
	 * re-syncs and does NOT wipe that state; on a bare chip (e.g. emulation
	 * with no firmware) the middle write takes it out of reset.
	 */
	(void)readb(c->base + c->layout->ctrl);
	writeb(0, c->base + c->layout->ctrl);
	(void)readb(c->base + c->layout->ctrl);

	/* Full-scale reload so the 32-bit cascade wraps only every ~28 min. */
	e17_cio_wr(c, Z8536_CT1_RELOAD_MSB, 0xff);
	e17_cio_wr(c, Z8536_CT1_RELOAD_LSB, 0xff);
	e17_cio_wr(c, Z8536_CT2_RELOAD_MSB, 0xff);
	e17_cio_wr(c, Z8536_CT2_RELOAD_LSB, 0xff);
	e17_cio_wr(c, Z8536_CT1_MODE, Z8536_CT_MODE_CSC);	/* continuous */

	/*
	 * Link CT1->CT2 (CT1 clocks CT2) for the 32-bit cascade, enable both
	 * counters and keep Port C (the 7-seg) enabled.  Read-modify-write so we
	 * preserve the other port-enable bits RMON set (e.g. DIP-switch Port B).
	 */
	cfg = e17_cio_rd(c, Z8536_CFG_CTRL);
	cfg = (cfg & ~Z8536_CFG_LC_MASK) | Z8536_CFG_LC_CLK | Z8536_CFG_PCE_CT3E;
	e17_cio_wr(c, Z8536_CFG_CTRL, cfg);
	cfg |= Z8536_CFG_CT1E | Z8536_CFG_CT2E;
	e17_cio_wr(c, Z8536_CFG_CTRL, cfg);

	/* Port C all-output for the 7-seg display. */
	e17_cio_wr(c, Z8536_PC_DD, 0x00);

	/* Gate + trigger both counters and confirm CT1 is counting. */
	e17_cio_wr(c, Z8536_CT1_CMDSTAT, Z8536_CT_GCB | Z8536_CT_TCB);
	e17_cio_wr(c, Z8536_CT2_CMDSTAT, Z8536_CT_GCB | Z8536_CT_TCB);

	for (timeout = 10000; timeout; timeout--)
		if (e17_cio_rd(c, Z8536_CT1_CMDSTAT) & Z8536_CT_CIP)
			break;

	raw_spin_unlock(&c->lock);
	return timeout ? 0 : -ETIMEDOUT;
}

/* --- Port C: the 7-seg POST display -------------------------------------- */

/*
 * Drive the 4-bit 7-seg display (low nibble of Port C).  Available once this
 * driver has probed; the pre-driver POST markers write Port C directly.
 */
void e17_cio_display(u8 val)
{
	struct e17_cio *c = e17_cio_display_dev;

	if (c)
		writeb(val & 0x0f, c->base + c->layout->portc);
}
EXPORT_SYMBOL_GPL(e17_cio_display);

/* --- CT3 counter interrupt (step toward a clockevent) -------------------- */

/*
 * Arming the CT3 interrupt hangs the bus on real hardware (the Z8536 does not
 * answer the level-5 self-vectored CPU IACK).  Kept off until understood; the
 * "e17_cio_ct3" boot arg re-enables it for experiments.
 */
static bool e17_cio_arm_ct3;
static int __init e17_cio_ct3_setup(char *s) { e17_cio_arm_ct3 = true; return 1; }
__setup("e17_cio_ct3", e17_cio_ct3_setup);

static atomic_t e17_cio_ct3_count = ATOMIC_INIT(0);

static irqreturn_t e17_cio_ct3_isr(int irq, void *dev_id)
{
	struct e17_cio *c = dev_id;
	unsigned long flags;
	bool ours;

	raw_spin_lock_irqsave(&c->lock, flags);
	ours = e17_cio_rd(c, Z8536_CT3_CMDSTAT) & Z8536_CT_IP;
	if (ours)
		e17_cio_wr(c, Z8536_CT3_CMDSTAT, Z8536_CMD_CLR_IPIUS);	/* ack */
	raw_spin_unlock_irqrestore(&c->lock, flags);

	if (!ours)
		return IRQ_NONE;
	if (atomic_inc_return(&e17_cio_ct3_count) == 1)
		pr_info("e17-cio: CT3 interrupt is being delivered (IRQ %d)\n", irq);
	return IRQ_HANDLED;
}

/*
 * Arm CT3 as a periodic (~26 ms) interrupt source so we can verify delivery of
 * a CIO counter interrupt through the VIC LIRQ6 self-vectored path.  This is the
 * stepping stone to a real clockevent; the counter is not yet the system tick.
 */
static void e17_cio_ct3_arm(struct e17_cio *c)
{
	raw_spin_lock(&c->lock);
	e17_cio_wr(c, Z8536_CTVEC, E17_CIO_CTVEC);	/* self-vector 0x60 */
	e17_cio_wr(c, Z8536_CT3_RELOAD_MSB, 0xff);
	e17_cio_wr(c, Z8536_CT3_RELOAD_LSB, 0xff);
	e17_cio_wr(c, Z8536_CT3_MODE, Z8536_CT_MODE_CSC);	/* continuous */
	e17_cio_wr(c, Z8536_MICR, Z8536_MICR_MIE);		/* master int enable */
	e17_cio_wr(c, Z8536_CT3_CMDSTAT, Z8536_CMD_SET_IE);	/* CT3 int enable */
	e17_cio_wr(c, Z8536_CT3_CMDSTAT, Z8536_CT_GCB | Z8536_CT_TCB);
	raw_spin_unlock(&c->lock);
}

/* --- CIO interrupt-routing probe (e17_cio_irqprobe boot arg) -------------- */
/*
 * Zero-risk diagnostic for resolving the parked CIO interrupt.  It answers "where
 * does each CIO's CT3 request actually land, and what state are the CIOs in?"
 * WITHOUT ever letting the CPU acknowledge an interrupt: it masks the VIC LIRQ4
 * and LIRQ6 inputs first, so no IACK cycle can be issued (nothing can hang the
 * bus), and only reads the LICR STATE bit -- the raw input-pin level, readable
 * while masked, 0 = asserted -- to see which VIC input a request drives for each
 * $FEC5.4001 SCIO/UCIO routing value.  Results are buffered and printed only
 * after all registers are restored (printk itself drives the LIRQ6 console).
 */
#define E17_VIC_PHYS		0xfec00000
#define E17_ILR_PHYS		0xfec54000	/* +1 = CIO/frame interrupt-level reg */
#define E17_UCIO_PHYS		0xfec10000	/* User CIO register window          */
#define E17_VIC_LICR1		0x27		/* LICRn = 0x27 + 4*(n-1)            */
#define E17_VIC_LICR4		0x33
#define E17_VIC_LICR6		0x3b
#define E17_VIC_LICR_MASK	0x80
#define E17_VIC_LICR_STATE	0x08		/* raw input pin, 0 = asserted      */
#define E17_ILR_SCIO		0x01		/* $fec54001 bit0: system CIO route */
#define E17_ILR_UCIO		0x02		/* $fec54001 bit1: user CIO route   */
#define E17_ILR_FR		0x04		/* $fec54001 bit2: frame route (set=LIRQ4) */
#define Z8536_CMD_CLR_IE	0xe0

static bool e17_cio_do_irqprobe __initdata;
static int __init e17_cio_irqprobe_setup(char *s)
{
	e17_cio_do_irqprobe = true;
	return 1;
}
__setup("e17_cio_irqprobe", e17_cio_irqprobe_setup);

static const char * __init e17_cio_where(u8 licr4, u8 licr6)
{
	bool l4 = !(licr4 & E17_VIC_LICR_STATE);
	bool l6 = !(licr6 & E17_VIC_LICR_STATE);

	if (l4 && l6)
		return "BOTH LIRQ4+LIRQ6(!)";
	if (l4)
		return "LIRQ4";
	if (l6)
		return "LIRQ6";
	return "neither";
}

/* Arm CT3 with a tiny reload so IP sets within a few counts; caller holds lock. */
static void __init e17_cio_ct3_arm_short(struct e17_cio *c)
{
	e17_cio_wr(c, Z8536_CT3_RELOAD_MSB, 0x00);
	e17_cio_wr(c, Z8536_CT3_RELOAD_LSB, 0x08);
	e17_cio_wr(c, Z8536_CT3_MODE, Z8536_CT_MODE_CSC);
	e17_cio_wr(c, Z8536_MICR, Z8536_MICR_MIE);	/* NV=0 -> supply CTVEC */
	e17_cio_wr(c, Z8536_CTVEC, E17_CIO_CTVEC);
	e17_cio_wr(c, Z8536_CT3_CMDSTAT, Z8536_CMD_SET_IE);
	e17_cio_wr(c, Z8536_CT3_CMDSTAT, Z8536_CT_GCB | Z8536_CT_TCB);
}

static int __init e17_cio_irqprobe_run(void)
{
	struct e17_cio *c = e17_cio_display_dev;
	void __iomem *vic = NULL, *ilrp = NULL, *ucio = NULL;
	unsigned long flags;
	u8 sav_licr4, sav_licr6, sav_ilr, sav_micr, sys_ctvec, sys_ct3cs;
	u8 licr[8] = {};
	u8 sys4[2], sys6[2];
	bool sys_ip[2];
	u8 u_micr, u_ctvec, u_ct3cs;
	int i;

	if (!e17_cio_do_irqprobe)
		return 0;
	if (!c) {
		pr_warn("e17-cio irqprobe: System CIO not up; skipping\n");
		return 0;
	}

	vic  = ioremap(E17_VIC_PHYS, 0x100);
	ilrp = ioremap(E17_ILR_PHYS, 0x10);
	ucio = ioremap(E17_UCIO_PHYS, 0x10);
	if (!vic || !ilrp || !ucio) {
		pr_warn("e17-cio irqprobe: ioremap failed\n");
		goto out_unmap;
	}

	raw_spin_lock_irqsave(&c->lock, flags);

	/* Snapshot (never touch $fec54000 offset 0 -- that is the I2C/watchdog reg). */
	for (i = 1; i <= 7; i++)
		licr[i] = readb(vic + E17_VIC_LICR1 + 4 * (i - 1));
	sav_licr4 = readb(vic + E17_VIC_LICR4);
	sav_licr6 = readb(vic + E17_VIC_LICR6);
	sav_ilr   = readb(ilrp + 1);
	sav_micr  = e17_cio_rd(c, Z8536_MICR);
	sys_ctvec = e17_cio_rd(c, Z8536_CTVEC);
	sys_ct3cs = e17_cio_rd(c, Z8536_CT3_CMDSTAT);

	/* SAFETY: mask both VIC inputs -- from here no CPU IACK can be issued. */
	writeb(sav_licr4 | E17_VIC_LICR_MASK, vic + E17_VIC_LICR4);
	writeb(sav_licr6 | E17_VIC_LICR_MASK, vic + E17_VIC_LICR6);

	/* P2: read the User CIO CT3 state config.c leaves armed every boot. */
	writeb(Z8536_MICR, ucio + 3);        u_micr  = readb(ucio + 3);
	writeb(Z8536_CTVEC, ucio + 3);       u_ctvec = readb(ucio + 3);
	writeb(Z8536_CT3_CMDSTAT, ucio + 3); u_ct3cs = readb(ucio + 3);

	/*
	 * Isolate the System CIO's request so LICR STATE reflects only it.  Two other
	 * sources can drive these inputs: the video frame on LIRQ4 (it asserts
	 * continuously) and the User CIO.  Silence both: clear the FR bit in
	 * $fec54001 (the board routes frame->LIRQ4 when FR is set -- confirmed 0xfc),
	 * and disable the User CIO's master interrupt.  The CD2401 on LIRQ6 is idle
	 * here (local IRQs off, no TX in progress).
	 */
	writeb(Z8536_MICR, ucio + 3); writeb(0x00, ucio + 3);	/* User CIO int off */

	/* P1: System CIO CT3 routing truth table for SCIO = 0 then 1 (FR cleared). */
	for (i = 0; i < 2; i++) {
		u8 ilr = (sav_ilr & ~(E17_ILR_SCIO | E17_ILR_FR)) |
			 (i ? E17_ILR_SCIO : 0);
		int t;

		writeb(ilr, ilrp + 1);
		e17_cio_ct3_arm_short(c);
		for (t = 5000; t; t--)
			if (e17_cio_rd(c, Z8536_CT3_CMDSTAT) & Z8536_CT_IP)
				break;
		sys_ip[i] = t != 0;
		sys4[i] = readb(vic + E17_VIC_LICR4);
		sys6[i] = readb(vic + E17_VIC_LICR6);
		e17_cio_wr(c, Z8536_CT3_CMDSTAT, Z8536_CMD_CLR_IPIUS);
		e17_cio_wr(c, Z8536_CT3_CMDSTAT, Z8536_CMD_CLR_IE);
	}

	/* Restore everything before we re-enable delivery. */
	writeb(Z8536_MICR, ucio + 3); writeb(u_micr, ucio + 3);	/* User CIO MICR */
	e17_cio_wr(c, Z8536_MICR, sav_micr);
	writeb(sav_ilr, ilrp + 1);
	writeb(sav_licr4, vic + E17_VIC_LICR4);
	writeb(sav_licr6, vic + E17_VIC_LICR6);

	raw_spin_unlock_irqrestore(&c->lock, flags);

	/* Report (safe now: printk uses the LIRQ6 console we just restored). */
	pr_info("e17-cio irqprobe: ILR($fec54001)=0x%02x LICR4=0x%02x LICR6=0x%02x (mask=b7, state=b3 0=asserted)\n",
		sav_ilr, sav_licr4, sav_licr6);
	pr_info("e17-cio irqprobe: LICR 1=%02x 2=%02x 3=%02x 4=%02x 5=%02x 6=%02x 7=%02x\n",
		licr[1], licr[2], licr[3], licr[4], licr[5], licr[6], licr[7]);
	pr_info("e17-cio irqprobe: System CIO MICR=%02x CTVEC=%02x CT3CS=%02x\n",
		sav_micr, sys_ctvec, sys_ct3cs);
	pr_info("e17-cio irqprobe: User   CIO MICR=%02x CTVEC=%02x CT3CS=%02x [IP=%d IE=%d IUS=%d CIP=%d]\n",
		u_micr, u_ctvec, u_ct3cs, !!(u_ct3cs & Z8536_CT_IP),
		!!(u_ct3cs & Z8536_CT_IE), !!(u_ct3cs & 0x80), !!(u_ct3cs & 0x01));
	for (i = 0; i < 2; i++)
		pr_info("e17-cio irqprobe: SCIO=%d -> System CT3 (IP=%d) request on %s  [frame isolated, User CIO masked]\n",
			i, sys_ip[i], e17_cio_where(sys4[i], sys6[i]));

out_unmap:
	if (vic)
		iounmap(vic);
	if (ilrp)
		iounmap(ilrp);
	if (ucio)
		iounmap(ucio);
	return 0;
}
late_initcall(e17_cio_irqprobe_run);

static int e17_cio_probe(struct platform_device *pdev)
{
	struct e17_cio *c;
	int ret;

	c = devm_kzalloc(&pdev->dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;

	c->layout = of_device_get_match_data(&pdev->dev);
	if (!c->layout)
		return -EINVAL;

	c->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(c->base))
		return PTR_ERR(c->base);

	raw_spin_lock_init(&c->lock);

	ret = e17_cio_setup(c);
	if (ret) {
		dev_err(&pdev->dev, "CT1/CT2 cascade did not start\n");
		return ret;
	}
	e17_cio_display_dev = c;	/* Port C (7-seg) now output-configured */

	c->cs.name	= "e17-cio";
	c->cs.rating	= 250;
	c->cs.read	= e17_cio_cs_read;
	c->cs.mask	= CLOCKSOURCE_MASK(32);
	c->cs.flags	= CLOCK_SOURCE_IS_CONTINUOUS;
	ret = clocksource_register_hz(&c->cs, E17_CIO_CLK_HZ);
	if (ret) {
		dev_err(&pdev->dev, "clocksource registration failed (%d)\n", ret);
		e17_cio_display_dev = NULL;
		return ret;
	}

	platform_set_drvdata(pdev, c);
	dev_info(&pdev->dev, "Z8536 CIO: 32-bit CT1/CT2 clocksource @ %u Hz, owns 7-seg\n",
		 E17_CIO_CLK_HZ);

	/*
	 * CT3 counter interrupt (VIC LIRQ6 self-vectored, CTVEC=0x60): DISABLED.
	 * On real hardware the first CT3 interrupt's level-5 CPU IACK hangs the
	 * bus -- the Z8536 does not answer the self-vectored acknowledge the way
	 * the CD2401 (PILR=0xfb) does.  The handler/arm and the DT wiring are kept
	 * for the next attempt once the Z8536 IACK/daisy-chain requirement is
	 * understood; enable via e17_cio_arm_ct3.
	 */
	if (e17_cio_arm_ct3) {
		c->irq = platform_get_irq_optional(pdev, 0);
		if (c->irq > 0) {
			ret = request_irq(c->irq, e17_cio_ct3_isr, 0,
					  "e17-cio-ct3", c);
			if (ret) {
				dev_err(&pdev->dev, "cannot get CT3 IRQ %u (%d)\n",
					c->irq, ret);
				c->irq = 0;
			} else {
				e17_cio_ct3_arm(c);
				dev_info(&pdev->dev, "CT3 interrupt armed on IRQ %u\n",
					 c->irq);
			}
		}
	}
	return 0;
}

static const struct of_device_id e17_cio_of_match[] = {
	{ .compatible = "eltec,e17-cio-system", .data = &e17_cio_layout_system },
	{ .compatible = "eltec,e17-cio-user",   .data = &e17_cio_layout_user   },
	{ }
};
MODULE_DEVICE_TABLE(of, e17_cio_of_match);

static struct platform_driver e17_cio_driver = {
	.driver = {
		.name		= "e17-cio",
		.of_match_table	= e17_cio_of_match,
		.suppress_bind_attrs = true,
	},
	.probe = e17_cio_probe,
};
builtin_platform_driver(e17_cio_driver);
