// SPDX-License-Identifier: GPL-2.0+
/*
 * Much copy and paste of sifive.c
 */

#include <linux/clk.h>
#include <linux/console.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/serial_core.h>
#include <linux/serial_reg.h>
#include <linux/slab.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>

#define MC68328_SERIAL_MAX_PORTS	1
#define MC68328_DEFAULT_BAUD_RATE	9600
#define MC68328_SERIAL_NAME			"68328-serial"
#define MC68328_TTY_PREFIX			"ttyDB"
#define MC68328_TX_FIFO_DEPTH		8
#define MC68328_RX_FIFO_DEPTH		12

#define REG_UCNT		0x0
#define UCNT_THAE		BIT(0)
#define UCNT_TXHE		BIT(1)
#define UCNT_TXEE		BIT(2)
#define UCNT_RXRE		BIT(3)
#define UCNT_TXEN		BIT(13)
#define UCNT_RXEN		BIT(14)
#define REG_URX			0x4
#define URX_DATAREADY	BIT(13)
#define REG_UTX			0x6
#define UTX_TXAVAIL		BIT(13)
#define UTX_TXEMPTY		BIT(15)

struct mc68328_serial_port {
	struct uart_port	port;
	struct device		*dev;
	unsigned long		baud_rate;
	struct clk			*clk;
};

#define port_to_mc68328_serial_port(p) (container_of((p), \
						    struct mc68328_serial_port, \
						    port))

/*
 * Linux serial API functions
 */
static void mc68328_serial_start_tx(struct uart_port *port)
{
	struct mc68328_serial_port *ssp = port_to_mc68328_serial_port(port);
	u16 ucnt;

	ucnt = readw(port->membase + REG_UCNT);
	ucnt |= UCNT_TXEE;
	writew(ucnt , port->membase + REG_UCNT);
}

static void mc68328_serial_stop_tx(struct uart_port *port)
{
	struct mc68328_serial_port *ssp = port_to_mc68328_serial_port(port);
	u16 ucnt;

	/*
	 * Just disable the TX interrupt, don't disable the transmitter
	 * as that results in sometimes bytes getting trapped in the fifo.
	 */
	ucnt = readw(port->membase + REG_UCNT);
	ucnt &= ~UCNT_TXEE;
	writew(ucnt , port->membase + REG_UCNT);
}

static void mc68328_serial_stop_rx(struct uart_port *port)
{
	struct mc68328_serial_port *ssp = port_to_mc68328_serial_port(port);
	u16 ucnt;

	ucnt = readw(port->membase + REG_UCNT);
	ucnt &= ~(UCNT_RXEN | UCNT_RXRE);
	writew(ucnt , port->membase + REG_UCNT);
}

static bool mc68328_serial_can_transmit(struct mc68328_serial_port *ssp)
{
	u16 utx = readw(ssp->port.membase + REG_UTX);

	return utx & UTX_TXAVAIL ? true : false;
}

static void mc68328_serial_transmit_char(struct mc68328_serial_port *ssp, u8 ch)
{
	writeb(ch, ssp->port.membase + REG_UTX + 1);
}

static void mc68328_serial_do_tx(struct mc68328_serial_port *ssp)
{
	unsigned int pending;
	u8 ch;

	/* Quickly return if this was a RX interrupt only */
	if (!mc68328_serial_can_transmit(ssp))
		return;

	uart_port_tx_limited(&ssp->port, ch, MC68328_TX_FIFO_DEPTH,
		mc68328_serial_can_transmit(ssp),
		mc68328_serial_transmit_char(ssp, ch),
		({}));
}

static void mc68328_serial_do_rx(struct mc68328_serial_port *ssp)
{
	u16 urx;

	do {
		unsigned char ch;
		urx = readw(ssp->port.membase + REG_URX);
		if (!(urx & URX_DATAREADY))
			break;

		ch = urx & 0xff;
		ssp->port.icount.rx++;
		uart_insert_char(&ssp->port, 0, 0, ch, TTY_NORMAL);
	} while (urx & URX_DATAREADY);

	tty_flip_buffer_push(&ssp->port.state->port);
}

static irqreturn_t mc68328_serial_irq(int irq, void *dev_id)
{
	struct mc68328_serial_port *ssp = dev_id;

	uart_port_lock(&ssp->port);

	mc68328_serial_do_rx(ssp);
	mc68328_serial_do_tx(ssp);

	uart_port_unlock(&ssp->port);

	return IRQ_HANDLED;
}

static unsigned int mc68328_serial_tx_empty(struct uart_port *port)
{
	struct mc68328_serial_port *ssp = port_to_mc68328_serial_port(port);
	u16 utx = readw(ssp->port.membase + REG_UTX);

	return (utx & UTX_TXEMPTY) ? TIOCSER_TEMT : 0;
}

static unsigned int mc68328_serial_get_mctrl(struct uart_port *port)
{
	return TIOCM_CAR | TIOCM_CTS | TIOCM_DSR;
}

static void mc68328_serial_set_mctrl(struct uart_port *port, unsigned int mctrl)
{

}

static void mc68328_serial_break_ctl(struct uart_port *port, int break_state)
{

}

static int mc68328_serial_startup(struct uart_port *port)
{
	struct mc68328_serial_port *ssp = port_to_mc68328_serial_port(port);
	u16 ucnt;

	ucnt = readw(port->membase + REG_UCNT);

	/* Enable RX and RX int */
	ucnt |= UCNT_RXEN | UCNT_RXRE;

	/* Enable TX */
	ucnt |= UCNT_TXEN;

	/* Disable all TX interrupts */
	ucnt &= ~(UCNT_THAE | UCNT_TXHE | UCNT_TXEE);

	writew(ucnt , port->membase + REG_UCNT);

	return 0;
}

static void mc68328_serial_shutdown(struct uart_port *port)
{
	struct mc68328_serial_port *ssp = port_to_mc68328_serial_port(port);
}

static void mc68328_serial_set_termios(struct uart_port *port,
				      struct ktermios *termios,
				      const struct ktermios *old)
{
	struct mc68328_serial_port *ssp = port_to_mc68328_serial_port(port);
}

static void mc68328_serial_release_port(struct uart_port *port)
{
}

static int mc68328_serial_request_port(struct uart_port *port)
{
	return 0;
}

static void mc68328_serial_config_port(struct uart_port *port, int flags)
{
	struct mc68328_serial_port *ssp = port_to_mc68328_serial_port(port);

}

static int mc68328_serial_verify_port(struct uart_port *port,
				     struct serial_struct *ser)
{
	return -EINVAL;
}

static const char *mc68328_serial_type(struct uart_port *port)
{
	return NULL;
}

/*
 * Early console support
 */
#ifdef CONFIG_SERIAL_EARLYCON
static void early_mc68328_serial_putc(struct uart_port *port, unsigned char c)
{
	u16 utx;

	do {
		utx = readw(port->membase + REG_UTX);
	} while (!(utx & UTX_TXAVAIL));

	writeb(c, port->membase + REG_UTX + 1);
}

static void early_mc68328_serial_write(struct console *con, const char *s,
				      unsigned int n)
{
	struct earlycon_device *dev = con->data;
	struct uart_port *port = &dev->port;

	uart_console_write(port, s, n, early_mc68328_serial_putc);
}

static int __init early_mc68328_serial_setup(struct earlycon_device *dev,
					    const char *options)
{
	struct uart_port *port = &dev->port;

	if (!port->membase)
		return -ENODEV;

	dev->con->write = early_mc68328_serial_write;

	return 0;
}

OF_EARLYCON_DECLARE(mc68ez328, "motorola,mc68ez328-uart", early_mc68328_serial_setup);
#endif /* CONFIG_SERIAL_EARLYCON */

/*
 * Linux console interface
 */
static struct mc68328_serial_port *mc68328_serial_console_ports[MC68328_SERIAL_MAX_PORTS];

static void mc68328_serial_console_putchar(struct uart_port *port, unsigned char ch)
{
	struct mc68328_serial_port *ssp = port_to_mc68328_serial_port(port);

	u16 utx;

	do {
		utx = readw(port->membase + REG_UTX);
	} while (!(utx & UTX_TXAVAIL));

	writeb(ch, port->membase + REG_UTX + 1);
}

static void mc68328_serial_console_write(struct console *co, const char *s,
					unsigned int count)
{
	struct mc68328_serial_port *ssp = mc68328_serial_console_ports[co->index];

	uart_port_lock(&ssp->port);
	uart_console_write(&ssp->port, s, count, mc68328_serial_console_putchar);
	uart_port_unlock(&ssp->port);
}

static int mc68328_serial_console_setup(struct console *co, char *options)
{
	struct mc68328_serial_port *ssp;

	if (co->index < 0 || co->index >= MC68328_SERIAL_MAX_PORTS)
		return -ENODEV;

	ssp = mc68328_serial_console_ports[co->index];
	if (!ssp)
		return -ENODEV;

	return 0;
}

static struct uart_driver mc68328_serial_uart_driver;

static struct console mc68328_serial_console = {
	.name		= MC68328_TTY_PREFIX,
	.write		= mc68328_serial_console_write,
	.device		= uart_console_device,
	.setup		= mc68328_serial_console_setup,
	.flags		= CON_PRINTBUFFER,
	.index		= -1,
	.data		= &mc68328_serial_uart_driver,
};

static int __init mc68328_serial_console_init(void)
{
	register_console(&mc68328_serial_console);
	return 0;
}
console_initcall(mc68328_serial_console_init);

static void mc68328_serial_add_console_port(struct mc68328_serial_port *ssp)
{
	mc68328_serial_console_ports[ssp->port.line] = ssp;
}

static void mc68328_serial_remove_console_port(struct mc68328_serial_port *ssp)
{
	mc68328_serial_console_ports[ssp->port.line] = NULL;
}

#define MC68328_SERIAL_CONSOLE	(&mc68328_serial_console)

static const struct uart_ops mc68328_serial_uops = {
	.tx_empty		= mc68328_serial_tx_empty,
	.set_mctrl		= mc68328_serial_set_mctrl,
	.get_mctrl		= mc68328_serial_get_mctrl,
	.start_tx		= mc68328_serial_start_tx,
	.stop_tx		= mc68328_serial_stop_tx,
	.stop_rx		= mc68328_serial_stop_rx,
	.break_ctl		= mc68328_serial_break_ctl,
	.startup		= mc68328_serial_startup,
	.shutdown		= mc68328_serial_shutdown,
	.set_termios	= mc68328_serial_set_termios,
	.type			= mc68328_serial_type,
	.release_port	= mc68328_serial_release_port,
	.request_port	= mc68328_serial_request_port,
	.config_port	= mc68328_serial_config_port,
	.verify_port	= mc68328_serial_verify_port,
};

static struct uart_driver mc68328_serial_uart_driver = {
	.owner			= THIS_MODULE,
	.driver_name	= MC68328_SERIAL_NAME,
	.dev_name		= MC68328_TTY_PREFIX,
	.nr				= MC68328_SERIAL_MAX_PORTS,
	.cons			= MC68328_SERIAL_CONSOLE,
};

static int mc68328_serial_probe(struct platform_device *pdev)
{
	struct mc68328_serial_port *ssp;
	struct resource *mem;
	struct clk *clk;
	void __iomem *base;
	int irq, id, r;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return -EPROBE_DEFER;

	base = devm_platform_get_and_ioremap_resource(pdev, 0, &mem);
	if (!base)
		return -ENOMEM;

	//clk = devm_clk_get_enabled(&pdev->dev, NULL);
	//if (IS_ERR(clk)) {
	//	return PTR_ERR(clk);
	//}

	id = of_alias_get_id(pdev->dev.of_node, "serial");
	if (id < 0) {
		return id;
	}

	if (id > MC68328_SERIAL_MAX_PORTS)
		return -EINVAL;

	ssp = devm_kzalloc(&pdev->dev, sizeof(*ssp), GFP_KERNEL);
	if (!ssp)
		return -ENOMEM;

	ssp->port.dev = &pdev->dev;
	ssp->port.type = 1;
	ssp->port.iotype = UPIO_MEM;
	ssp->port.irq = irq;
	ssp->port.fifosize = MC68328_TX_FIFO_DEPTH;
	ssp->port.ops = &mc68328_serial_uops;
	ssp->port.line = id;
	ssp->port.mapbase = mem->start;
	ssp->port.membase = base;
	ssp->dev = &pdev->dev;
	ssp->clk = clk;

	/* Set up clock divider */
	//ssp->port.uartclk = clk_get_rate(ssp->clk);
	//ssp->baud_rate = MC68328_DEFAULT_BAUD_RATE;

	platform_set_drvdata(pdev, ssp);

	r = request_irq(ssp->port.irq, mc68328_serial_irq, ssp->port.irqflags,
			dev_name(&pdev->dev), ssp);
	if (r) {
		goto probe_out2;
	}

	mc68328_serial_add_console_port(ssp);

	r = uart_add_one_port(&mc68328_serial_uart_driver, &ssp->port);
	if (r != 0) {
		goto probe_out3;
	}

	return 0;

probe_out3:
	mc68328_serial_remove_console_port(ssp);
	free_irq(ssp->port.irq, ssp);
probe_out2:
probe_out1:
	return r;
}

static int mc68328_serial_remove(struct platform_device *dev)
{
	struct mc68328_serial_port *ssp = platform_get_drvdata(dev);

	mc68328_serial_remove_console_port(ssp);
	uart_remove_one_port(&mc68328_serial_uart_driver, &ssp->port);
	free_irq(ssp->port.irq, ssp);

	return 0;
}

static const struct of_device_id mc68328_serial_of_match[] = {
	{ .compatible = "motorola,mc68ez328-uart" },
	{},
};
MODULE_DEVICE_TABLE(of, mc68328_serial_of_match);

static struct platform_driver mc68328_serial_platform_driver = {
	.probe		= mc68328_serial_probe,
	.remove		= mc68328_serial_remove,
	.driver		= {
		.name	= MC68328_SERIAL_NAME,
		.of_match_table = mc68328_serial_of_match,
	},
};

static int __init mc68328_serial_init(void)
{
	int r;

	r = uart_register_driver(&mc68328_serial_uart_driver);
	if (r)
		goto init_out1;

	r = platform_driver_register(&mc68328_serial_platform_driver);
	if (r)
		goto init_out2;

	return 0;

init_out2:
	uart_unregister_driver(&mc68328_serial_uart_driver);
init_out1:
	return r;
}

static void __exit mc68328_serial_exit(void)
{
	platform_driver_unregister(&mc68328_serial_platform_driver);
	uart_unregister_driver(&mc68328_serial_uart_driver);
}

module_init(mc68328_serial_init);
module_exit(mc68328_serial_exit);

MODULE_DESCRIPTION("mc68xx328 UART serial driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Daniel Palmer <daniel@thingy.jp>");
