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
#include <linux/font.h>		/* font_vga_8x8 for the framebuffer boot console */
#ifdef CONFIG_SMP
#include <asm/smp_e17.h>
#endif

#include "eltec.h"

/*
 * Boot diagnostics: drop a distinct code on the board's 7-seg POST display
 * (Z8536 CIO1 port C at 0xfec30000) at each milestone so the furthest point
 * reached is visible without a serial console.  A plain store -- it can never
 * hang the CPU.  Codes 0xf1..0xf7 are used in setup_arch(); this file continues
 * the sequence into IRQ/timer bring-up.
 *
 * NB: the latch's UPPER nibble is a per-bit ACTIVE-LOW write-enable for the
 * LOWER nibble (a lower bit changes only where its upper enable bit is 0, per
 * the manual).  So we must mask the upper nibble to 0 to actually update the
 * display; callers keep passing 0xfN for readability.
 */
#define E17_POST(code)	(*(volatile u8 *)0xfec30000 = (code) & 0x0f)

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

#define E17_CD2401_ACK_LOOPS	0x10000		/* per STATE-wait; bounded */
#define E17_CD2401_TACT_RETRY	64		/* IACK->TACT retries */

/*
 * Wait (bounded) for the CD2401 to request tx service -- the VIC's LICR6 STATE
 * bit is the raw interrupt line, active LOW -- then interrupt-acknowledge at the
 * tx priority level, which enters tx service context (datasheet 5.3.2).
 * Returns 0 once the IACK was issued.
 */
static int e17_cd2401_ack_irq(void)
{
	unsigned int i;

	for (i = E17_CD2401_ACK_LOOPS; i; i--) {
		if (e17_vic[E17_VIC_LICR6] & E17_VIC_LICR_STATE) {
			cpu_relax();			/* line not asserted yet */
			continue;
		}
		(void)e17_cd2401_iack[CD2401_TX_IPL];	/* IACK -> enter service */
		return 0;
	}
	return -1;
}

/*
 * Enter the tx service context AND confirm it is really the transmitter's
 * (TIR & TACT).  This mirrors u-boot's serial_cd2401_ack_tx_irq(), which prints
 * reliably on this board: if the IACK landed before TACT asserted, retry the
 * whole STATE-wait+IACK.  We only ever enable channel-0 TxD, so TACT is the sole
 * qualifier we need.  Returns 0 on success.
 */
static int e17_cd2401_ack_tx(void)
{
	unsigned int i;

	for (i = E17_CD2401_TACT_RETRY; i; i--) {
		if (e17_cd2401_ack_irq() != 0)
			return -1;
		if (e17_cd2401[CD2401_TIR] & CD2401_TIR_TACT)
			return 0;
	}
	return -1;
}

/*
 * Transmit one byte through the full CD2401 tx service handshake, exactly the
 * way u-boot's serial_cd2401_write_tx() does it ONE CHARACTER AT A TIME (that is
 * the path proven to print pages reliably on real hardware):
 *
 *   enable TxD -> ack_tx (STATE+IACK+TACT) -> write the byte -> TEOIR=0
 *   -> ack_tx AGAIN -> TEOIR=Notrans (the "double close") -> disable TxD.
 *
 * The double close is why this must be per-byte, not per-fifo-load: the second
 * ack waits for the chip to re-request service, which only happens once the
 * FIFO has drained below the COR4 watermark.  With a single byte in flight that
 * happens almost immediately; with a whole FIFO-load queued it would not
 * re-request for many bit-times and the second ack would stall (that is the bug
 * the old per-fifo-load writer hit and "fixed" by dropping the close+TACT --
 * which then let the chip re-transmit the last FIFO load, i.e. the garbled
 * "timer_pro timer_pro ..." repeats seen on the board).
 */
static void e17_cd2401_putc_raw(char c)
{
	e17_cd2401[CD2401_CAR] = 0;			/* select channel 0 */
	e17_cd2401[CD2401_TPILR] = CD2401_TX_IPL;
	e17_cd2401[CD2401_IER] |= CD2401_IER_TXD;	/* arm tx service request */

	if (e17_cd2401_ack_tx() == 0) {
		e17_cd2401[CD2401_DR] = c;		/* one byte into the FIFO */
		e17_cd2401[CD2401_TEOIR] = 0;		/* end-of-int: data xferred */
		e17_cd2401_ack_tx();			/* re-enter service (close) */
		e17_cd2401[CD2401_TEOIR] = CD2401_TEOIR_NOTRANS;
	}

	e17_cd2401[CD2401_IER] &= ~CD2401_IER_TXD;	/* disarm */
}

static void e17_cd2401_write(const char *s, unsigned int n)
{
	while (n--) {
		if (*s == '\n')
			e17_cd2401_putc_raw('\r');	/* CR/LF expansion */
		e17_cd2401_putc_raw(*s++);
	}
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

/* ------------------------------------------------------------------------- */
/* Framebuffer (VGA) boot console                                            */
/*                                                                           */
/* The CD2401 polled TX path still garbles/wedges on real hardware, so the   */
/* reliable early console is the VGA framebuffer:                            */
/*   - VRAM at 0x0fc00000, 8bpp, pitch = width = 800 bytes/line.  Pixel      */
/*     (x,y) is E17_FB_BASE + y*800 + x.  head.S installs a transparent,     */
/*     cache-inhibited TTR over 0x0f000000..0x10000000, so no ioremap.       */
/*   - RAMDAC at 0xfec40000 (mapped cache-inhibited via dtt1).  VGA-style:   */
/*     byte +0 = palette write-address, +1 = palette data (R,G,B autoinc).   */
/* Text is pixel 0xff (palette entry 0xff = white) on 0x00 (entry 0 = black),*/
/* rendered from the kernel's built-in VGA 8x8 font.  Runs before e17drm     */
/* probes; it is a CON_BOOT console so the kernel drops it when the real     */
/* console (fbcon/ttyS0) takes over.                                         */
/* ------------------------------------------------------------------------- */
#define E17_FB_BASE		0x0fc00000UL
#define E17_FB_WIDTH		800
#define E17_FB_HEIGHT		600
#define E17_FB_PITCH		E17_FB_WIDTH		/* 8bpp: 1 byte/pixel */

#define E17_RAMDAC_BASE		0xfec40000UL
#define E17_RAMDAC_PALADDR	0	/* palette write-address port */
#define E17_RAMDAC_PALDATA	1	/* palette data port (R,G,B autoinc) */

#define E17_FB_CELL		8			/* 8x8 font cell */
#define E17_FB_COLS		(E17_FB_WIDTH / E17_FB_CELL)	/* 100 */
#define E17_FB_ROWS		(E17_FB_HEIGHT / E17_FB_CELL)	/* 75 */
#define E17_FB_FG		0xff	/* white (palette set in init) */
#define E17_FB_BG		0x00	/* black */

static volatile u8 *const e17_fb = (volatile u8 *)E17_FB_BASE;
static volatile u8 *const e17_ramdac = (volatile u8 *)E17_RAMDAC_BASE;

static int e17_fb_col;
static int e17_fb_row;
static bool e17_fb_ready;

/*
 * Serialize all framebuffer access.  Once the AP is up, both CPUs can drive the
 * console; without this their VRAM writes interleave (corrupt glyphs/cursor) and
 * two masters hammering VRAM against the scanout can lock the local bus.
 */
static DEFINE_RAW_SPINLOCK(e17_fb_lock);

/* Load one palette entry (VGA-style: address then three data bytes). */
static void e17_fb_setpal(u8 idx, u8 r, u8 g, u8 b)
{
	e17_ramdac[E17_RAMDAC_PALADDR] = idx;
	e17_ramdac[E17_RAMDAC_PALDATA] = r;
	e17_ramdac[E17_RAMDAC_PALDATA] = g;
	e17_ramdac[E17_RAMDAC_PALDATA] = b;
}

/*
 * Prepare the framebuffer console: entry 0 = black, entry 0xff = white, home
 * the cursor.  No full-screen clear -- a sustained ~480KB write loop contends
 * with the video scanout and can hard-lock the bus; each glyph/row clears only
 * its own cells in short bursts.  Idempotent.
 */
void e17_fb_init(void)
{
	if (e17_fb_ready)
		return;

	e17_fb_setpal(E17_FB_BG, 0x00, 0x00, 0x00);	/* index 0 = black */
	e17_fb_setpal(E17_FB_FG, 0xff, 0xff, 0xff);	/* index 0xff = white */

	e17_fb_col = 0;
	e17_fb_row = 0;
	e17_fb_ready = true;
}

/* Clear one text row (8 scanlines) to background -- a short ~6.4KB burst. */
static void e17_fb_clear_row(int row)
{
	int y;

	for (y = 0; y < E17_FB_CELL; y++) {
		volatile u8 *line = e17_fb +
			(size_t)(row * E17_FB_CELL + y) * E17_FB_PITCH;
		int x;

		for (x = 0; x < E17_FB_WIDTH; x++)
			line[x] = E17_FB_BG;
	}
}

/*
 * Advance to the next text line.  A real scroll (a ~473KB VRAM memmove) sustains
 * a long write loop that contends with scanout and locks the bus, so instead we
 * wrap to the top and clear just the line we are about to write.  The console
 * fills top-to-bottom then overwrites from the top -- always the most recent
 * screenful, each line fresh.
 */
static void e17_fb_newline(void)
{
	e17_fb_col = 0;
	if (++e17_fb_row >= E17_FB_ROWS)
		e17_fb_row = 0;
	if (e17_fb_ready)
		e17_fb_clear_row(e17_fb_row);
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
			line[gx] = (bits & (0x80 >> gx)) ? E17_FB_FG : E17_FB_BG;
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
	unsigned long flags;

#ifdef CONFIG_SMP
	/*
	 * The VGA console only bus-locks once the secondary CPU is also a bus
	 * master (its cycles + our sustained VRAM bursts + scanout).  While we are
	 * still single-CPU (e.g. maxcpus=1, or before the AP is brought up) it is
	 * safe and gives a real boot log on VGA.  Silence it once cpu1 is online.
	 */
	if (num_online_cpus() > 1)
		return;
#endif
	raw_spin_lock_irqsave(&e17_fb_lock, flags);
	while (n--)
		e17_fb_putc(*s++);
	raw_spin_unlock_irqrestore(&e17_fb_lock, flags);
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
/*
 * Early boot trace: render straight to the VGA framebuffer.  Reliable once
 * e17_fb_init() has run (from config_eltec_e17); before that e17_fb_putc() is a
 * no-op guarded by e17_fb_ready, and anything printk'd earlier is replayed by
 * the CON_PRINTBUFFER framebuffer console when it registers, so nothing is lost.
 * (The CD2401 serial path still garbles/wedges on real hardware, so it is left
 * out here -- re-enable e17_cd2401_write() below once the serial TX is fixed.)
 */
void e17_early_puts(const char *s)
{
	unsigned long flags;

#ifdef CONFIG_SMP
	/*
	 * Once the AP is online, don't render post-SMP markers to VRAM: a burst can
	 * bus-lock if the AP is mid-activity, and the fb is not the SMP console.
	 * Show liveness on the 7-seg instead -- 'a' at the first post-smp_init
	 * marker (kernel_init_freeable done), 'b' at run_init, etc.  If the digit
	 * stops advancing, the boot wedged at that milestone; if it reaches the
	 * last marker the kernel booted and only ttyS0 output is in question.
	 */
	if (num_online_cpus() > 1) {
		static u8 roll = 0x0a;

		*(volatile u8 *)0xfec30000 = roll & 0x0f;
		roll++;
		return;
	}
#endif
	raw_spin_lock_irqsave(&e17_fb_lock, flags);
	e17_fb_puts(s);
	raw_spin_unlock_irqrestore(&e17_fb_lock, flags);
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
#define E17_VIC_LICR1		0x27	/* local IRQ 1 = keyboard (NOT the timer) */
#define E17_VIC_LICR2		0x2b	/* local IRQ 2 = VIC clock tick timer */
#define E17_VIC_SSCR0		0xc3	/* slave-select ctrl reg 0: tick enable/freq */
#define E17_VIC_LIVBR		0x57	/* local interrupt vector base register */
#define E17_VIC_LICR_LEVEL(l)	((l) & 0x07)	/* bit7=0 -> unmasked */
#define E17_VIC_LICR2_SENSE	0x30	/* LIRQ2 input sense/mode bits (per RMon) */

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
#define Z8536_CS_RCC		0x08	/* read-counter-control (latch count) */
#define Z8536_CS_GCB		0x04	/* gate command bit */
#define Z8536_CS_TCB		0x02	/* trigger command bit */
#define Z8536_CS_IP		0x20	/* interrupt pending (status) */
#define Z8536_CS_CIP		0x01	/* count in progress (status) */
#define Z8536_CT3CC_MSB		0x14	/* CT3 current count MSB */
#define Z8536_CT3CC_LSB		0x15	/* CT3 current count LSB */

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
	/* acknowledge: clear CT3's interrupt-pending (it auto-reloads) */
	e17_cio_wr(Z8536_CT3CS, Z8536_CMD_CLR_IPUS);
	/*
	 * NB: do NOT read the 0xfec50000 watchdog/ack block here.  The hardware
	 * IACK re-arms the tick; that read is not needed, and per the HW manual
	 * the first access to the watchdog block ARMS the watchdog -- which then
	 * resets the board the moment the tick stalls (e.g. on a crash).  Leave
	 * the watchdog disabled for now.
	 */
	legacy_timer_tick(1);
#ifdef CONFIG_SMP
	/*
	 * Poll-only baseline (CONFIG_E17_SMP_HW_IPI=n): service any inter-processor
	 * doorbell that arrived while this CPU was busy.  With HW_IPI the mailbox and
	 * ICMS doorbell ISRs handle delivery, so this tick backstop is not needed.
	 */
	if (!IS_ENABLED(CONFIG_E17_SMP_HW_IPI))
		e17_ipi_poll();
#endif
	return IRQ_HANDLED;
}

#ifdef CONFIG_SMP
/*
 * Secondary CPU per-CPU tick, driven by the board's VIC-clock which is delivered
 * to the secondary as a LEVEL-4 autovector interrupt (manual: the secondary can
 * be interrupted by the primary, the VIC timer, or the LEB).  This gives the AP
 * a real periodic tick so it reports RCU quiescent states and runs the scheduler
 * tick -- without it the AP wedges partway through the cpuhp online states.
 *
 * legacy_timer_tick(0): do the PER-CPU work (update_process_times, RCU, profile)
 * but NOT do_timer() -- the boot CPU owns global jiffies.
 */
static irqreturn_t e17_ap_tick(int irq, void *dev_id)
{
	static unsigned long last_jiffies, fires_this_jiffy;

	if (smp_processor_id() == 0)		/* only the secondary owns this tick */
		return IRQ_NONE;

	/*
	 * Ack the VIC-clock FIRST so the source de-asserts regardless of what we do
	 * with the tick below.
	 */
	writeb(E17_SICF_IACK_VIC, (void __iomem *)E17_CPU2CON);	/* ack VIC-clock */

	/*
	 * Storm rate-limiter.  A healthy VIC-clock fires at HZ, i.e. about once per
	 * jiffy (global jiffies is advanced by CPU0's tick, independent of the AP).
	 * If this handler somehow fires many times within a SINGLE jiffy, just SKIP
	 * the per-CPU timer work for the excess -- do NOT disable the clock.  The
	 * old code permanently disabled the VIC-clock here, which turned any
	 * transient burst (or, before cache coherency was fixed, a stale read of
	 * jiffies that made it look frozen) into a dead secondary tick forever.
	 * We still ack every interrupt (above), so the source cannot storm the CPU.
	 */
	if (jiffies != last_jiffies) {
		last_jiffies = jiffies;
		fires_this_jiffy = 0;
	} else if (++fires_this_jiffy > 8) {
		if (fires_this_jiffy == 9)	/* log once per burst, not per fire */
			pr_warn_ratelimited("E17 SMP: AP VIC-clock fired >8x in one jiffy (%lu); rate-limiting this jiffy\n",
					    jiffies);
		return IRQ_HANDLED;		/* skip timer work, keep the clock alive */
	}

	legacy_timer_tick(0);
	return IRQ_HANDLED;
}

/* Called on the SECONDARY CPU (secondary_start_kernel) to start its own tick. */
void e17_ap_tick_enable(void)
{
	writeb(E17_SICF_VICCLK_EN, (void __iomem *)E17_CPU2CON);
}
#endif

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
	 * The VIC routes the tick on LIRQ2 with vector base 0x40, so it arrives
	 * on vector 0x42 -> IRQ_USER+2.  Claim exactly that IRQ (claiming the
	 * whole IRQ_USER..+7 range collides with the CD2401 serial on IRQ 15).
	 */
	if (request_irq(E17_IRQ_TIMER, e17_timer_int, IRQF_TIMER, "vicclock-cpu0", NULL))
		pr_err("E17: unable to register timer interrupt\n");

	/*
	 * Route the tick through the VIC on LIRQ2.  Per the EUROCOM-17 HW
	 * manual (Table 48) the clock tick timer is LIRQ2 at level 6 -- LIRQ1
	 * is the *keyboard*, which is why programming LICR1 did nothing.  The
	 * CT3 signal reaches the CPU as a VIC-vectored interrupt:
	 *   LIVBR (vector base) = 0x40, so LIRQ2 -> vector 0x40|2 = 0x42 (66),
	 *   which m68k_setup_user_interrupt(VEC_USER=0x40) maps to IRQ_USER+2.
	 * Slave-select control reg 0 bits 7,6 enable/clock the tick path;
	 * LICR2 unmasks it at level 6 with the input-sense bits RMon uses.
	 * (This mirrors RMON's proven arm sequence.)
	 */
	e17_vic[E17_VIC_LIVBR] = 0x40;			/* vector base -> 66 on LIRQ2 */
	e17_vic[E17_VIC_SSCR0] |= 0xc0;			/* enable tick path */
	e17_vic[E17_VIC_LICR2] =
		E17_VIC_LICR2_SENSE | E17_VIC_LICR_LEVEL(6);	/* unmask, level 6 */

	/*
	 * Enable the CT3 interrupt, then start it counting.  The real Z8536
	 * only counts when the counter is both *gated* and *triggered*, so we
	 * must set the gate command bit (GCB) as well as the trigger (TCB) --
	 * TCB alone triggers but leaves the counter gated off, so it never
	 * reaches terminal count and never interrupts.  (QEMU's model starts on
	 * TCB alone, which masked this.)  This matches RMON's own gate+trigger.
	 */
	e17_cio_wr(Z8536_CT3CS, Z8536_CMD_SET_IE);
	e17_cio_wr(Z8536_CT3CS, Z8536_CS_GCB | Z8536_CS_TCB);

#ifdef CONFIG_SMP
	/*
	 * Register the secondary CPU's tick handler on the level-4 autovector IRQ
	 * (the VIC-clock delivered to the secondary).  The handler runs on the AP
	 * when it fires; the AP turns the delivery on via e17_ap_tick_enable() once
	 * it is online.  IRQF_TIMER | IRQF_SHARED (level 4 could be shared later).
	 */
	if (request_irq(IRQ_AUTO_4, e17_ap_tick, IRQF_TIMER | IRQF_SHARED,
			"vicclock-cpu1", &e17_ap_tick))
		pr_err("E17: unable to register secondary VIC-clock tick IRQ\n");
#endif
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
	 * Do NOT register the VGA framebuffer as the boot console under SMP: its
	 * sustained VRAM bursts (the per-newline row clear) hard-lock the local bus
	 * once the second CPU is also a bus master (its cas cycles + poll + the
	 * video scanout starve the write).  SMP bring-up itself works; the normal
	 * ttyS0 serial console (and later DRM/fbcon) provide the log.  The fb code
	 * is kept for UP use / future work.  [Now that the m68k atomic/bitops are
	 * casl-based under SMP, concurrent printk to ttyS0 is safe.]
	 */
	e17_fb_init();				/* palette + cursor home */
	register_console(&e17_fb_console);	/* runtime-gated to single-CPU */
	(void)e17_early_console;		/* kept for later; silence unused */
	(void)e17_cd2401_init;			/* kept for later; silence unused */

#ifdef CONFIG_SMP
	/* Enumerate the second CPU socket (also done from the DT cpu@1 node). */
	e17_smp_init_cpus();
#endif
}
