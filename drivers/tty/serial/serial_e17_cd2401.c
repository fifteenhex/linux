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
#define CD2401_REOIR		0x84	/* receive end of interrupt */
#define CD2401_TEOIR		0x85	/* transmit end of interrupt */
#define CD2401_RISRH		0x88	/* receive interrupt status, high */
#define CD2401_RISRL		0x89	/* receive interrupt status, low */
#define CD2401_TISR		0x8a	/* transmit interrupt status */
#define CD2401_TISR_TXEMPTY	0x02
#define CD2401_TFTC		0x80	/* tx FIFO transfer count (free space) */
#define CD2401_TEOIR_NOTRANS	0x08	/* TEOIR: no data transferred this service */
#define CD2401_TPILR		0xe0	/* tx priority interrupt level */
#define CD2401_RPILR		0xe1	/* rx priority interrupt level */
#define CD2401_CAR		0xee	/* channel access (select) */
#define CD2401_DR		0xf8	/* rx/tx data */

/* interrupt-acknowledge window: address bits carry the priority level */
#define CD2401_RX_IPL		1
#define CD2401_TX_IPL		2

/*
 * RX/exception are delivered self-vectored on the VIC LIRQ6 daisy chain: the
 * CPU's level-5 IACK enters the chip's receive service context directly.  The
 * chip answers that IACK when a PILR matches the IACK's A[6:0] = 0x7b; RMON uses
 * 0xfb (the compare is 7-bit, 0xfb & 0x7f == 0x7b), so use the same.  TX keeps
 * its own private polled window IACK at CD2401_TX_IPL, so it is left distinct.
 */
#define CD2401_RX_PILR		0xfb

/* VIC068A local interrupt 6 = the CD2401 line */
#define E17_VIC_BASE		0xfec00000
#define E17_VIC_LICR6		0x3b
#define E17_VIC_LICR_MASK	0x80
#define E17_VIC_LICR_STATE	0x08	/* raw CD2401 pin level (active low) */
#define E17_VIC_CD2401_LEVEL	6	/* m68k IPL the VIC raises for it */

#define E17_CD2401_MAX_PORTS	1
#define E17_CD2401_NAME		"ttyS"

struct e17_cd2401_port {
	struct uart_port port;
	void __iomem *iack;	/* interrupt-acknowledge window */
	void __iomem *vic;	/* VIC068A register window */
	u8 livr;		/* interrupt vector base (type bits clear) */
	int rxexc_irq;		/* rx-exception vector (own IRQ, shares handler) */
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

/*
 * Transmit one byte.  The CD2401 only sends from its interrupt-service
 * context, so enable the tx interrupt, wait for the VIC to see the line
 * asserted (LICR6 STATE, active low), acknowledge through the IACK
 * window to enter tx service, write the byte and end with TEOIR.  The
 * receive interrupt-enable bit is preserved so input keeps working.
 */
static void e17_cd2401_tx_one(struct e17_cd2401_port *up, u8 ch)
{
	struct uart_port *port = &up->port;
	int timeout = 200000;
	u8 ier, licr;

	/*
	 * Polled TX shares the CD2401's single interrupt line -- and thus VIC
	 * LICR6 -- with the interrupt-driven RX path.  Enabling the tx interrupt to
	 * enter tx-service context makes the chip assert that line; if the VIC
	 * forwards it to the CPU we take a vectored interrupt whose group the
	 * software IACK below has already consumed, so the CPU's IACK returns the
	 * base (group 0) vector -> "unexpected interrupt from <livr>".  Mask the
	 * CD2401 at the VIC for the duration of the handshake so ONLY our software
	 * IACK services it.  The STATE pin level we poll is a raw input, readable
	 * regardless of the mask, and the IACK window is a direct chip cycle, so tx
	 * still works.  Restoring LICR6 re-enables RX delivery (deferred at most for
	 * this one-character window).
	 */
	licr = readb(up->vic + E17_VIC_LICR6);
	writeb(licr | E17_VIC_LICR_MASK, up->vic + E17_VIC_LICR6);

	cd_write(port, CD2401_CAR, 0);		/* channel 0 */
	cd_write(port, CD2401_TPILR, CD2401_TX_IPL);
	ier = cd_read(port, CD2401_IER);
	cd_write(port, CD2401_IER, ier | CD2401_IER_TXD);

	while ((readb(up->vic + E17_VIC_LICR6) & E17_VIC_LICR_STATE) && --timeout)
		cpu_relax();

	readb(up->iack + CD2401_TX_IPL);	/* IACK -> enter tx service */
	cd_write(port, CD2401_DR, ch);
	cd_write(port, CD2401_TEOIR, 0);
	cd_write(port, CD2401_IER, ier);	/* restore (tx irq off) */

	writeb(licr, up->vic + E17_VIC_LICR6);	/* restore VIC routing (STATE is RO) */
}

static void e17_cd2401_putchar(struct uart_port *port, unsigned char ch)
{
	struct e17_cd2401_port *up =
		container_of(port, struct e17_cd2401_port, port);

	e17_cd2401_tx_one(up, ch);
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

static unsigned int e17_cd2401_tx_empty(struct uart_port *port)
{
	return TIOCSER_TEMT;		/* transmit is polled and immediate */
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
}

static void e17_cd2401_start_tx(struct uart_port *port)
{
	e17_cd2401_tx_chars(port);
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
		if (ret) {
			free_irq(port->irq, port);
			return ret;
		}
	}

	uart_port_lock_irqsave(port, &flags);

	cd_write(port, CD2401_CAR, 0);		/* console channel */
	cd_write(port, CD2401_CMR, CD2401_CMR_ASYNC);
	cd_write(port, CD2401_LIVR, up->livr);
	cd_write(port, CD2401_RPILR, CD2401_RX_PILR);	/* answers level-5 IACK */
	cd_write(port, CD2401_TPILR, CD2401_TX_IPL);	/* private polled-tx window */
	cd_write(port, CD2401_CCR, CD2401_CCR_ENBRX | CD2401_CCR_ENBXMTR);
	cd_write(port, CD2401_IER, CD2401_IER_RXD);

	uart_port_unlock_irqrestore(port, flags);

	/* Route LIRQ6 to the CPU (VIC unmasks LICR6, refcounted across sources). */
	enable_irq(port->irq);
	if (up->rxexc_irq > 0)
		enable_irq(up->rxexc_irq);

	return 0;
}

static void e17_cd2401_shutdown(struct uart_port *port)
{
	struct e17_cd2401_port *up =
		container_of(port, struct e17_cd2401_port, port);
	unsigned long flags;

	uart_port_lock_irqsave(port, &flags);
	e17_cd2401_stop_rx(port);
	uart_port_unlock_irqrestore(port, flags);

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

	/* Interrupt index 0 = rx-data, 1 = rx-exception (interrupt-names). */
	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;
	port->irq = irq;
	up->rxexc_irq = platform_get_irq_optional(pdev, 1);

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
