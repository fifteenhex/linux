// SPDX-License-Identifier: GPL-2.0+
/*
 * Console/tty driver for the Cirrus Logic CD2401 on the ELTEC Eurocom
 * E17 (VMEbus MC68040/060 SBC).  Channel 0 is the RMON serial console.
 *
 * The E17 wires the CD2401 interrupt through local input 6 of the
 * VIC068A (register LICR6) and delivers it to the CPU as an m68k
 * vectored interrupt using the vector programmed into the chip's LIVR.
 * There is no VIC irqchip yet, so the DT routes the interrupt through
 * the generic user-vector controller (intc_user); an interrupt cell N
 * there maps to CPU vector VEC_USER+N / Linux IRQ IRQ_USER+N.  Because
 * the chip supplies the interrupt type in the low two bits of the
 * vector (11 = receive data), the DT cell must be chosen so that
 * VEC_USER+N has those bits set (N congruent to 3 mod 4).
 *
 * Transmit is done polled (instantaneous on the model, fast enough on
 * hardware for a console); receive is interrupt driven.  As on the
 * board's firmware, the receive service enters context by reading the
 * board's interrupt-acknowledge window (second reg range, 0xfec66000),
 * drains the FIFO and ends with a write to REOIR.
 *
 * Register knowledge is reverse-engineered from RMON and the u-boot
 * port; see E17-NOTES.md in the qemu-e17 tree.
 */

#include <linux/console.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kfifo.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/serial.h>
#include <linux/serial_core.h>
#include <linux/tty_flip.h>

#include <asm/irq.h>
#include <asm/traps.h>

/* CD2401 registers (channel bank selected by CAR, plus service regs) */
#define CD2401_LIVR		0x09	/* local interrupt vector */
#define CD2401_IER		0x11	/* interrupt enable */
#define CD2401_IER_TXD		0x01
#define CD2401_IER_RXD		0x08
#define CD2401_CCR		0x13	/* channel command */
#define CD2401_CCR_ENBRX	0x02
#define CD2401_CCR_ENBXMTR	0x08
#define CD2401_CMR		0x1b	/* channel mode */
#define CD2401_CMR_ASYNC	0x02
#define CD2401_RFOC		0x30	/* receive FIFO output count */
#define CD2401_TFTC		0x80	/* tx FIFO transfer count (free space) */
#define CD2401_REOIR		0x84	/* receive end of interrupt */
#define CD2401_TEOIR		0x85	/* transmit end of interrupt */
#define CD2401_MEOIR		0x86	/* modem end of interrupt */
#define CD2401_RISRH		0x88	/* receive interrupt status, high */
#define CD2401_RISRL		0x89	/* receive interrupt status, low */
#define CD2401_TISR		0x8a	/* transmit interrupt status */
#define CD2401_TISR_TXEMPTY	0x02	/* transmitter (FIFO+shifter) fully empty */
#define CD2401_TEOIR_NOTRANS	0x08	/* TEOIR: no data transferred this service */
#define CD2401_IER_TXMPTY	0x02	/* interrupt when transmitter fully empty */
#define CD2401_TPILR		0xe0	/* tx priority interrupt level */
#define CD2401_RPILR		0xe1	/* rx priority interrupt level */
#define CD2401_MPILR		0xe3	/* modem priority interrupt level */
#define CD2401_TIR		0xec	/* transmit interrupt register */
#define CD2401_TIR_TACT		0x40	/* a transmit service context is active */
#define CD2401_CAR		0xee	/* channel access (select) */
#define CD2401_DR		0xf8	/* rx/tx data */

/* interrupt-acknowledge window: address bits carry the priority level */
#define CD2401_RX_IPL		1
#define CD2401_TX_IPL		2	/* private polled-tx window (legacy path) */

/*
 * RX/exception are delivered self-vectored on the VIC LIRQ6 daisy chain: the
 * CPU's level-5 IACK enters the chip's receive service context directly.  The
 * chip answers that IACK when a PILR matches the IACK's A[6:0] = 0x7b; RMON uses
 * 0xfb (the compare is 7-bit, 0xfb & 0x7f == 0x7b), so use the same.
 *
 * Legacy (polled-TX) mode keeps TX on its own private window level (TX_IPL) so
 * the CPU never vectors on it.  Interrupt-driven mode (txirq) instead programs
 * ALL three PILRs equal to 0xfb: the CD2401's internal priority (rx > tx >
 * modem) then arbitrates, TX is delivered CPU-vectored at LIVR|2, and the
 * polled console shares the same 0x7b acknowledge window as RX.
 */
#define CD2401_RX_PILR		0xfb
#define CD2401_SVC_IPL		(CD2401_RX_PILR & 0x7f)	/* 0x7b: shared window */

/*
 * Interrupt-driven transmit.  The CD2401 polled TX path (e17_cd2401_tx_one) is
 * the historical console-wedge source (its LICR6 save/restore races the VIC
 * refcount, and it can only move data via a hand-rolled software IACK).  With
 * txirq the tty transmits from a real CPU-vectored TX interrupt instead; the
 * console keeps a polled putchar (for oops/panic/no-IRQ context) but shares the
 * 0x7b acknowledge window and dispatches whatever the chip's internal priority
 * hands back.  Default on (confirmed on hardware); boot
 * serial_e17_cd2401.txirq=0 to fall back to the polled-TX path.
 */
static bool txirq = true;
module_param(txirq, bool, 0444);
MODULE_PARM_DESC(txirq,
	"Interrupt-driven transmit (default 1; set 0 for polled-TX fallback)");

/* VIC068A local interrupt 6 = the CD2401 line */
#define E17_VIC_BASE		0xfec00000
#define E17_VIC_LICR6		0x3b
#define E17_VIC_LICR_MASK	0x80
#define E17_VIC_LICR_STATE	0x08	/* raw CD2401 pin level (active low) */
#define E17_VIC_CD2401_LEVEL	6	/* m68k IPL the VIC raises for it */

#define E17_CD2401_MAX_PORTS	1
#define E17_CD2401_NAME		"ttyS"
#define CD2401_NR_SCRUB		4	/* channels to silence at startup */

struct e17_cd2401_port {
	struct uart_port port;
	void __iomem *iack;	/* interrupt-acknowledge window */
	void __iomem *vic;	/* VIC068A register window */
	u8 livr;		/* interrupt vector base (type bits clear) */
	int rxexc_irq;		/* rx-exception vector (own IRQ, shares handler) */
	int tx_irq;		/* transmit vector (txirq only) */
	int modem_irq;		/* modem vector (txirq only; catch-all handler) */
	bool tx_idle;		/* transmitter drained (for a truthful tx_empty) */
	bool console_wedged;	/* chip didn't respond during this console record */
	u32 console_wedges;	/* count of records that hit the wedge (diagnostic) */
};

static struct e17_cd2401_port *e17_cd2401_ports[E17_CD2401_MAX_PORTS];

static inline u8 cd_read(struct uart_port *port, unsigned int reg)
{
	return readb(port->membase + reg);
}

static inline void cd_write(struct uart_port *port, unsigned int reg, u8 val)
{
	writeb(val, port->membase + reg);
}

static void e17_cd2401_rx_chars(struct uart_port *port);

/*
 * Wait (bounded) for the CD2401 to assert its VIC line: LICR6 STATE is the raw
 * pin, active low, readable regardless of the mask.  Returns true if asserted.
 *
 * A healthy chip asserts within microseconds of IER.TxD being set, so this bound
 * is ~10-20 ms of uncached LICR6 reads on the 25 MHz local bus -- ample slack for
 * a working chip, but critically NOT the old ~100-200 ms: the console spins here
 * with interrupts hard-off, and the CD2401 TX is known to intermittently stop
 * re-asserting, so a long per-character timeout multiplied across a printk record
 * was seconds of IRQs-off -> a watchdog reset.  The console path additionally
 * gives up on the whole record after the first timeout (see e17_cd2401_putchar).
 */
#define CD2401_STATE_POLL	20000

static bool e17_cd2401_wait_asserted(struct e17_cd2401_port *up)
{
	int timeout = CD2401_STATE_POLL;

	while ((readb(up->vic + E17_VIC_LICR6) & E17_VIC_LICR_STATE) && --timeout)
		cpu_relax();
	return timeout != 0;
}

/*
 * Legacy polled transmit of one byte (txirq=0).  The CD2401 only sends from an
 * interrupt-service context, so enable the tx interrupt, wait for the VIC to see
 * the line asserted, acknowledge through the chip's private polled window
 * (TX_IPL) to enter tx service, write the byte and end with TEOIR.  The CD2401
 * is masked at the VIC for the duration so the CPU never vectors on it.
 */
static bool e17_cd2401_tx_one_polled(struct e17_cd2401_port *up, u8 ch)
{
	struct uart_port *port = &up->port;
	bool sent = false;
	u8 ier, licr;

	licr = readb(up->vic + E17_VIC_LICR6);
	writeb(licr | E17_VIC_LICR_MASK, up->vic + E17_VIC_LICR6);

	cd_write(port, CD2401_CAR, 0);		/* channel 0 */
	cd_write(port, CD2401_TPILR, CD2401_TX_IPL);
	ier = cd_read(port, CD2401_IER);
	cd_write(port, CD2401_IER, ier | CD2401_IER_TXD);

	/*
	 * Only acknowledge if the chip actually asserted the tx request: an IACK
	 * with nothing posted at this level gets no DTACK and hangs the bus.  If
	 * the STATE poll timed out, drop the character rather than wedge.
	 */
	if (e17_cd2401_wait_asserted(up)) {
		readb(up->iack + CD2401_TX_IPL);	/* IACK -> enter tx service */
		cd_write(port, CD2401_DR, ch);
		cd_write(port, CD2401_TEOIR, 0);
		sent = true;
	}
	cd_write(port, CD2401_IER, ier);	/* restore (tx irq off) */

	writeb(licr, up->vic + E17_VIC_LICR6);	/* restore VIC routing (STATE is RO) */
	return sent;
}

/*
 * Interrupt-driven-mode polled console putchar (txirq=1).  Runs in no-IRQ
 * contexts (printk/oops/panic) while the tty transmits from its TX interrupt,
 * so it cannot use the ISR.  All three PILRs are equal (0xfb), meaning the tty
 * TX/RX vectors and this software acknowledge all match the same 0x7b window;
 * the chip's internal priority (rx > tx > modem) decides what each IACK yields.
 *
 * Protocol: mask the CD2401 at the VIC (no CPU vectoring during the handshake),
 * post our own TX request (IER.TxD) so an IACK is always backed by a pending
 * request -- the invariant that keeps the acknowledge from hanging the bus --
 * then IACK at 0x7b and dispatch on the returned type: service TX (send our
 * byte), or fully service an RX that internal priority handed us first (drain +
 * REOIR) and retry, or dismiss a modem event.  A STATE-poll timeout drops the
 * byte rather than IACK with nothing posted.
 */
static bool e17_cd2401_tx_one_irq(struct e17_cd2401_port *up, u8 ch)
{
	struct uart_port *port = &up->port;
	bool sent = false;
	u8 ier, licr;
	int retries;

	licr = readb(up->vic + E17_VIC_LICR6);
	writeb(licr | E17_VIC_LICR_MASK, up->vic + E17_VIC_LICR6);

	cd_write(port, CD2401_CAR, 0);
	/*
	 * Program the PILRs ourselves before acknowledging: the console runs from
	 * early boot (printk) BEFORE the tty's startup() sets them, so we cannot
	 * assume they already match the 0x7b window -- an IACK that matches no PILR
	 * gets no DTACK and hangs the real bus.  Set all three equal to 0xfb so our
	 * posted TX request is matchable and any (post-startup) pending RX is too.
	 */
	cd_write(port, CD2401_TPILR, CD2401_RX_PILR);
	cd_write(port, CD2401_RPILR, CD2401_RX_PILR);
	cd_write(port, CD2401_MPILR, CD2401_RX_PILR);
	ier = cd_read(port, CD2401_IER);
	cd_write(port, CD2401_IER, ier | CD2401_IER_TXD);

	for (retries = 0; retries < 16 && !sent; retries++) {
		u8 ack, type;

		if (!e17_cd2401_wait_asserted(up))
			break;				/* nothing posted: do not IACK */

		ack = readb(up->iack + CD2401_SVC_IPL);	/* enter service context */
		type = ack & 0x3;
		if (type == 0x2) {			/* transmit */
			cd_write(port, CD2401_DR, ch);
			cd_write(port, CD2401_TEOIR, 0);
			sent = true;
		} else if (type == 0x3 || type == 0x0) {	/* rx good / rx exc */
			if (port->state)
				e17_cd2401_rx_chars(port);	/* drains + REOIR */
			else
				cd_write(port, CD2401_REOIR, CD2401_TEOIR_NOTRANS);
			/* our TX request is still posted; loop and IACK again */
		} else {				/* modem */
			cd_write(port, CD2401_MEOIR, CD2401_TEOIR_NOTRANS);
		}
	}

	cd_write(port, CD2401_IER, ier);	/* restore tty's IER intent */
	writeb(licr, up->vic + E17_VIC_LICR6);	/* restore VIC routing */
	return sent;
}

/* Returns true if the byte was handed to the chip, false if it did not respond. */
static bool e17_cd2401_tx_one(struct e17_cd2401_port *up, u8 ch)
{
	if (txirq)
		return e17_cd2401_tx_one_irq(up, ch);
	return e17_cd2401_tx_one_polled(up, ch);
}

static void e17_cd2401_putchar(struct uart_port *port, unsigned char ch)
{
	struct e17_cd2401_port *up =
		container_of(port, struct e17_cd2401_port, port);

	/*
	 * Console records run with the port lock held and interrupts OFF for the
	 * whole string.  If the CD2401 has stopped responding, give up on the rest
	 * of THIS record after the first failure rather than paying the (bounded)
	 * STATE-poll timeout for every remaining character -- that per-char sum was
	 * the multi-second IRQs-off window that tripped the watchdog.  The flag is
	 * cleared at the start of each record (e17_cd2401_console_write).
	 */
	if (up->console_wedged)
		return;
	if (!e17_cd2401_tx_one(up, ch)) {
		up->console_wedged = true;
		up->console_wedges++;
	}
}

/*
 * Polled transmit of the pending FIFO.  The CD2401 moves tx data only inside an
 * interrupt-service context, which e17_cd2401_tx_one() enters with a software
 * IACK (the VIC line is masked there so the CPU never vectors on it).  True
 * CPU-vectored tx interrupts are unsafe on this board: the transmit and receive
 * services share one VIC line, and a CPU IACK whose level matches no posted
 * interrupt gets no DTACK and hangs the bus -- so tx stays polled while rx is
 * interrupt-driven.  See CD2401-TX-DESIGN.md.
 */
static void e17_cd2401_tx_chars(struct uart_port *port)
{
	struct e17_cd2401_port *up =
		container_of(port, struct e17_cd2401_port, port);
	u8 ch;

	uart_port_tx(port, ch, true, e17_cd2401_tx_one(up, ch));
}

static void e17_cd2401_rx_chars(struct uart_port *port)
{
	unsigned int cnt, i;

	/*
	 * Self-vectored on the LIRQ6 daisy chain at level 5: the CPU's level-5
	 * IACK already fetched the vector AND entered the chip's receive service
	 * context, so we must NOT poke the board IACK window here -- doing so
	 * would be a second acknowledge (and reading it with nothing posted at
	 * that level gets no DTACK and hangs the bus).  Both rx-data (0x53) and
	 * rx-exception (0x50) arrive here on their own IRQs; either way read (and
	 * clear) the receive status first -- an overrun latches a receive
	 * exception until RISR is read, else the chip re-interrupts forever.
	 */
	cd_read(port, CD2401_RISRH);
	cd_read(port, CD2401_RISRL);
	cnt = cd_read(port, CD2401_RFOC);
	for (i = 0; i < cnt; i++) {
		u8 ch = cd_read(port, CD2401_DR);

		port->icount.rx++;
		if (!uart_handle_sysrq_char(port, ch))
			tty_insert_flip_char(&port->state->port, ch, TTY_NORMAL);
	}

	cd_write(port, CD2401_REOIR, 0);	/* end of receive interrupt */

	tty_flip_buffer_push(&port->state->port);
}

static irqreturn_t e17_cd2401_interrupt(int irq, void *data)
{
	struct uart_port *port = data;
	unsigned long flags;

	uart_port_lock_irqsave(port, &flags);
	e17_cd2401_rx_chars(port);
	uart_port_unlock_irqrestore(port, flags);

	return IRQ_HANDLED;
}

/*
 * Transmit interrupt (txirq only), CPU-vectored at LIVR|2.  The CPU's level-5
 * IACK has already entered the chip's transmit service context, so TFTC/TDR are
 * live and we must end with exactly one TEOIR (the last chip access) -- no board
 * IACK window.  Fill up to TFTC bytes from the xmit fifo; drop IER.TxD *before*
 * the EOI once the ring drains so the level-triggered request cannot re-post and
 * storm; keep IER.TxMpty until the transmitter is fully empty so tx_empty() is
 * truthful.  TEOIR carries NOTRANS when no byte was moved.
 */
static irqreturn_t e17_cd2401_tx_interrupt(int irq, void *data)
{
	struct uart_port *port = data;
	struct e17_cd2401_port *up =
		container_of(port, struct e17_cd2401_port, port);
	struct tty_port *tport = &port->state->port;
	unsigned long flags;
	unsigned int tftc, sent = 0;
	u8 ier, ch;

	uart_port_lock_irqsave(port, &flags);

	ier = cd_read(port, CD2401_IER);

	if (port->x_char) {
		cd_write(port, CD2401_DR, port->x_char);
		port->icount.tx++;
		port->x_char = 0;
		sent++;
	} else if (!uart_tx_stopped(port)) {
		tftc = cd_read(port, CD2401_TFTC);
		while (sent < tftc && kfifo_get(&tport->xmit_fifo, &ch)) {
			cd_write(port, CD2401_DR, ch);
			port->icount.tx++;
			sent++;
		}
	}

	if (uart_tx_stopped(port) ||
	    (!port->x_char && kfifo_is_empty(&tport->xmit_fifo))) {
		/*
		 * Nothing left to send: stop requesting TxD.  If the shifter is
		 * also empty, drop TxMpty too and latch idle for tx_empty().
		 * Update IER before the EOI (EOIR must be the last chip access).
		 */
		ier &= ~CD2401_IER_TXD;
		if (cd_read(port, CD2401_TISR) & CD2401_TISR_TXEMPTY) {
			ier &= ~CD2401_IER_TXMPTY;
			up->tx_idle = true;
		}
		cd_write(port, CD2401_IER, ier);
	}

	cd_write(port, CD2401_TEOIR, sent ? 0 : CD2401_TEOIR_NOTRANS);

	if (kfifo_len(&tport->xmit_fifo) < WAKEUP_CHARS)
		uart_write_wakeup(port);

	uart_port_unlock_irqrestore(port, flags);
	return IRQ_HANDLED;
}

/*
 * Modem interrupt (txirq only), CPU-vectored at LIVR|1.  We enable no modem
 * sources, but once MPILR matches the level-5 IACK any stray modem request must
 * still be acknowledged: an un-EOI'd service context stops the chip
 * interrupting forever.  Dismiss it with a NOTRANS MEOIR.
 */
static irqreturn_t e17_cd2401_modem_interrupt(int irq, void *data)
{
	struct uart_port *port = data;
	unsigned long flags;

	uart_port_lock_irqsave(port, &flags);
	cd_write(port, CD2401_MEOIR, CD2401_TEOIR_NOTRANS);
	uart_port_unlock_irqrestore(port, flags);

	return IRQ_HANDLED;
}

static unsigned int e17_cd2401_tx_empty(struct uart_port *port)
{
	struct e17_cd2401_port *up =
		container_of(port, struct e17_cd2401_port, port);

	if (!txirq)
		return TIOCSER_TEMT;	/* polled TX is synchronous/immediate */

	/* Set by the TX ISR when it saw TISR TXEMPTY with an empty ring. */
	return up->tx_idle ? TIOCSER_TEMT : 0;
}

static void e17_cd2401_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
}

static unsigned int e17_cd2401_get_mctrl(struct uart_port *port)
{
	return TIOCM_CTS | TIOCM_DSR | TIOCM_CAR;
}

static void e17_cd2401_stop_tx(struct uart_port *port)
{
	u8 ier;

	if (!txirq)
		return;			/* polled TX has nothing running to stop */

	cd_write(port, CD2401_CAR, 0);
	ier = cd_read(port, CD2401_IER);
	/* Keep TxMpty so the tail drain is still observed for tx_empty(). */
	cd_write(port, CD2401_IER, ier & ~CD2401_IER_TXD);
}

static void e17_cd2401_start_tx(struct uart_port *port)
{
	struct e17_cd2401_port *up =
		container_of(port, struct e17_cd2401_port, port);
	u8 ier;

	if (!txirq) {
		e17_cd2401_tx_chars(port);	/* legacy polled path */
		return;
	}

	up->tx_idle = false;
	cd_write(port, CD2401_CAR, 0);
	ier = cd_read(port, CD2401_IER);
	/* TxD paces the fill; TxMpty is the belt-and-braces re-post/empty signal. */
	cd_write(port, CD2401_IER, ier | CD2401_IER_TXD | CD2401_IER_TXMPTY);
}

static void e17_cd2401_stop_rx(struct uart_port *port)
{
	struct e17_cd2401_port *up =
		container_of(port, struct e17_cd2401_port, port);

	cd_write(port, CD2401_CAR, 0);
	cd_write(port, CD2401_IER, cd_read(port, CD2401_IER) & ~CD2401_IER_RXD);
	/*
	 * Disable both rx sources; the VIC refcounts LICR6 and masks it once the
	 * last source is disabled.  IRQ_DISABLE_UNLAZY makes this synchronous.
	 * _nosync because we hold the uart port lock here.
	 */
	disable_irq_nosync(port->irq);
	if (up->rxexc_irq > 0)
		disable_irq_nosync(up->rxexc_irq);
}

static int e17_cd2401_startup(struct uart_port *port)
{
	struct e17_cd2401_port *up =
		container_of(port, struct e17_cd2401_port, port);
	unsigned long flags;
	int ret;

	/*
	 * rx-data and rx-exception are separate self-vectored sources on LIRQ6,
	 * each with its own IRQ but the same service handler.  Request both masked
	 * (NO_AUTOEN); the VIC refcounts the shared LICR6, so enable/disable of
	 * either is safe.
	 */
	ret = request_irq(port->irq, e17_cd2401_interrupt, IRQF_NO_AUTOEN,
			  "ttyS", port);
	if (ret)
		return ret;
	if (up->rxexc_irq > 0) {
		ret = request_irq(up->rxexc_irq, e17_cd2401_interrupt,
				  IRQF_NO_AUTOEN, "ttyS-rxexc", port);
		if (ret)
			goto err_rxexc;
	}
	if (txirq && up->tx_irq > 0) {
		ret = request_irq(up->tx_irq, e17_cd2401_tx_interrupt,
				  IRQF_NO_AUTOEN, "ttyS-tx", port);
		if (ret)
			goto err_tx;
	}
	if (txirq && up->modem_irq > 0) {
		ret = request_irq(up->modem_irq, e17_cd2401_modem_interrupt,
				  IRQF_NO_AUTOEN, "ttyS-modem", port);
		if (ret)
			goto err_modem;
	}

	uart_port_lock_irqsave(port, &flags);

	/*
	 * Scrub every channel's interrupt-enable before arming ours: once the PILRs
	 * match the level-5 IACK a stale enable on any channel could vector to an
	 * un-handled context and stop the chip.  (u-boot/RMON leave ch3 untouched.)
	 */
	if (txirq) {
		int ch;

		for (ch = CD2401_NR_SCRUB - 1; ch >= 0; ch--) {
			cd_write(port, CD2401_CAR, ch);
			cd_write(port, CD2401_IER, 0);
		}
	}

	cd_write(port, CD2401_CAR, 0);		/* console channel */
	cd_write(port, CD2401_CMR, CD2401_CMR_ASYNC);
	cd_write(port, CD2401_LIVR, up->livr);
	cd_write(port, CD2401_RPILR, CD2401_RX_PILR);	/* answers level-5 IACK */
	if (txirq) {
		/* equal PILRs -> internal priority rx > tx > modem, all vectored */
		cd_write(port, CD2401_TPILR, CD2401_RX_PILR);
		cd_write(port, CD2401_MPILR, CD2401_RX_PILR);
	} else {
		cd_write(port, CD2401_TPILR, CD2401_TX_IPL);	/* private polled-tx */
	}
	cd_write(port, CD2401_CCR, CD2401_CCR_ENBRX | CD2401_CCR_ENBXMTR);
	cd_write(port, CD2401_IER, CD2401_IER_RXD);
	up->tx_idle = true;

	uart_port_unlock_irqrestore(port, flags);

	/* Route LIRQ6 to the CPU (VIC unmasks LICR6, refcounted across sources). */
	enable_irq(port->irq);
	if (up->rxexc_irq > 0)
		enable_irq(up->rxexc_irq);
	if (txirq && up->tx_irq > 0)
		enable_irq(up->tx_irq);
	if (txirq && up->modem_irq > 0)
		enable_irq(up->modem_irq);

	return 0;

err_modem:
	if (txirq && up->tx_irq > 0)
		free_irq(up->tx_irq, port);
err_tx:
	if (up->rxexc_irq > 0)
		free_irq(up->rxexc_irq, port);
err_rxexc:
	free_irq(port->irq, port);
	return ret;
}

static void e17_cd2401_shutdown(struct uart_port *port)
{
	struct e17_cd2401_port *up =
		container_of(port, struct e17_cd2401_port, port);
	unsigned long flags;

	uart_port_lock_irqsave(port, &flags);
	e17_cd2401_stop_rx(port);
	if (txirq) {
		cd_write(port, CD2401_CAR, 0);
		cd_write(port, CD2401_IER,
			 cd_read(port, CD2401_IER) &
			 ~(CD2401_IER_TXD | CD2401_IER_TXMPTY));
	}
	uart_port_unlock_irqrestore(port, flags);

	if (txirq && up->modem_irq > 0)
		free_irq(up->modem_irq, port);
	if (txirq && up->tx_irq > 0)
		free_irq(up->tx_irq, port);
	if (up->rxexc_irq > 0)
		free_irq(up->rxexc_irq, port);
	free_irq(port->irq, port);
}

static void e17_cd2401_set_termios(struct uart_port *port, struct ktermios *new,
				   const struct ktermios *old)
{
	unsigned long flags;
	unsigned int baud;

	uart_port_lock_irqsave(port, &flags);
	baud = uart_get_baud_rate(port, new, old, 50, 115200);
	uart_update_timeout(port, new->c_cflag, baud);
	uart_port_unlock_irqrestore(port, flags);
}

static const char *e17_cd2401_type(struct uart_port *port)
{
	return "E17-CD2401";
}

static void e17_cd2401_config_port(struct uart_port *port, int flags)
{
	if (flags & UART_CONFIG_TYPE)
		port->type = PORT_UNKNOWN + 1;
}

static int e17_cd2401_verify_port(struct uart_port *port,
				  struct serial_struct *ser)
{
	return 0;
}

static void e17_cd2401_release_port(struct uart_port *port)
{
}

static int e17_cd2401_request_port(struct uart_port *port)
{
	return 0;
}

static const struct uart_ops e17_cd2401_ops = {
	.tx_empty	= e17_cd2401_tx_empty,
	.set_mctrl	= e17_cd2401_set_mctrl,
	.get_mctrl	= e17_cd2401_get_mctrl,
	.stop_tx	= e17_cd2401_stop_tx,
	.start_tx	= e17_cd2401_start_tx,
	.stop_rx	= e17_cd2401_stop_rx,
	.startup	= e17_cd2401_startup,
	.shutdown	= e17_cd2401_shutdown,
	.set_termios	= e17_cd2401_set_termios,
	.type		= e17_cd2401_type,
	.config_port	= e17_cd2401_config_port,
	.verify_port	= e17_cd2401_verify_port,
	.release_port	= e17_cd2401_release_port,
	.request_port	= e17_cd2401_request_port,
};

#ifdef CONFIG_SERIAL_E17_CD2401_CONSOLE
static void e17_cd2401_console_write(struct console *co, const char *s,
				     unsigned int count)
{
	struct e17_cd2401_port *up = e17_cd2401_ports[co->index];
	struct uart_port *port;
	unsigned long flags;

	if (!up)
		return;
	port = &up->port;

	up->console_wedged = false;	/* fresh chance for this record */
	uart_port_lock_irqsave(port, &flags);
	uart_console_write(port, s, count, e17_cd2401_putchar);
	uart_port_unlock_irqrestore(port, flags);
}

static int e17_cd2401_console_setup(struct console *co, char *options)
{
	struct e17_cd2401_port *up;
	int baud = 9600, bits = 8, parity = 'n', flow = 'n';

	if (co->index < 0 || co->index >= E17_CD2401_MAX_PORTS)
		return -EINVAL;
	up = e17_cd2401_ports[co->index];
	if (!up || !up->port.membase)
		return -ENODEV;

	if (options)
		uart_parse_options(options, &baud, &parity, &bits, &flow);

	return uart_set_options(&up->port, co, baud, parity, bits, flow);
}

static struct uart_driver e17_cd2401_uart_driver;

static struct console e17_cd2401_console = {
	.name	= E17_CD2401_NAME,
	.write	= e17_cd2401_console_write,
	.device	= uart_console_device,
	.setup	= e17_cd2401_console_setup,
	.flags	= CON_PRINTBUFFER,
	.index	= -1,
	.data	= &e17_cd2401_uart_driver,
};

static int __init e17_cd2401_console_init(void)
{
	register_console(&e17_cd2401_console);
	return 0;
}
console_initcall(e17_cd2401_console_init);

#define E17_CD2401_CONSOLE	(&e17_cd2401_console)
#else
#define E17_CD2401_CONSOLE	NULL
#endif /* CONFIG_SERIAL_E17_CD2401_CONSOLE */

static struct uart_driver e17_cd2401_uart_driver = {
	.owner		= THIS_MODULE,
	.driver_name	= "e17_cd2401",
	.dev_name	= E17_CD2401_NAME,
	.major		= 0,
	.minor		= 0,
	.nr		= E17_CD2401_MAX_PORTS,
	.cons		= E17_CD2401_CONSOLE,
};

static int e17_cd2401_probe(struct platform_device *pdev)
{
	struct e17_cd2401_port *up;
	struct uart_port *port;
	struct resource *res;
	u32 livr_base;
	int irq, ret;

	up = devm_kzalloc(&pdev->dev, sizeof(*up), GFP_KERNEL);
	if (!up)
		return -ENOMEM;
	port = &up->port;

	port->membase = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(port->membase))
		return PTR_ERR(port->membase);
	port->mapbase = res->start;

	up->iack = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(up->iack))
		return PTR_ERR(up->iack);

	up->vic = devm_ioremap(&pdev->dev, E17_VIC_BASE, 0x100);
	if (!up->vic)
		return -ENOMEM;

	/*
	 * Interrupt index 0 = rx-data, 1 = rx-exception, 2 = tx, 3 = modem
	 * (interrupt-names).  TX/modem are only used in txirq mode.
	 */
	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;
	port->irq = irq;
	up->rxexc_irq = platform_get_irq_optional(pdev, 1);
	up->tx_irq = platform_get_irq_optional(pdev, 2);
	up->modem_irq = platform_get_irq_optional(pdev, 3);

	/*
	 * The chip self-vectors: LIVR holds the base and the CD2401 fills the
	 * low two bits with the interrupt type (1=modem, 2=tx, 3=rx).  The IRQ
	 * is now a VIC-domain virq (unrelated to the vector), so take the base
	 * from the DT (RMON uses 0x50, clear of the VIC's LIVBR groups).
	 */
	if (of_property_read_u32(pdev->dev.of_node, "livr-base", &livr_base))
		livr_base = 0x50;
	up->livr = livr_base & ~0x3;

	port->dev = &pdev->dev;
	port->type = PORT_UNKNOWN;
	port->iotype = UPIO_MEM;
	port->flags = UPF_BOOT_AUTOCONF;
	port->ops = &e17_cd2401_ops;
	port->fifosize = 16;
	port->line = 0;
	port->uartclk = 20000000;

	e17_cd2401_ports[0] = up;
	platform_set_drvdata(pdev, up);

	ret = uart_add_one_port(&e17_cd2401_uart_driver, port);
	if (ret)
		e17_cd2401_ports[0] = NULL;

	return ret;
}

static void e17_cd2401_remove(struct platform_device *pdev)
{
	struct e17_cd2401_port *up = platform_get_drvdata(pdev);

	uart_remove_one_port(&e17_cd2401_uart_driver, &up->port);
	e17_cd2401_ports[0] = NULL;
}

static const struct of_device_id e17_cd2401_of_match[] = {
	{ .compatible = "eltec,e17-cd2401" },
	{ .compatible = "cirrus,cd2401" },
	{ }
};
MODULE_DEVICE_TABLE(of, e17_cd2401_of_match);

static struct platform_driver e17_cd2401_platform_driver = {
	.probe	= e17_cd2401_probe,
	.remove	= e17_cd2401_remove,
	.driver	= {
		.name		= "e17_cd2401",
		.of_match_table	= e17_cd2401_of_match,
	},
};

static int __init e17_cd2401_init(void)
{
	int ret;

	ret = uart_register_driver(&e17_cd2401_uart_driver);
	if (ret)
		return ret;

	ret = platform_driver_register(&e17_cd2401_platform_driver);
	if (ret)
		uart_unregister_driver(&e17_cd2401_uart_driver);

	return ret;
}

static void __exit e17_cd2401_exit(void)
{
	platform_driver_unregister(&e17_cd2401_platform_driver);
	uart_unregister_driver(&e17_cd2401_uart_driver);
}

module_init(e17_cd2401_init);
module_exit(e17_cd2401_exit);

MODULE_DESCRIPTION("ELTEC E17 CD2401 serial console driver");
MODULE_LICENSE("GPL");
