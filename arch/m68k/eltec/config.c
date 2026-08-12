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
#define E17_POST(code)	(*(volatile u8 *)0xfec30000 = (code))

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
#define CD2401_IER_TXMPTY	0x02	/* transmitter fully idle */
#define CD2401_LICR		0x26	/* (in service) interrupting channel << 2 */
#define CD2401_TFTC		0x80	/* (in tx service) writable byte count */
#define CD2401_REOIR		0x84	/* rx end-of-interrupt */
#define CD2401_TEOIR		0x85	/* tx end-of-interrupt */
#define CD2401_MEOIR		0x86	/* modem end-of-interrupt */
#define CD2401_EOIR_NOTRANS	0x08	/* "no data transferred" end-of-interrupt */
#define CD2401_RISRH		0x88	/* rx interrupt status (reading clears specials) */
#define CD2401_RISRL		0x89
#define CD2401_TPILR		0xe0	/* tx priority interrupt level */
#define CD2401_RPILR		0xe1	/* rx priority interrupt level */
#define CD2401_MPILR		0xe3	/* modem priority interrupt level */
#define CD2401_DR		0xf8	/* rx/tx data register */

#define E17_CD2401_IPL		2	/* one level for all three PILRs (RMON-style) */
#define E17_CD2401_TIMEOUT	0x200000 /* ~1-4s of uncached VIC reads; >> 17 char times */
#define E17_CD2401_RETRIES	16	/* foreign-service dismissals per batch */

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
 * CD2401 transmit -- see /workspace/files/CD2401-TX-DESIGN.md.
 *
 * The chip only moves data in interrupt-service context (a bare TDR write is
 * ignored), entered by reading the board IACK window.  The one way to wedge
 * the CPU is to issue an IACK whose level matches no internally-posted
 * interrupt: that bus cycle never gets a DTACK and stalls until the watchdog.
 * u-boot walks into this because it splits the priority levels and leaves rx
 * enabled, so typing during tx collides ("locks up if you send too fast").
 *
 * We follow RMON's scheme instead: program all three PILRs to the SAME level,
 * so an IACK issued while the (shared) VIC line is asserted is always answered
 * by *some* pending interrupt; the ack byte's low two bits say which type
 * arrived, and we dismiss anything that isn't our channel-0 tx service with a
 * NOTRANS end-of-interrupt.  One IACK, one TEOIR, no re-arm.
 */

/* One-time interrupt plumbing; channel format/baud are inherited from the firmware. */
static void e17_cd2401_init(void)
{
	int ch;

	for (ch = 3; ch >= 0; ch--) {		/* silence ALL channels (u-boot misses ch3) */
		e17_cd2401[CD2401_CAR] = ch;
		e17_cd2401[CD2401_IER] = 0;
	}
	/* equal PILRs: an IACK at this level is answered by any pending type */
	e17_cd2401[CD2401_TPILR] = E17_CD2401_IPL;
	e17_cd2401[CD2401_RPILR] = E17_CD2401_IPL;
	e17_cd2401[CD2401_MPILR] = E17_CD2401_IPL;

	/*
	 * NB: deliberately do NOT set the VIC LICR6 mask bit here.  On this VIC
	 * masking the line (bit 7) also stops its STATE bit (bit 3) from
	 * tracking the CD2401 -- which is exactly the signal we poll -- so the
	 * console would time out on every byte.  Interrupts are masked at the
	 * CPU during early boot; a proper irqchip owns LIRQ6 masking later.
	 */
}

/*
 * Enter a channel-0 transmit service.  Returns 1 on success (caller writes the
 * bytes, then TEOIR=0 and IER=0), 0 on timeout/failure (IER already cleared,
 * caller drops the data).  Never IACKs unless the shared line is asserted;
 * foreign services (rx/modem/other channel) are dismissed with a NOTRANS EOI
 * and the poll retried, which also scavenges anything stale left by RMON/u-boot.
 */
static int e17_cd2401_enter_tx_service(void)
{
	int retry;

	e17_cd2401[CD2401_CAR] = 0;
	e17_cd2401[CD2401_IER] = CD2401_IER_TXMPTY | CD2401_IER_TXD;

	for (retry = 0; retry < E17_CD2401_RETRIES; retry++) {
		long t = E17_CD2401_TIMEOUT;
		u8 ack;

		while ((e17_vic[E17_VIC_LICR6] & E17_VIC_LICR_STATE) && --t)
			cpu_relax();		/* STATE is active low */
		if (!t)
			break;			/* stalled/absent uart: drop */

		ack = e17_cd2401_iack[E17_CD2401_IPL];	/* the one IACK */
		switch (ack & 3) {
		case 2:					/* transmit */
			if (!(e17_cd2401[CD2401_LICR] & 0x0c))
				return 1;		/* ch0 tx service entered */
			e17_cd2401[CD2401_TEOIR] = CD2401_EOIR_NOTRANS;
			break;			/* another channel: dismiss */
		case 3:					/* rx data: discard */
		case 0:					/* rx exception: RISR read clears it */
			(void)e17_cd2401[CD2401_RISRH];
			(void)e17_cd2401[CD2401_RISRL];
			e17_cd2401[CD2401_REOIR] = CD2401_EOIR_NOTRANS;
			break;
		case 1:					/* modem */
			e17_cd2401[CD2401_MEOIR] = CD2401_EOIR_NOTRANS;
			break;
		}
	}
	e17_cd2401[CD2401_IER] = 0;
	return 0;
}

/* transmit raw bytes; batches of up to TFTC (<=16) per service */
static void e17_cd2401_tx(const char *s, unsigned int n)
{
	while (n) {
		unsigned int cnt;

		if (!e17_cd2401_enter_tx_service())
			return;				/* drop the rest, never hang */

		cnt = e17_cd2401[CD2401_TFTC];
		if (cnt > 16)
			cnt = 16;
		if (!cnt) {				/* shouldn't happen; be paranoid */
			e17_cd2401[CD2401_TEOIR] = CD2401_EOIR_NOTRANS;
			e17_cd2401[CD2401_IER] = 0;
			continue;
		}
		if (cnt > n)
			cnt = n;
		n -= cnt;
		while (cnt--)
			e17_cd2401[CD2401_DR] = *s++;
		e17_cd2401[CD2401_TEOIR] = 0;		/* data transferred */
		e17_cd2401[CD2401_IER] = 0;		/* no re-arm, no 2nd ack */
	}
}

static void e17_cd2401_write(const char *s, unsigned int n)
{
	static bool inited;

	if (!inited) {
		e17_cd2401_init();
		inited = true;
	}
	while (n) {
		unsigned int i = 0;

		while (i < n && s[i] != '\n')
			i++;
		e17_cd2401_tx(s, i);
		s += i; n -= i;
		if (n) {				/* s[0] == '\n' */
			e17_cd2401_tx("\r\n", 2);
			s++; n--;
		}
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

#define E17_IRQ_TIMER		IRQ_USER	/* CTVEC = VEC_USER */

static volatile u8 *const e17_cio2 = (volatile u8 *)E17_CIO2_BASE;

/* write value V to indirect CIO register REG via the control port */
static void e17_cio_wr(u8 reg, u8 val)
{
	e17_cio2[E17_CIO_CTRL] = reg;
	e17_cio2[E17_CIO_CTRL] = val;
}

static irqreturn_t e17_timer_int(int irq, void *dev_id)
{
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

	if (request_irq(E17_IRQ_TIMER, e17_timer_int, IRQF_TIMER, "timer",
			NULL))
		pr_err("E17: unable to register timer interrupt\n");

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

	/* bring up the boot console as early as possible */
	register_console(&e17_early_console);
}
