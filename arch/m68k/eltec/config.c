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
#include <linux/io.h>

#include <asm/bootinfo.h>
#include <asm/bootinfo-vme.h>
#include <asm/byteorder.h>
#include <asm/setup.h>
#include <asm/machdep.h>
#include <asm/traps.h>

#include "eltec.h"

/*
 * Cirrus CD2401 serial controller at 0xfec64000, channel 0 = the RMON
 * console (Serial Port 1).  Registers used for a polled transmit:
 *   +0xee CAR   - channel select (write the channel number)
 *   +0x62 TDR   - transmit data
 *   +0x60 ...   - (see the qemu cd2401 model / u-boot driver)
 * This is only the boot/early console; a real tty driver comes later.
 */
#define E17_CD2401_BASE		0xfec64000
#define CD2401_CAR		0xee
#define CD2401_TDR		0x62
#define CD2401_TFTC		0x80	/* transmit FIFO transfer count */

static void __iomem *e17_cd2401;

static void e17_cons_putc(char c)
{
	/* wait for room, then push one byte to channel 0's TX FIFO */
	int timeout = 200000;

	out_8(e17_cd2401 + CD2401_CAR, 0);
	while (in_8(e17_cd2401 + CD2401_TFTC) == 0 && --timeout)
		cpu_relax();
	out_8(e17_cd2401 + CD2401_TDR, c);
}

static void e17_cons_write(struct console *co, const char *s, unsigned int n)
{
	while (n--) {
		if (*s == '\n')
			e17_cons_putc('\r');
		e17_cons_putc(*s++);
	}
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

static void __init eltec_e17_init_IRQ(void)
{
	/* onboard interrupters are routed through the VIC068A; a proper
	 * irqchip is TODO -- use the generic user-vector setup for now. */
	m68k_setup_user_interrupt(VEC_USER, 192);
}

void __init config_eltec_e17(void)
{
	mach_init_IRQ	= eltec_e17_init_IRQ;
	mach_get_model	= eltec_e17_get_model;

	if (!vme_brdtype)
		vme_brdtype = VME_TYPE_E17;

	/* bring up the boot console as early as possible */
	e17_cd2401 = ioremap(E17_CD2401_BASE, 0x100);
	if (e17_cd2401)
		register_console(&e17_early_console);
}
