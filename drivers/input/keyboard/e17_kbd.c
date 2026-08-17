// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ELTEC EUROCOM-17 (E17) AT keyboard controller driver.
 *
 * The E17 is an m68k dual-68040 VME single-board computer.  Its onboard
 * keyboard controller lives in the 0xfec00000 I/O window at 0xfec60000
 * ($FEC6.0000 in the ELTEC hardware manual, "3.9 Keyboard Interface").
 * It exposes two 8-bit registers:
 *
 *   +0x00  Data register    - read returns the next byte received from
 *                             the keyboard; write transmits a byte to it.
 *   +0x01  Control/Status   - read returns status; bit0 = interface
 *                             ready/present, bit1 = receive buffer full
 *                             (a byte is waiting in the data register).
 *
 * The keyboard speaks raw AT scan code set 2 (on the real machine RMON
 * translates in software).  We decode set 2 -> Linux keycodes here using
 * the same table layout as drivers/input/keyboard/atkbd.c.
 *
 * On the real hardware the controller's interrupt is wired to the
 * VIC068A LIRQ1 input, but there is no VIC irqchip yet (and the QEMU
 * model does not deliver the keyboard IRQ to the CPU at all), so this
 * driver polls the status register from a timer by default.  If the DT
 * node ever gains a usable "interrupts" property the probe will request
 * it and drive the same drain routine from interrupt context instead.
 */

#include <linux/delay.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/timer.h>

#define E17_KBD_REG_DATA	0x00
#define E17_KBD_REG_STAT	0x01

#define E17_KBD_STAT_RDY	0x01	/* interface ready/present */
#define E17_KBD_STAT_OBF	0x02	/* receive buffer full (byte waiting) */

/* Scan set 2 special bytes */
#define E17_KBD_RET_ACK		0xfa
#define E17_KBD_RET_NAK		0xfe
#define E17_KBD_RET_BAT		0xaa
#define E17_KBD_RET_RESEND	0xfe
#define E17_KBD_RET_EMUL0	0xe0	/* extended (E0) prefix */
#define E17_KBD_RET_EMUL1	0xe1	/* Pause/Break prefix */
#define E17_KBD_RET_RELEASE	0xf0	/* break (release) prefix */

/* Keyboard commands */
#define E17_KBD_CMD_ENABLE	0xf4

#define E17_KBD_POLL_MS		10

/*
 * AT scan code set 2 -> Linux keycode table.  Index 0x00..0x7f are the
 * plain scancodes; index 0x80..0xff are the E0-prefixed (extended)
 * scancodes (scancode | 0x80).  Kept in sync with atkbd_set2_keycode[]
 * in drivers/input/keyboard/atkbd.c.
 */
static const unsigned short e17_kbd_set2_keycode[256] = {
	  0, 67, 65, 63, 61, 59, 60, 88,183, 68, 66, 64, 62, 15, 41,117,
	184, 56, 42, 93, 29, 16,  2,  0,185,  0, 44, 31, 30, 17,  3,  0,
	186, 46, 45, 32, 18,  5,  4, 95,187, 57, 47, 33, 20, 19,  6,183,
	188, 49, 48, 35, 34, 21,  7,184,189,  0, 50, 36, 22,  8,  9,185,
	190, 51, 37, 23, 24, 11, 10,  0,191, 52, 53, 38, 39, 25, 12,  0,
	192, 89, 40,  0, 26, 13,  0,193, 58, 54, 28, 27,  0, 43,  0,194,
	  0, 86, 91, 90, 92,  0, 14, 94,  0, 79,124, 75, 71,121,  0,  0,
	 82, 83, 80, 76, 77, 72,  1, 69, 87, 78, 81, 74, 55, 73, 70, 99,

	  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	217,100,255,  0, 97,165,  0,  0,156,  0,  0,  0,  0,  0,  0,125,
	173,114,  0,113,  0,  0,  0,126,128,  0,  0,140,  0,  0,  0,127,
	159,  0,115,  0,164,  0,  0,116,158,  0,172,166,  0,  0,  0,142,
	157,  0,  0,  0,  0,  0,  0,  0,155,  0, 98,  0,  0,163,  0,  0,
	226,  0,  0,  0,  0,  0,  0,  0,  0,255, 96,  0,  0,  0,143,  0,
	  0,  0,  0,  0,  0,  0,  0,  0,  0,107,  0,105,102,  0,  0,112,
	110,111,108,112,106,103,  0,119,  0,118,109,  0, 99,104,119,  0,
};

struct e17_kbd {
	struct input_dev	*input;
	void __iomem		*base;
	struct timer_list	poll_timer;
	int			irq;
	spinlock_t		lock;		/* serialises the drain path   */

	/* set 2 decode state */
	bool			release;	/* 0xF0 break prefix seen */
	bool			extended;	/* 0xE0 extended prefix seen */
	unsigned int		emul1_left;	/* bytes left in a 0xE1 seq */

	unsigned short		keycode[256];
};

/* Feed one raw scan-set-2 byte through the decoder and report keys. */
static void e17_kbd_process_byte(struct e17_kbd *kbd, u8 data)
{
	struct input_dev *input = kbd->input;
	unsigned short code;

	/*
	 * The Pause/Break key is the only one prefixed by 0xE1; it comes
	 * as an 8-byte make sequence (E1 14 77 E1 F0 14 F0 77).  Emit a
	 * single KEY_PAUSE tap on the prefix and swallow the remaining 7.
	 */
	if (kbd->emul1_left) {
		kbd->emul1_left--;
		return;
	}

	switch (data) {
	case E17_KBD_RET_BAT:
	case E17_KBD_RET_ACK:
	case E17_KBD_RET_NAK:
	case 0x00:
	case 0xff:
		/* replies / errors: reset any pending prefix state */
		kbd->release = false;
		kbd->extended = false;
		return;
	case E17_KBD_RET_EMUL0:
		kbd->extended = true;
		return;
	case E17_KBD_RET_EMUL1:
		kbd->emul1_left = 7;
		input_report_key(input, KEY_PAUSE, 1);
		input_sync(input);
		input_report_key(input, KEY_PAUSE, 0);
		input_sync(input);
		return;
	case E17_KBD_RET_RELEASE:
		kbd->release = true;
		return;
	}

	code = data;
	if (kbd->extended)
		code |= 0x80;

	code = kbd->keycode[code & 0xff];
	if (code)
		input_report_key(input, code, !kbd->release);
	else
		dev_dbg(input->dev.parent,
			"unmapped scancode %s0x%02x\n",
			kbd->extended ? "e0 " : "", data);

	input_sync(input);

	kbd->release = false;
	kbd->extended = false;
}

/* Drain every byte currently waiting in the controller. */
static void e17_kbd_drain(struct e17_kbd *kbd)
{
	unsigned int guard = 256;
	unsigned long flags;
	u8 status;

	/*
	 * The IRQ handler and the polling backstop can both call this (on
	 * different CPUs under SMP), and the scan-set-2 decoder carries state
	 * across bytes (release/extended/emul1), so serialise the whole drain.
	 */
	spin_lock_irqsave(&kbd->lock, flags);
	while (guard--) {
		status = readb(kbd->base + E17_KBD_REG_STAT);
		if (!(status & E17_KBD_STAT_OBF))
			break;
		e17_kbd_process_byte(kbd, readb(kbd->base + E17_KBD_REG_DATA));
	}
	spin_unlock_irqrestore(&kbd->lock, flags);
}

static void e17_kbd_poll(struct timer_list *t)
{
	struct e17_kbd *kbd = timer_container_of(kbd, t, poll_timer);

	e17_kbd_drain(kbd);
	mod_timer(&kbd->poll_timer,
		  jiffies + msecs_to_jiffies(E17_KBD_POLL_MS));
}

static irqreturn_t e17_kbd_interrupt(int irq, void *dev_id)
{
	struct e17_kbd *kbd = dev_id;

	e17_kbd_drain(kbd);
	return IRQ_HANDLED;
}

static int e17_kbd_open(struct input_dev *input)
{
	struct e17_kbd *kbd = input_get_drvdata(input);

	/* Ask the keyboard to (re)start scanning; reply is drained below. */
	if (readb(kbd->base + E17_KBD_REG_STAT) & E17_KBD_STAT_RDY)
		writeb(E17_KBD_CMD_ENABLE, kbd->base + E17_KBD_REG_DATA);

	e17_kbd_drain(kbd);

	/*
	 * Always run the poll timer: with no IRQ it is the only source, and with
	 * an IRQ it is a cheap backstop until VIC LIRQ1 delivery is confirmed on
	 * real hardware (QEMU never drives it).  Both paths share kbd->lock.
	 */
	mod_timer(&kbd->poll_timer,
		  jiffies + msecs_to_jiffies(E17_KBD_POLL_MS));

	return 0;
}

static void e17_kbd_close(struct input_dev *input)
{
	struct e17_kbd *kbd = input_get_drvdata(input);

	timer_delete_sync(&kbd->poll_timer);
}

static int e17_kbd_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct e17_kbd *kbd;
	struct input_dev *input;
	int i, error;

	kbd = devm_kzalloc(dev, sizeof(*kbd), GFP_KERNEL);
	if (!kbd)
		return -ENOMEM;

	kbd->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(kbd->base))
		return PTR_ERR(kbd->base);

	input = devm_input_allocate_device(dev);
	if (!input)
		return -ENOMEM;

	kbd->input = input;
	spin_lock_init(&kbd->lock);
	timer_setup(&kbd->poll_timer, e17_kbd_poll, 0);

	input->name = "ELTEC E17 AT keyboard";
	input->phys = "e17-kbd/input0";
	input->id.bustype = BUS_HOST;
	input->id.vendor = 0x0001;
	input->id.product = 0x0001;
	input->id.version = 0x0100;
	input->dev.parent = dev;
	input->open = e17_kbd_open;
	input->close = e17_kbd_close;

	input->keycode = kbd->keycode;
	input->keycodesize = sizeof(kbd->keycode[0]);
	input->keycodemax = ARRAY_SIZE(kbd->keycode);

	memcpy(kbd->keycode, e17_kbd_set2_keycode, sizeof(kbd->keycode));

	__set_bit(EV_KEY, input->evbit);
	__set_bit(EV_REP, input->evbit);
	for (i = 0; i < ARRAY_SIZE(kbd->keycode); i++)
		if (kbd->keycode[i])
			__set_bit(kbd->keycode[i], input->keybit);
	__clear_bit(KEY_RESERVED, input->keybit);
	__set_bit(KEY_PAUSE, input->keybit);

	input_set_drvdata(input, kbd);

	/*
	 * Optional interrupt: the E17 wires the controller to VIC LIRQ1,
	 * but until a VIC irqchip exists the DT carries no usable line and
	 * we fall back to polling.  Wire it up transparently if present.
	 */
	kbd->irq = platform_get_irq_optional(pdev, 0);
	if (kbd->irq > 0) {
		error = devm_request_irq(dev, kbd->irq, e17_kbd_interrupt,
					 0, "e17-kbd", kbd);
		if (error) {
			dev_err(dev, "failed to request IRQ %d: %d\n",
				kbd->irq, error);
			return error;
		}
		dev_info(dev, "using IRQ %d\n", kbd->irq);
	} else {
		kbd->irq = 0;
		dev_info(dev, "no usable IRQ, polling every %d ms\n",
			 E17_KBD_POLL_MS);
	}

	error = input_register_device(input);
	if (error)
		return error;

	platform_set_drvdata(pdev, kbd);
	return 0;
}

static const struct of_device_id e17_kbd_of_match[] = {
	{ .compatible = "eltec,e17-kbd" },
	{ }
};
MODULE_DEVICE_TABLE(of, e17_kbd_of_match);

static struct platform_driver e17_kbd_driver = {
	.probe	= e17_kbd_probe,
	.driver	= {
		.name		= "e17-kbd",
		.of_match_table	= e17_kbd_of_match,
	},
};
module_platform_driver(e17_kbd_driver);

MODULE_DESCRIPTION("ELTEC EUROCOM-17 AT keyboard controller driver");
MODULE_LICENSE("GPL");
