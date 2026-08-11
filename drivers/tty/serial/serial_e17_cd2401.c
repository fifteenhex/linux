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
#define CD2401_RISRH		0x88	/* receive interrupt status, high */
#define CD2401_RISRL		0x89	/* receive interrupt status, low */
#define CD2401_TISR		0x8a	/* transmit interrupt status */
#define CD2401_TISR_TXEMPTY	0x02
#define CD2401_TPILR		0xe0	/* tx priority interrupt level */
#define CD2401_RPILR		0xe1	/* rx priority interrupt level */
#define CD2401_CAR		0xee	/* channel access (select) */
#define CD2401_DR		0xf8	/* rx/tx data */

/* interrupt-acknowledge window: address bits carry the priority level */
#define CD2401_RX_IPL		1
#define CD2401_TX_IPL		2

/* VIC068A local interrupt 6 = the CD2401 line */
#define E17_VIC_BASE		0xfec00000
#define E17_VIC_LICR6		0x3b
#define E17_VIC_LICR_MASK	0x80
#define E17_VIC_CD2401_LEVEL	6	/* m68k IPL the VIC raises for it */

#define E17_CD2401_MAX_PORTS	1
#define E17_CD2401_NAME		"ttyS"

struct e17_cd2401_port {
	struct uart_port port;
	void __iomem *iack;	/* interrupt-acknowledge window */
	void __iomem *vic;	/* VIC068A register window */
	u8 livr;		/* interrupt vector base (type bits clear) */
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

static void e17_cd2401_putchar(struct uart_port *port, unsigned char ch)
{
	cd_write(port, CD2401_CAR, 0);		/* console = channel 0 */
	while (!(cd_read(port, CD2401_TISR) & CD2401_TISR_TXEMPTY))
		cpu_relax();
	cd_write(port, CD2401_DR, ch);
}

static void e17_cd2401_tx_chars(struct uart_port *port)
{
	u8 ch;

	cd_write(port, CD2401_CAR, 0);
	uart_port_tx(port, ch,
		     cd_read(port, CD2401_TISR) & CD2401_TISR_TXEMPTY,
		     cd_write(port, CD2401_DR, ch));
}

static void e17_cd2401_rx_chars(struct uart_port *port)
{
	struct e17_cd2401_port *up =
		container_of(port, struct e17_cd2401_port, port);
	unsigned int cnt, i;

	/* enter receive service context: the ack byte selects the channel */
	readb(up->iack + CD2401_RX_IPL);

	/*
	 * Read (and clear) the receive status.  An overrun is reported as a
	 * receive exception that latches until RISR is read; clearing it
	 * keeps a fast paste from stalling the receiver.
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

static void e17_cd2401_vic_mask(struct e17_cd2401_port *up, bool mask)
{
	u8 v = mask ? E17_VIC_LICR_MASK : E17_VIC_CD2401_LEVEL;

	writeb(v, up->vic + E17_VIC_LICR6);
}

static void e17_cd2401_stop_rx(struct uart_port *port)
{
	struct e17_cd2401_port *up =
		container_of(port, struct e17_cd2401_port, port);

	cd_write(port, CD2401_CAR, 0);
	cd_write(port, CD2401_IER, cd_read(port, CD2401_IER) & ~CD2401_IER_RXD);
	e17_cd2401_vic_mask(up, true);
}

static int e17_cd2401_startup(struct uart_port *port)
{
	struct e17_cd2401_port *up =
		container_of(port, struct e17_cd2401_port, port);
	unsigned long flags;
	int ret;

	ret = request_irq(port->irq, e17_cd2401_interrupt, 0, "ttyS", port);
	if (ret)
		return ret;

	uart_port_lock_irqsave(port, &flags);

	cd_write(port, CD2401_CAR, 0);		/* console channel */
	cd_write(port, CD2401_CMR, CD2401_CMR_ASYNC);
	cd_write(port, CD2401_LIVR, up->livr);
	cd_write(port, CD2401_RPILR, CD2401_RX_IPL);
	cd_write(port, CD2401_TPILR, CD2401_TX_IPL);
	cd_write(port, CD2401_CCR, CD2401_CCR_ENBRX | CD2401_CCR_ENBXMTR);
	cd_write(port, CD2401_IER, CD2401_IER_RXD);

	/* route local IRQ 6 to the CPU at E17_VIC_CD2401_LEVEL, unmasked */
	e17_cd2401_vic_mask(up, false);

	uart_port_unlock_irqrestore(port, flags);

	return 0;
}

static void e17_cd2401_shutdown(struct uart_port *port)
{
	unsigned long flags;

	uart_port_lock_irqsave(port, &flags);
	e17_cd2401_stop_rx(port);
	uart_port_unlock_irqrestore(port, flags);

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

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;
	port->irq = irq;

	/*
	 * The chip supplies the vector; the VIC/CPU deliver it as an m68k
	 * vectored interrupt.  Recover the vector this IRQ corresponds to
	 * and program it into LIVR (with the type bits cleared - the chip
	 * fills in 11 = receive-data when it raises the line).
	 */
	up->livr = (irq - IRQ_USER + VEC_USER) & ~0x3;

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
