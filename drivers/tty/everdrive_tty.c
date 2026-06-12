#include <asm/io.h>
#include <linux/console.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/interrupt.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>
#include <linux/timer.h>

#include <asm/everdrive.h>
#include <asm/everdrive-fifo.h>

static struct tty_driver *everdrive_tty_driver = NULL;
static struct tty_port everdrive_port;
static struct timer_list everdrive_timer;
static bool everdrive_polling;

#define POLL_HZ 10
#define POLL_INTERVAL (HZ / POLL_HZ)

static volatile void *everdrive_fifo = (void *) MEGADRIVE_EVERDRIVE_MAILBOX;

static void everdrive_tty_str(u8 *chars, size_t size)
{
	tty_insert_flip_string(&everdrive_port, chars, size);
}

static void everdrive_tty_poll_done(void)
{
	tty_flip_buffer_push(&everdrive_port);

	mod_timer(&everdrive_timer, jiffies + POLL_INTERVAL);
}

static struct everdrive_fifo_msg msg = {
	.flags = EVERDRIVE_FIFO_MSG_FLAG_TTYGET,
	.cb_str = everdrive_tty_str,
	.cb_done = everdrive_tty_poll_done,
};

static void everdrive_poll(struct timer_list *t)
{
	if (!READ_ONCE(everdrive_polling))
		return;

	everdrive_fifo_enqueue(&msg);
}

static int everdrive_port_activate(struct tty_port *port, struct tty_struct *tty)
{
	WRITE_ONCE(everdrive_polling, true);

	mod_timer(&everdrive_timer, jiffies + POLL_INTERVAL);

	return 0;
}

static void everdrive_port_shutdown(struct tty_port *port)
{
	WRITE_ONCE(everdrive_polling, false);
	timer_delete_sync(&everdrive_timer);
}

static const struct tty_port_operations everdrive_port_ops = {
	.activate = everdrive_port_activate,
	.shutdown = everdrive_port_shutdown,
};

static int everdrive_tty_open(struct tty_struct *tty, struct file *filp)
{
        return tty_port_open(&everdrive_port, tty, filp);
}

static void everdrive_tty_close(struct tty_struct *tty, struct file *filp)
{
        tty_port_close(&everdrive_port, tty, filp);
}

static ssize_t everdrive_tty_write(struct tty_struct *tty, const unsigned char *buf, size_t count)
{
	u16 status;
	struct everdrive_fifo_msg msg = {
		.cmd = &cmd_usbwr,
		.flags = EVERDRIVE_FIFO_MSG_FLAG_USBWR,
		.arg_u16 = count,
		.arg_data = buf,
		.status = &status,
	};

        everdrive_fifo_enqueue_wait(&msg);

        return count;
}

static unsigned int everdrive_tty_write_room(struct tty_struct *tty)
{
	return 64;
}

static const struct tty_operations everdrive_tty_ops = {
        .open       = everdrive_tty_open,
        .close      = everdrive_tty_close,
        .write      = everdrive_tty_write,
        .write_room = everdrive_tty_write_room,
};

static int __init everdrive_tty_init(void)
{
	struct device_node *np;
	int ret;

	np = of_find_compatible_node(NULL, NULL, "krikzz,everdrive-serial");
	if (!np)
		return 0;

	of_node_put(np);

	tty_port_init(&everdrive_port);
	everdrive_port.ops = &everdrive_port_ops;

	everdrive_tty_driver = tty_alloc_driver(1, TTY_DRIVER_REAL_RAW |
						TTY_DRIVER_DYNAMIC_DEV);
        if (IS_ERR(everdrive_tty_driver))
                return PTR_ERR(everdrive_tty_driver);

	everdrive_tty_driver->driver_name  = "ttyED";
	everdrive_tty_driver->name         = "ttyED";
	everdrive_tty_driver->type         = TTY_DRIVER_TYPE_SERIAL;
	everdrive_tty_driver->subtype      = SERIAL_TYPE_NORMAL;
	everdrive_tty_driver->init_termios = tty_std_termios;
	tty_set_operations(everdrive_tty_driver, &everdrive_tty_ops);
	tty_port_link_device(&everdrive_port, everdrive_tty_driver, 0);
	ret = tty_register_driver(everdrive_tty_driver);
	if (ret) {
		tty_driver_kref_put(everdrive_tty_driver);
		tty_port_destroy(&everdrive_port);
		return ret;
        }

	timer_setup(&everdrive_timer, everdrive_poll, 0);

	tty_register_device(everdrive_tty_driver, 0, NULL);

	return 0;
}

static void __exit everdrive_tty_exit(void)
{
        if (!everdrive_tty_driver)
                return;

        WRITE_ONCE(everdrive_polling, false);
        timer_delete_sync(&everdrive_timer);
        tty_unregister_device(everdrive_tty_driver, 0);
        tty_unregister_driver(everdrive_tty_driver);
        tty_driver_kref_put(everdrive_tty_driver);
        tty_port_destroy(&everdrive_port);
}

module_init(everdrive_tty_init);
module_exit(everdrive_tty_exit);

/* Console */
static void everdrive_console_emit(struct nbcon_write_context *wctxt)
{
	unsigned int i;

	if (!nbcon_enter_unsafe(wctxt))
		return;

	for (i = 0; i < wctxt->len; i++) {
		char ch = wctxt->outbuf[i];

		if (ch == '\n')
			everdrive_usb_write('\r');
		everdrive_usb_write(ch);
	}

	nbcon_exit_unsafe(wctxt);
}

static void everdrive_console_write_atomic(struct console *co,
					   struct nbcon_write_context *wctxt)
{
	everdrive_console_emit(wctxt);
}

static void everdrive_console_write_thread(struct console *co,
					   struct nbcon_write_context *wctxt)
{
	everdrive_console_emit(wctxt);
}

static void everdrive_console_device_lock(struct console *co, unsigned long *flags)
{
	mutex_lock(&everdrive_fifo_mutex);
}

static void everdrive_console_device_unlock(struct console *co, unsigned long flags)
{
	mutex_unlock(&everdrive_fifo_mutex);
}

static struct tty_driver *everdrive_console_device(struct console *co, int *index)
{
	*index = 0;

	return everdrive_tty_driver;
}

static int everdrive_console_setup(struct console *co, char *options)
{
	return 0;
}

static struct console everdrive_console = {
        .name          = "ttyED",
        .write_atomic  = everdrive_console_write_atomic,
        .write_thread  = everdrive_console_write_thread,
        .device_lock   = everdrive_console_device_lock,
        .device_unlock = everdrive_console_device_unlock,
        .device        = everdrive_console_device,
        .setup         = everdrive_console_setup,
        .flags         = CON_PRINTBUFFER | CON_NBCON,
        .index         = 0,
};

static int __init everdrive_console_init(void)
{
	struct device_node *np;
	int ret;

	np = of_find_compatible_node(NULL, NULL, "krikzz,everdrive-serial");
	if (!np)
		return 0;

	of_node_put(np);

        register_console(&everdrive_console);

	return 0;
}

console_initcall(everdrive_console_init);

MODULE_LICENSE("GPL");
