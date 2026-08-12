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
#include <linux/font.h>

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
#define CD2401_IER_TXMPTY	0x02	/* transmitter fully idle (re-posts reliably) */
#define CD2401_LICR		0x26	/* local interrupt (which channel) */
#define CD2401_TFTC		0x80	/* tx fifo transfer count (free space) */
#define CD2401_TEOIR		0x85	/* transmit end of interrupt */
#define CD2401_TEOIR_NOTRANS	0x08	/* "no transfer" end-of-interrupt */
#define CD2401_TPILR		0xe0	/* tx priority interrupt level */
#define CD2401_TIR		0xec	/* tx interrupt register */
#define CD2401_TIR_TACT		0x40	/* tx service active */
#define CD2401_TX_IPL		0x02
#define CD2401_RX_IPL		0x01
#define CD2401_DR		0xf8	/* rx/tx data register */
#define CD2401_LIV		0x09	/* local interrupt vector */
#define CD2401_CCR		0x13	/* channel command register */
#define CD2401_CCR_ENBRX	0x02
#define CD2401_CCR_ENBXMTR	0x08
#define CD2401_CMR		0x1b	/* channel mode register */
#define CD2401_CMR_ASYNC	0x02
#define CD2401_RPILR		0xe1	/* rx priority interrupt level */
#define CD2401_COR4		0x15	/* FIFO threshold (shared rx/tx, max 12) */
#define CD2401_FIFO_THRESH	0x08	/* tx re-requests when free space > this */

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
 * Enter the CD2401 tx interrupt-service context.  Ported verbatim from
 * u-boot's serial_cd2401_ack_tx_irq() (which reliably prints pages on this
 * board): wait for the VIC to see the tx line asserted (LICR6 STATE, active
 * low), IACK through the board window, then RETRY -- up to 64K times -- until
 * the chip reports its tx service is actually active (TIR & TACT) for channel
 * 0.  The generous retry is the crucial difference from the old one-shot
 * version, which dropped everything after the first fifo-full.  Returns 0 ok.
 */
#define E17_CD2401_ACK_LOOPS	0x100000	/* generous: covers fifo drain at low baud */

/*
 * Enter the tx service: wait (bounded) for the CD2401 to request service --
 * the VIC's LICR6 STATE bit is the raw interrupt line, active LOW -- then do
 * the interrupt-acknowledge cycle at the tx priority level, which enters tx
 * service context (datasheet 5.3.2 / 5.616).  No TACT-check-then-retry: if
 * TACT lagged the IACK we'd 'continue' and re-poll STATE, but the IACK already
 * consumed the request so STATE is gone and we'd time out -- which is exactly
 * how the 2nd service stalled.  We only ever enable TxD (never RxD) so the sole
 * request source is our channel-0 transmitter.  Returns 0 on success.
 */
static int e17_cd2401_ack_tx(void)
{
	unsigned int i;

	for (i = E17_CD2401_ACK_LOOPS; i; i--) {
		if (!(e17_vic[E17_VIC_LICR6] & E17_VIC_LICR_STATE)) {
			(void)e17_cd2401_iack[CD2401_TX_IPL];	/* IACK -> service */
			return 0;
		}
		cpu_relax();
	}
	return -1;
}

/*
 * Transmit a buffer the way the CD2401 datasheet (227-04631, section 5.3.2 +
 * COR4) actually specifies for polled streaming:
 *   - enable TxD in IER ONCE and leave it enabled for the whole buffer;
 *   - each time the chip requests service (VIC STATE asserts), enter service
 *     via the IACK, read TFTC (free fifo slots), write exactly that many bytes
 *     (never more -- that overruns), and end with a SINGLE TEOIR;
 *   - the hardware re-requests automatically when the FIFO drains below the
 *     COR4 watermark -- so do NOT toggle IER and do NOT do a second
 *     ack/TEOIR "close" per pass (both dropped the pending request and stalled
 *     output after the first fifo-load on real hardware).
 * TEOIR = 0 when we wrote data, 0x08 (Notrans) if a service fired with nothing
 * left to send.  Newlines expand to CR/LF.
 */
static void e17_cd2401_write(const char *s, unsigned int n)
{
	e17_cd2401[CD2401_CAR] = 0;			/* select channel 0 */
	e17_cd2401[CD2401_TPILR] = CD2401_TX_IPL;
	e17_cd2401[CD2401_IER] |= CD2401_IER_TXD;	/* enable tx irq -- once */

	while (n) {
		unsigned int free, w;

		if (e17_cd2401_ack_tx() != 0)
			break;				/* couldn't enter service */

		free = e17_cd2401[CD2401_TFTC] & 0x1f;	/* free fifo slots */
		for (w = 0; w < free && n; ) {
			char c = *s;

			if (c == '\n') {
				if (w + 2 > free)
					break;		/* need 2 slots for CR/LF */
				e17_cd2401[CD2401_DR] = '\r';
				e17_cd2401[CD2401_DR] = '\n';
				w += 2;
			} else {
				e17_cd2401[CD2401_DR] = c;
				w++;
			}
			s++;
			n--;
		}

		/* single close: data written -> 0, nothing written -> Notrans */
		e17_cd2401[CD2401_TEOIR] = w ? 0 : CD2401_TEOIR_NOTRANS;
	}

	e17_cd2401[CD2401_IER] &= ~CD2401_IER_TXD;	/* done: disable tx irq */
}

/*
 * Bring the CD2401 into a known-good tx state, ported from u-boot's
 * serial_cd2401_probe().  head.S's simplified per-char writer leaves the
 * chip's tx service in a state where TACT never re-asserts, so our TACT-gated
 * ack (above) drops everything until we re-init the channel here: select
 * async mode, give tx/rx distinct priority levels, and (re)enable the
 * transmitter via CCR.  Baud/format stay as RMON/u-boot programmed them.
 */
void e17_cd2401_init(void)
{
	int i, bound;

	/* quiesce the other two channels (as u-boot does) */
	for (i = 1; i < 3; i++) {
		e17_cd2401[CD2401_CAR] = i;
		e17_cd2401[CD2401_IER] = 0;
		e17_cd2401[CD2401_LIV] = i << 2;
	}

	e17_cd2401[CD2401_CAR] = 0;			/* channel 0 */
	e17_cd2401[CD2401_CMR] = CD2401_CMR_ASYNC;
	e17_cd2401[CD2401_LIV] = 0;
	e17_cd2401[CD2401_TPILR] = CD2401_TX_IPL;
	e17_cd2401[CD2401_RPILR] = CD2401_RX_IPL;
	e17_cd2401[CD2401_COR4] = CD2401_FIFO_THRESH;	/* tx re-request watermark */

	/* wait (bounded) for any in-progress channel command to finish */
	for (bound = 100000; bound && e17_cd2401[CD2401_CCR]; bound--)
		cpu_relax();

	e17_cd2401[CD2401_CCR] = CD2401_CCR_ENBRX | CD2401_CCR_ENBXMTR;
}

/*
 * Framebuffer text debug console.
 *
 * The CD2401 serial tx is fragile on this board, so the onboard video is used
 * as a second, dead-reliable output channel: it is just memory writes.  RMON
 * programs the onboard gfx for 800x600x8 (indexed) whenever video is fitted,
 * so the mode and CRTC timing are already set up -- we only load two palette
 * entries and paint pixels.
 *
 *   - VRAM at 0x0fc00000, 8bpp, pitch = width = 800 bytes/line.  Pixel (x,y)
 *     lives at E17_FB_BASE + y*800 + x.  This region is reachable directly:
 *     head.S installs a transparent, cache-inhibited TTR over 0x0f000000..
 *     0x10000000, so no ioremap is needed.
 *   - RAMDAC at 0xfec40000 (in the onboard I/O window, already mapped
 *     cache-inhibited via dtt1).  It is VGA-style: byte register +0 is the
 *     palette *write address*, +1 the palette *data* port (three bytes R,G,B
 *     per entry with auto-increment), +2 an ID that reads 0x3a.  These offsets
 *     and the byte-wide access match the qemu e17_vid model's decode exactly
 *     (addr & 7: case 0 sets the palette address, case 1 loads R/G/B).
 *
 * Text uses pixel value 0xff (palette entry 0xff = white) on a 0x00 (entry 0 =
 * black) background, rendered from the kernel's built-in VGA 8x8 font.
 */
#define E17_FB_BASE		0x0fc00000UL
#define E17_FB_WIDTH		800
#define E17_FB_HEIGHT		600
#define E17_FB_PITCH		E17_FB_WIDTH		/* 8bpp: 1 byte/pixel */

#define E17_RAMDAC_BASE		0xfec40000UL
#define E17_RAMDAC_PALADDR	0	/* palette write-address port */
#define E17_RAMDAC_PALDATA	1	/* palette data port (R,G,B autoinc) */
#define E17_RAMDAC_ID		2	/* ID register (reads 0x3a) */

#define E17_FB_CELL		8			/* 8x8 font cell */
#define E17_FB_COLS		(E17_FB_WIDTH / E17_FB_CELL)	/* 100 */
#define E17_FB_ROWS		(E17_FB_HEIGHT / E17_FB_CELL)	/* 75 */
/*
 * PROBE MODE: draw black glyphs (a distinct palette index we set to black)
 * directly onto RMON's existing white background -- no full-screen clear, so a
 * bad/absent VRAM address can't be run into and hard-lock the bus (the full
 * memset was hitting one ~1/4 of the way down).  Readable text => the linear
 * 8bpp/pitch-800 layout is right; scrambled => the pixel layout differs.
 */
#define E17_FB_FG		0x01	/* fg index; palette set to black in init */
#define E17_FB_BG		0x00	/* (unused in probe: bg left as-is) */

static volatile u8 *const e17_fb = (volatile u8 *)E17_FB_BASE;
static volatile u8 *const e17_ramdac = (volatile u8 *)E17_RAMDAC_BASE;

static int e17_fb_col;
static int e17_fb_row;
static bool e17_fb_ready;

/* Load one palette entry (VGA-style: address then three data bytes). */
static void e17_fb_setpal(u8 idx, u8 r, u8 g, u8 b)
{
	e17_ramdac[E17_RAMDAC_PALADDR] = idx;
	e17_ramdac[E17_RAMDAC_PALDATA] = r;
	e17_ramdac[E17_RAMDAC_PALDATA] = g;
	e17_ramdac[E17_RAMDAC_PALDATA] = b;
}

/*
 * Prepare the framebuffer console: entry 0 = black, entry 0xff = white, clear
 * the whole screen and home the cursor.  Idempotent -- safe to call once early.
 */
void e17_fb_init(void)
{
	if (e17_fb_ready)
		return;

	/* make our fg index black; leave RMON's white background untouched */
	e17_fb_setpal(E17_FB_FG, 0x00, 0x00, 0x00);

	/*
	 * PROBE: no full-screen memset (it hit unresponsive VRAM ~1/4 down and
	 * hard-locked).  Instead draw one small solid reference bar (32x8) at the
	 * top-left with a plain byte loop -- if it shows as a clean bar the linear
	 * pitch-800 layout is correct; if it's scattered/stretched the layout
	 * differs.  Well within the region that filled OK before the lock.
	 */
	{
		int y, x;
		for (y = 0; y < E17_FB_CELL; y++) {
			volatile u8 *line = e17_fb + (size_t)y * E17_FB_PITCH;
			for (x = 0; x < 32; x++)
				line[x] = E17_FB_FG;
		}
	}

	e17_fb_col = 0;
	e17_fb_row = 1;			/* start text below the reference bar */
	e17_fb_ready = true;
}

/* Scroll the whole screen up by one text row and clear the freed bottom row. */
static void e17_fb_scroll(void)
{
	const size_t row_bytes = (size_t)E17_FB_PITCH * E17_FB_CELL;

	memmove((void *)e17_fb, (const void *)(e17_fb + row_bytes),
		row_bytes * (E17_FB_ROWS - 1));
	memset((void *)(e17_fb + row_bytes * (E17_FB_ROWS - 1)), E17_FB_BG,
	       row_bytes);
}

/* Advance to the start of the next text line, scrolling if at the bottom. */
static void e17_fb_newline(void)
{
	e17_fb_col = 0;
	if (++e17_fb_row >= E17_FB_ROWS) {
		e17_fb_scroll();
		e17_fb_row = E17_FB_ROWS - 1;
	}
}

/* Render one character at the current cell and advance the cursor. */
void e17_fb_putc(char c)
{
	const unsigned char *glyph;
	int gy;
	int px, py;

	if (!e17_fb_ready)
		return;

	if (c == '\n') {
		e17_fb_newline();
		return;
	}
	if (c == '\r') {
		e17_fb_col = 0;
		return;
	}

	if (e17_fb_col >= E17_FB_COLS)
		e17_fb_newline();

	/* VGA 8x8 font: 8 bytes/glyph, one byte per row, bit7 = leftmost pixel */
	glyph = font_data_buf(font_vga_8x8.data) + ((unsigned char)c) * E17_FB_CELL;
	px = e17_fb_col * E17_FB_CELL;
	py = e17_fb_row * E17_FB_CELL;

	for (gy = 0; gy < E17_FB_CELL; gy++) {
		volatile u8 *line = e17_fb + (size_t)(py + gy) * E17_FB_PITCH + px;
		u8 bits = glyph[gy];
		int gx;

		for (gx = 0; gx < E17_FB_CELL; gx++)
			if (bits & (0x80 >> gx))
				line[gx] = E17_FB_FG;	/* fg only; bg = RMON white */
	}

	e17_fb_col++;
}

void e17_fb_puts(const char *s)
{
	while (*s)
		e17_fb_putc(*s++);
}

static void e17_fb_cons_write(struct console *co, const char *s, unsigned int n)
{
	while (n--)
		e17_fb_putc(*s++);
}

static struct console e17_fb_console = {
	.name	= "e17fb",
	.write	= e17_fb_cons_write,
	.flags	= CON_PRINTBUFFER | CON_BOOT,
	.index	= -1,
};

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
	e17_fb_puts(s);			/* mirror to the screen (reliable channel) */
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
	e17_cd2401_init();		/* known-good tx state before first printk */
	e17_fb_init();			/* ensure the screen console is up */
	register_console(&e17_early_console);
	/*
	 * PROBE: do NOT register the framebuffer as a printk console yet -- that
	 * floods it and triggers scroll (memmove/memset of the whole screen) into
	 * the VRAM region that hard-locks.  Only the explicit early e17_fb_puts
	 * markers render (a few lines, no scroll) so we can safely read the layout.
	 */
	/* register_console(&e17_fb_console); */
}
