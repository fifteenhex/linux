/*
 * drivers/char/vme_scc.c: MVME147, MVME162, BVME6000 SCC serial ports
 * implementation.
 * Copyright 1999 Richard Hirst <richard@sleepie.demon.co.uk>
 *
 * Based on atari_SCC.c which was
 *   Copyright 1994-95 Roman Hodek <Roman.Hodek@informatik.uni-erlangen.de>
 *   Partially based on PC-Linux serial.c by Linus Torvalds and Theodore Ts'o
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file COPYING in the main directory of this archive
 * for more details.
 *
 */

#include <linux/module.h>
#include <linux/kdev_t.h>
#include <asm/io.h>
#include <linux/kernel.h>
#include <linux/ioport.h>
#include <linux/interrupt.h>
#include <linux/errno.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/mm.h>
#include <linux/serial.h>
#include <linux/fcntl.h>
#include <linux/major.h>
#include <linux/delay.h>
#include <linux/miscdevice.h>
#include <linux/console.h>
#include <linux/init.h>
#include <asm/setup.h>


#include <linux/generic_serial.h>


#define SERIAL_XMIT_SIZE PAGE_SIZE

#define SCC_MINOR_BASE	64





static struct tty_port *tty_ports[2];

/*---------------------------------------------------------------------------
 * Interface from generic_serial.c back here
 *--------------------------------------------------------------------------*/

static struct real_driver scc_real_driver = {
        scc_disable_tx_interrupts,
        scc_enable_tx_interrupts,
        scc_disable_rx_interrupts,
        scc_enable_rx_interrupts,
        scc_shutdown_port,
        scc_set_real_termios,
        scc_chars_in_buffer,
        scc_close,
        scc_hungup,
        NULL
};


static const struct tty_operations scc_ops = {
	.open	= scc_open,
	.close = gs_close,
	.write = gs_write,
	.put_char = gs_put_char,
	.flush_chars = gs_flush_chars,
	.write_room = gs_write_room,
	.chars_in_buffer = gs_chars_in_buffer,
	.flush_buffer = gs_flush_buffer,
	.ioctl = scc_ioctl,
	.throttle = scc_throttle,
	.unthrottle = scc_unthrottle,
	.set_termios = gs_set_termios,
	.stop = gs_stop,
	.start = gs_start,
	.hangup = gs_hangup,
	.break_ctl = scc_break_ctl,
};

static const struct tty_port_operations scc_port_ops = {
	.carrier_raised = scc_carrier_raised,
};


/*---------------------------------------------------------------------------
 * generic_serial.c callback funtions
 *--------------------------------------------------------------------------*/




static bool scc_carrier_raised(struct tty_port *port)
{
	struct scc_port *sc = container_of(port, struct scc_port, gs.port);
	unsigned channel = sc->channel;

	return !!(scc_last_status_reg[channel] & SR_DCD);
}


static void scc_shutdown_port(void *ptr)
{
	struct scc_port *port = ptr;

	port->gs.port.flags &= ~ GS_ACTIVE;
	if (port->gs.port.tty && (port->gs.port.tty->termios.c_cflag & HUPCL)) {
		scc_setsignals (port, 0, 0);
	}
}


static int scc_set_real_termios (void *ptr)
{
	/* the SCC has char sizes 5,7,6,8 in that order! */
	static int chsize_map[4] = { 0, 2, 1, 3 };
	unsigned cflag, baud, chsize, channel, brgval = 0;
	unsigned long flags;
	struct scc_port *port = ptr;
	SCC_ACCESS_INIT(port);

	if (!port->gs.port.tty) return 0;

	channel = port->channel;

	if (channel == CHANNEL_A)
		return 0;		/* Settings controlled by boot PROM */

	cflag  = port->gs.port.tty->termios.c_cflag;
	baud = port->gs.baud;
	chsize = (cflag & CSIZE) >> 4;

	if (baud == 0) {
		/* speed == 0 -> drop DTR */
		local_irq_save(flags);
		SCCmod(TX_CTRL_REG, ~TCR_DTR, 0);
		local_irq_restore(flags);
		return 0;
	}
	else if ((MACH_IS_MVME16x && (baud < 50 || baud > 38400)) ||
		 (MACH_IS_MVME147 && (baud < 50 || baud > 19200)) ||
		 (MACH_IS_BVME6000 &&(baud < 50 || baud > 76800))) {
		printk(KERN_NOTICE "SCC: Bad speed requested, %d\n", baud);
		return 0;
	}

	tty_port_set_check_carrier(&port->gs.port, ~cflag & CLOCAL);

#ifdef CONFIG_MVME147_SCC
	if (MACH_IS_MVME147)
		brgval = (M147_SCC_PCLK + baud/2) / (16 * 2 * baud) - 2;
#endif
#ifdef CONFIG_MVME162_SCC
	if (MACH_IS_MVME16x)
		brgval = (MVME_SCC_PCLK + baud/2) / (16 * 2 * baud) - 2;
#endif
#ifdef CONFIG_BVME6000_SCC
	if (MACH_IS_BVME6000)
		brgval = (BVME_SCC_RTxC + baud/2) / (16 * 2 * baud) - 2;
#endif
	/* Now we have all parameters and can go to set them: */
	local_irq_save(flags);

	/* receiver's character size and auto-enables */
	SCCmod(RX_CTRL_REG, ~(RCR_CHSIZE_MASK|RCR_AUTO_ENAB_MODE),
			(chsize_map[chsize] << 6) |
			((cflag & CRTSCTS) ? RCR_AUTO_ENAB_MODE : 0));
	/* parity and stop bits (both, Tx and Rx), clock mode never changes */
	SCCmod (AUX1_CTRL_REG,
		~(A1CR_PARITY_MASK | A1CR_MODE_MASK),
		((cflag & PARENB
		  ? (cflag & PARODD ? A1CR_PARITY_ODD : A1CR_PARITY_EVEN)
		  : A1CR_PARITY_NONE)
		 | (cflag & CSTOPB ? A1CR_MODE_ASYNC_2 : A1CR_MODE_ASYNC_1)));
	/* sender's character size, set DTR for valid baud rate */
	SCCmod(TX_CTRL_REG, ~TCR_CHSIZE_MASK, chsize_map[chsize] << 5 | TCR_DTR);
	/* clock sources never change */
	/* disable BRG before changing the value */
	SCCmod(DPLL_CTRL_REG, ~DCR_BRG_ENAB, 0);
	/* BRG value */
	SCCwrite(TIMER_LOW_REG, brgval & 0xff);
	SCCwrite(TIMER_HIGH_REG, (brgval >> 8) & 0xff);
	/* BRG enable, and clock source never changes */
	SCCmod(DPLL_CTRL_REG, 0xff, DCR_BRG_ENAB);

	local_irq_restore(flags);

	return 0;
}


static int scc_chars_in_buffer (void *ptr)
{
	struct scc_port *port = ptr;
	SCC_ACCESS_INIT(port);

	return (SCCread (SPCOND_STATUS_REG) & SCSR_ALL_SENT) ? 0  : 1;
}



/*---------------------------------------------------------------------------
 * Internal support functions
 *--------------------------------------------------------------------------*/

static void scc_setsignals(struct scc_port *port, int dtr, int rts)
{
	unsigned long flags;
	unsigned char t;
	SCC_ACCESS_INIT(port);

	local_irq_save(flags);
	t = SCCread(TX_CTRL_REG);
	if (dtr >= 0) t = dtr? (t | TCR_DTR): (t & ~TCR_DTR);
	if (rts >= 0) t = rts? (t | TCR_RTS): (t & ~TCR_RTS);
	SCCwrite(TX_CTRL_REG, t);
	local_irq_restore(flags);
}


static void scc_send_xchar(struct tty_struct *tty, char ch)
{
	struct scc_port *port = tty->driver_data;

	port->x_char = ch;
	if (ch)
		scc_enable_tx_interrupts(port);
}


/*---------------------------------------------------------------------------
 * Driver entrypoints referenced from above
 *--------------------------------------------------------------------------*/

static int scc_open (struct tty_struct * tty, struct file * filp)
{
	int line = tty->index;
	int retval;
	struct scc_port *port = &scc_ports[line];
	int i, channel = port->channel;
	unsigned long	flags;

	
	tty->driver_data = port;
	port->gs.port.tty = tty;
	port->gs.port.count++;
	retval = gs_init_port(&port->gs);
	if (retval) {
		port->gs.port.count--;
		return retval;
	}
	port->gs.port.flags |= GS_ACTIVE;
	retval = gs_block_til_ready(port, filp);

	if (retval) {
		port->gs.port.count--;
		return retval;
	}

	port->c_dcd = tty_port_carrier_raised(&port->gs.port);

	scc_enable_rx_interrupts(port);

	return 0;
}


static void scc_throttle (struct tty_struct * tty)
{
	struct scc_port *port = tty->driver_data;
	unsigned long	flags;
	SCC_ACCESS_INIT(port);

	if (tty->termios.c_cflag & CRTSCTS) {
		local_irq_save(flags);
		SCCmod(TX_CTRL_REG, ~TCR_RTS, 0);
		local_irq_restore(flags);
	}
	if (I_IXOFF(tty))
		scc_send_xchar(tty, STOP_CHAR(tty));
}


static void scc_unthrottle (struct tty_struct * tty)
{
	struct scc_port *port = tty->driver_data;
	unsigned long	flags;
	SCC_ACCESS_INIT(port);

	if (tty->termios.c_cflag & CRTSCTS) {
		local_irq_save(flags);
		SCCmod(TX_CTRL_REG, 0xff, TCR_RTS);
		local_irq_restore(flags);
	}
	if (I_IXOFF(tty))
		scc_send_xchar(tty, START_CHAR(tty));
}


static int scc_ioctl(struct tty_struct *tty,
		     unsigned int cmd, unsigned long arg)
{
	return -ENOIOCTLCMD;
}


static int scc_break_ctl(struct tty_struct *tty, int break_state)
{
	struct scc_port *port = tty->driver_data;
	unsigned long	flags;
	SCC_ACCESS_INIT(port);

	local_irq_save(flags);
	SCCmod(TX_CTRL_REG, ~TCR_SEND_BREAK, 
			break_state ? TCR_SEND_BREAK : 0);
	local_irq_restore(flags);
	return 0;
}
