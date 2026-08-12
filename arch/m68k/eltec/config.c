// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  arch/m68k/eltec/config.c
 *
 *  Board setup for the ELTEC Eurocom E17 (VMEbus MC68040/060 SBC).
 *
 *  The board is described by its device tree (arch/m68k/dts/eltec-e17.dts,
 *  embedded in the kernel); this file provides the pieces the DT model
 *  cannot yet: machine identification, the low-level boot console, and
 *  interrupt setup.  Everything here is reverse-engineered from the RMON
 *  monitor ROM -- see E17-NOTES.md in the qemu-e17 tree.
 */

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/console.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/timex.h>
#include <linux/string.h>

#include <asm/bootinfo.h>
#include <asm/bootinfo-vme.h>
#include <asm/byteorder.h>
#include <asm/setup.h>
#include <asm/machdep.h>
#include <asm/irq.h>
#include <asm/traps.h>

#include "eltec.h"

/*
 * Boot diagnostics: drop a distinct code on the board's 7-seg POST display
 * (Z8536 CIO1 port C at 0xfec30000, low nibble shown) at each milestone so
 * the furthest point reached is visible without a serial console.  A plain
 * store -- it can never hang the CPU.  Codes 0xf1..0xf7 are used in
 * setup_arch(); this file continues the sequence into IRQ/timer bring-up.
 */
#define E17_WATCHDOG	0xfec50000
#define E17_POST(code)	do { \
		*(volatile u8 *)0xfec30000 = (code); \
		(void)*(volatile u8 *)E17_WATCHDOG;	/* pet watchdog too */ \
	} while (0)

/*
 * Pet the onboard watchdog (at 0xfec50000): a READ refreshes its deadline,
 * exactly as RMON does from its own tick handler.  RMON stops petting at
 * hand-off, so we must pet from our timer tick (and through early boot) or the
 * watchdog fires a level-7 NMI that vectors through a NULL vector to address 0.
 */
#define E17_WDPET()	((void)*(volatile u8 *)E17_WATCHDOG)

/*
 * Cirrus CD2401 serial controller at 0xfec64000, channel 0 = the RMON
 * console (Serial Port 1).  The real chip only transmits from its
 * interrupt-service context - a bare TDR write is ignored - so a byte is
 * sent the way the firmware does: enable the tx interrupt, wait for the
 * VIC to see the line asserted, acknowledge it through the board IACK
 * window to enter tx service, write the byte and end with TEOIR.
 */
#define E17_CD2401_BASE		0xfec64000
#define E17_CD2401_IACK		0xfec66000	/* interrupt-acknowledge window */
#define E17_VIC_BASE		0xfec00000
#define E17_VIC_LICR6		0x3b		/* CD2401 local interrupt */
#define E17_VIC_LICR_STATE	0x08		/* raw pin level (active low) */
#define CD2401_CAR		0xee	/* channel access (select) register */
#define CD2401_IER		0x11	/* interrupt enable register */
#define CD2401_IER_TXD		0x01
#define CD2401_IER_TXMPTY	0x02	/* transmitter fully idle (re-posts reliably) */
#define CD2401_LICR		0x26	/* local interrupt (which channel) */
#define CD2401_TFTC		0x80	/* tx fifo transfer count (free space) */
#define CD2401_TEOIR		0x85	/* transmit end of interrupt */
#define CD2401_TEOIR_NOTRANS	0x08	/* "no transfer" end-of-interrupt */
#define CD2401_TPILR		0xe0	/* tx priority interrupt level */
#define CD2401_TIR		0xec	/* tx interrupt register */
#define CD2401_TIR_TACT		0x40	/* tx service active */
#define CD2401_TX_IPL		0x02
#define CD2401_DR		0xf8	/* rx/tx data register */

/*
 * The onboard I/O window (0xfec00000) is reached at its physical
 * address; RMON leaves it transparently translated and the kernel's
 * early 040 mapping keeps it accessible, so no ioremap is needed for
 * this polled boot console (as on mvme16x).
 */
static volatile u8 *const e17_cd2401 = (volatile u8 *)E17_CD2401_BASE;
static volatile u8 *const e17_cd2401_iack __maybe_unused =
	(volatile u8 *)E17_CD2401_IACK;
static volatile u8 *const e17_vic = (volatile u8 *)E17_VIC_BASE;

/*
 * Enter the CD2401 tx interrupt-service context: wait (bounded) for the VIC
 * to see the tx line asserted, acknowledge through the board IACK window to
 * read the vector, and confirm the tx service is active for our channel.
 * Mirrors u-boot's serial_cd2401_ack_tx_irq().  Returns 0 on success.
 */
static int e17_cd2401_ack_tx(void)
{
	int outer = 64;			/* wait long enough for the fifo to drain */

	while (outer--) {
		int inner = 40000;

		/* VIC LICR6 STATE is active low: wait for it to go low */
		while ((e17_vic[E17_VIC_LICR6] & E17_VIC_LICR_STATE) && --inner)
			cpu_relax();
		if (!inner)
			return -1;

		(void)e17_cd2401_iack[CD2401_TX_IPL];	/* IACK -> read vector */

		/* tx service must be active */
		if (!(e17_cd2401[CD2401_TIR] & CD2401_TIR_TACT))
			continue;

		/* must be channel 0 (LICR interrupting-port field) */
		if (((e17_cd2401[CD2401_LICR] >> 2) & 3) != 0) {
			e17_cd2401[CD2401_TEOIR] = CD2401_TEOIR_NOTRANS;
			continue;
		}
		return 0;
	}
	return -1;
}

/*
 * Transmit a run of bytes the way the firmware does, in fifo-sized batches.
 * The important property (vs the earlier one-service-per-byte version that
 * died on the real chip) is that a whole batch is written into the tx fifo
 * *before* the closing TEOIR: once the bytes are in the fifo they transmit
 * regardless of whether the fragile per-service re-arm succeeds.  Newlines
 * are expanded to CR/LF.  Bytes are dropped (not hung) if we cannot enter
 * the tx service.
 */
/*
 * Only IER=TxD (0x01) transmits on this board, and TxD does not reliably
 * re-post once the fifo has filled -- so we cannot rely on re-entering the tx
 * service to drain a backlog.  Instead we PACE: write a batch, then busy-wait
 * long enough for those bytes to physically leave the shift register before
 * the next batch, so every service starts from an empty fifo where TxD is
 * guaranteed to assert.  Slow but reliable and never drops.
 */
#define E17_CD2401_DRAIN_PER_BYTE	40000	/* ~ one char-time at the line rate */

static void e17_cd2401_write(const char *s, unsigned int n)
{
	e17_cd2401[CD2401_CAR] = 0;		/* select channel 0 */
	e17_cd2401[CD2401_TPILR] = CD2401_TX_IPL;

	while (n) {
		int space, written = 0;
		long drain;

		e17_cd2401[CD2401_IER] = CD2401_IER_TXD;	/* enable tx irq */
		if (e17_cd2401_ack_tx() != 0) {		/* enter tx service */
			e17_cd2401[CD2401_IER] = 0;
			return;				/* give up (drop rest) */
		}

		/* fill the fifo (leave room for a possible CR before an LF) */
		space = e17_cd2401[CD2401_TFTC];
		while (space >= 2 && n) {
			char c = *s++;

			n--;
			if (c == '\n') {
				e17_cd2401[CD2401_DR] = '\r';
				space--;
				written++;
			}
			e17_cd2401[CD2401_DR] = c;
			space--;
			written++;
		}

		e17_cd2401[CD2401_TEOIR] = 0;		/* transfer the batch */
		e17_cd2401[CD2401_IER] = 0;		/* disable tx irq */

		/*
		 * Drain after EVERY batch (including the last) so this call
		 * returns with an empty fifo -- otherwise the next, separate
		 * write starts against a still-full fifo and drops (this was the
		 * "[arch1:entry][" cutoff).
		 */
		for (drain = (long)written * E17_CD2401_DRAIN_PER_BYTE;
		     drain > 0; drain--)
			cpu_relax();
	}
}

static void e17_cons_write(struct console *co, const char *s, unsigned int n)
{
	e17_cd2401_write(s, n);
}

/*
 * Low-level reliable serial puts, usable anywhere in early boot (before the
 * console is registered) to trace where the kernel gets to.
 */
void e17_early_puts(const char *s)
{
	e17_cd2401_write(s, strlen(s));
}

/* trace helper: print a value as 8 hex digits (for early bootinfo tracing) */
void e17_early_puthex(unsigned long v)
{
	static const char hex[] = "0123456789abcdef";
	char buf[9];
	int i;

	for (i = 7; i >= 0; i--) {
		buf[i] = hex[v & 0xf];
		v >>= 4;
	}
	buf[8] = 0;
	e17_cd2401_write(buf, 8);
}

static struct console e17_early_console = {
	.name	= "e17cons",
	.write	= e17_cons_write,
	.flags	= CON_PRINTBUFFER | CON_BOOT,
	.index	= -1,
};

int __init eltec_e17_parse_bootinfo(const struct bi_record *bi)
{
	uint16_t tag = be16_to_cpu(bi->tag);

	/* accept (ignore) the VME board-info tags the bootloader may pass */
	if (tag == BI_VME_TYPE || tag == BI_VME_BRDINFO)
		return 0;
	return 1;
}

static void eltec_e17_get_model(char *model)
{
	sprintf(model, "ELTEC Eurocom E17");
}

/*
 * Periodic tick.  The E17 has no PCC-style timer; the system tick is
 * generated by counter/timer 3 of the "user" Zilog Z8536 CIO at
 * 0xfec10000, whose interrupt is routed through local IRQ 1 of the
 * VIC068A (register LICR1).  The CIO clock (PCLK) runs at 2.5MHz, so a
 * time constant of PCLK/HZ gives the tick.  This mirrors what the RMON
 * monitor and the VxWorks BSP program, and matches the qemu e17 model.
 *
 * The VIC delivers the interrupt as an m68k vectored interrupt using the
 * vector programmed into the CIO's CTVEC register; we use the first user
 * vector, which the generic user-vector setup maps to IRQ_USER.
 */
/* E17_VIC_BASE / e17_vic are defined with the boot console above */
#define E17_VIC_LICR1		0x27	/* local IRQ 1 control (CIO timers) */
#define E17_VIC_LICR_LEVEL(l)	((l) & 0x07)	/* bit7=0 -> unmasked */

#define E17_CIO2_BASE		0xfec10000
#define E17_CIO_CTRL		3	/* control port (register 0-2 = ports) */

/* Z8536 register numbers */
#define Z8536_MICR		0x00	/* master interrupt control */
#define Z8536_MICR_MIE		0x80
#define Z8536_MICR_RESET	0x01
#define Z8536_MCCR		0x01	/* master config control */
#define Z8536_MCCR_CT3E		0x10
#define Z8536_CTVEC		0x04	/* counter/timer interrupt vector */
#define Z8536_CT3CS		0x0c	/* CT3 command and status */
#define Z8536_CT3MODE		0x1e	/* CT3 mode specification */
#define Z8536_CT3MODE_CONT	0x80	/* continuous (auto-reload) */
#define Z8536_CT3TC_MSB		0x1a
#define Z8536_CT3TC_LSB		0x1b
/* CT command/status writes */
#define Z8536_CMD_CLR_IPUS	0x20	/* clear IP and IUS */
#define Z8536_CMD_SET_IE	0xc0	/* set interrupt enable */
#define Z8536_CS_TCB		0x02	/* trigger command bit */

#define E17_CIO_PCLK		2500000
#define E17_TICK_TC		(E17_CIO_PCLK / HZ)

/*
 * The Z8536 delivers the CT3 interrupt with its source status folded into the
 * vector: with CTVEC = VEC_USER the counter/timer 3 interrupt actually arrives
 * on vector VEC_USER+2 (observed on real hardware: the timer fires at 0x42,
 * not 0x40).  The generic user-vector setup maps that to IRQ_USER+2, so that
 * is the IRQ the handler must claim -- claiming IRQ_USER leaves CT3 unserviced
 * and it storms.
 */
#define E17_CT3_VIS_OFFSET	2
#define E17_IRQ_TIMER		(IRQ_USER + E17_CT3_VIS_OFFSET)

static volatile u8 *const e17_cio2 = (volatile u8 *)E17_CIO2_BASE;

/* write value V to indirect CIO register REG via the control port */
static void e17_cio_wr(u8 reg, u8 val)
{
	e17_cio2[E17_CIO_CTRL] = reg;
	e17_cio2[E17_CIO_CTRL] = val;
}

static irqreturn_t e17_timer_int(int irq, void *dev_id)
{
	E17_WDPET();			/* pet the watchdog every tick, as RMON does */
	E17_POST(0xfd);			/* 'd': first/every timer interrupt */
	/* acknowledge: clear CT3's interrupt-pending (it auto-reloads) */
	e17_cio_wr(Z8536_CT3CS, Z8536_CMD_CLR_IPUS);
	legacy_timer_tick(1);
	return IRQ_HANDLED;
}

static void __init eltec_e17_sched_init(void)
{
	/*
	 * Reset the CIO, then bring it out of reset.  A control-port read
	 * syncs the internal pointer/data flip-flop; pointing at the MICR
	 * and writing the RESET bit puts the chip in reset, after which a
	 * single control write of 0 (which, while RESET is asserted, goes
	 * straight to the MICR) clears it.
	 */
	(void)e17_cio2[E17_CIO_CTRL];		/* sync the pointer flip-flop */
	e17_cio_wr(Z8536_MICR, Z8536_MICR_RESET);
	e17_cio2[E17_CIO_CTRL] = 0;		/* clear reset */

	/* interrupt vector delivered by the CIO on the tick */
	e17_cio_wr(Z8536_CTVEC, VEC_USER);

	/* program CT3: continuous mode, PCLK/HZ time constant */
	e17_cio_wr(Z8536_CT3MODE, Z8536_CT3MODE_CONT);
	e17_cio_wr(Z8536_CT3TC_MSB, E17_TICK_TC >> 8);
	e17_cio_wr(Z8536_CT3TC_LSB, E17_TICK_TC & 0xff);

	/* enable CT3 and master interrupts */
	e17_cio_wr(Z8536_MCCR, Z8536_MCCR_CT3E);
	e17_cio_wr(Z8536_MICR, Z8536_MICR_MIE);

	/*
	 * Claim both the base CTVEC IRQ and the +2 status-folded IRQ: the real
	 * Z8536 folds CT3's status into the vector and delivers VEC_USER+2,
	 * while the QEMU model delivers CTVEC (VEC_USER) verbatim.  Registering
	 * both makes the same kernel work on hardware and in emulation; only the
	 * one that actually fires is used.
	 */
	if (request_irq(E17_IRQ_TIMER, e17_timer_int, IRQF_TIMER, "timer",
			NULL))
		pr_err("E17: unable to register timer interrupt\n");
	if (request_irq(IRQ_USER, e17_timer_int, IRQF_TIMER, "timer-base",
			NULL))
		pr_err("E17: unable to register base timer interrupt\n");

	/* route VIC local IRQ 1 (CIO timers) to CPU IPL 6, unmasked */
	e17_vic[E17_VIC_LICR1] = E17_VIC_LICR_LEVEL(6);

	/* enable CT3 interrupt and start it counting */
	e17_cio_wr(Z8536_CT3CS, Z8536_CMD_SET_IE);
	e17_cio_wr(Z8536_CT3CS, Z8536_CS_TCB);
}

static void __init eltec_e17_init_IRQ(void)
{
	E17_POST(0xf8);			/* '8': init_IRQ */
	/* onboard interrupters are routed through the VIC068A; a proper
	 * irqchip is TODO -- use the generic user-vector setup for now. */
	m68k_setup_user_interrupt(VEC_USER, 192);
}

void __init config_eltec_e17(void)
{
	mach_init_IRQ	= eltec_e17_init_IRQ;
	mach_sched_init	= eltec_e17_sched_init;
	mach_get_model	= eltec_e17_get_model;

	if (!vme_brdtype)
		vme_brdtype = VME_TYPE_E17;

	/*
	 * DIAGNOSTIC: do NOT register the serial console.  The CD2401 tx can
	 * wedge the bus (IACK-with-nothing-pending), so every printk that
	 * flushes through it risks hanging the boot.  With the console off,
	 * printk is buffered and the boot runs silently -- progress is read
	 * from the POST display milestones instead.  Re-enable once the tx is
	 * bulletproof.
	 */
	if (0)
		register_console(&e17_early_console);
}
