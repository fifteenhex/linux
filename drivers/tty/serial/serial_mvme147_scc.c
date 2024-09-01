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
#include <linux/platform_device.h>
#include <linux/serial_core.h>
#include <linux/serial_reg.h>
#include <linux/slab.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <asm/bootinfo.h>
#include <asm/bootinfo-vme.h>

#include <asm/mvme147hw.h>
#include <asm/mvme16xhw.h>
#include <asm/bvme6000hw.h>

#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>
#include <linux/property.h>

#define MVME147_SCC_SERIAL_MAX_PORTS	2
#define MVME147_SCC_DEFAULT_BAUD_RATE	9600
#define MVME147_SCC_SERIAL_NAME		"mvme147-scc-serial"
#define MVME147_SCC_TTY_PREFIX		"ttyS"
#define MVME147_SCC_TX_FIFO_DEPTH	1
#define MVME147_SCC_RX_FIFO_DEPTH	1

#define CHANNEL_A	0
#define CHANNEL_B	1

#include <linux/delay.h>

/* Clock sources */

#define CLK_RTxC	0
#define CLK_TRxC	1
#define CLK_PCLK	2

/* baud_bases for the common clocks in the Atari. These are the real
 * frequencies divided by 16.
 */

#define SCC_BAUD_BASE_TIMC	19200	/* 0.3072 MHz from TT-MFP, Timer C */
#define SCC_BAUD_BASE_BCLK	153600	/* 2.4576 MHz */
#define SCC_BAUD_BASE_PCLK4	229500	/* 3.6720 MHz */
#define SCC_BAUD_BASE_PCLK	503374	/* 8.0539763 MHz */
#define SCC_BAUD_BASE_NONE	0	/* for not connected or unused
					 * clock sources */

/* The SCC clock configuration structure */

struct scc_clock_config {
	unsigned RTxC_base;	/* base_baud of RTxC */
	unsigned TRxC_base;	/* base_baud of TRxC */
	unsigned PCLK_base;	/* base_baud of PCLK, both channels! */
	struct {
		unsigned clksrc;	/* CLK_RTxC, CLK_TRxC or CLK_PCLK */
		unsigned divisor;	/* divisor for base baud, valid values:
					 * see below */
	} baud_table[17];		/* For 50, 75, 110, 135, 150, 200, 300,
					 * 600, 1200, 1800, 2400, 4800, 9600,
					 * 19200, 38400, 57600 and 115200 bps.
					 * The last two could be replaced by
					 * other rates > 38400 if they're not
					 * possible.
					 */
};

/* The following divisors are valid:
 *
 *   - CLK_RTxC: 1 or even (1, 2 and 4 are the direct modes, > 4 use
 *               the BRG)
 *
 *   - CLK_TRxC: 1, 2 or 4 (no BRG, only direct modes possible)
 *
 *   - CLK_PCLK: >= 4 and even (no direct modes, only BRG)
 *
 */

struct scc_port {
	volatile unsigned char	*ctrlp;
	volatile unsigned char	*datap;
	int			x_char;		/* xon/xoff character */
	int			c_dcd;
	int			channel;
	struct scc_port		*port_a;	/* Reference to port A and B */
	struct scc_port		*port_b;	/*   structs for reg access  */
};

/***********************************************************************/
/*                                                                     */
/*                             Register Names                          */
/*                                                                     */
/***********************************************************************/

/* The SCC documentation gives no explicit names to the registers,
 * they're just called WR0..15 and RR0..15. To make the source code
 * better readable and make the transparent write reg read access (see
 * below) possible, I christen them here with self-invented names.
 * Note that (real) read registers are assigned numbers 16..31. WR7'
 * has number 33.
 */

#define	COMMAND_REG		0	/* wo */
#define	INT_AND_DMA_REG		1	/* wo */
#define	INT_VECTOR_REG		2	/* rw, common to both channels */
#define	RX_CTRL_REG		3	/* rw */
#define	AUX1_CTRL_REG		4	/* rw */
#define	TX_CTRL_REG		5	/* rw */
#define	SYNC_ADR_REG		6	/* wo */
#define	SYNC_CHAR_REG		7	/* wo */
#define	SDLC_OPTION_REG		33	/* wo */
#define	TX_DATA_REG		8	/* wo */
#define	MASTER_INT_CTRL		9	/* wo, common to both channels */
#define	AUX2_CTRL_REG		10	/* rw */
#define	CLK_CTRL_REG		11	/* wo */
#define	TIMER_LOW_REG		12	/* rw */
#define	TIMER_HIGH_REG		13	/* rw */
#define	DPLL_CTRL_REG		14	/* wo */
#define	INT_CTRL_REG		15	/* rw */

#define	STATUS_REG		16	/* ro */
#define	SPCOND_STATUS_REG	17	/* wo */
/* RR2 is WR2 for Channel A, Channel B gives vector + current status: */
#define	CURR_VECTOR_REG		18	/* Ch. B only, Ch. A for rw */
#define	INT_PENDING_REG		19	/* Channel A only! */
/* RR4 is WR4, if b6(MR7') == 1 */
/* RR5 is WR5, if b6(MR7') == 1 */
#define	FS_FIFO_LOW_REG		22	/* ro */
#define	FS_FIFO_HIGH_REG	23	/* ro */
#define	RX_DATA_REG		24	/* ro */
/* RR9 is WR3, if b6(MR7') == 1 */
#define	DPLL_STATUS_REG		26	/* ro */
/* RR11 is WR10, if b6(MR7') == 1 */
/* RR12 is WR12 */
/* RR13 is WR13 */
/* RR14 not present */
/* RR15 is WR15 */


/***********************************************************************/
/*                                                                     */
/*                             Register Values                         */
/*                                                                     */
/***********************************************************************/


/* WR0: COMMAND_REG "CR" */

#define	CR_RX_CRC_RESET		0x40
#define	CR_TX_CRC_RESET		0x80
#define	CR_TX_UNDERRUN_RESET	0xc0

#define	CR_EXTSTAT_RESET	0x10
#define	CR_SEND_ABORT		0x18
#define	CR_ENAB_INT_NEXT_RX	0x20
#define	CR_TX_PENDING_RESET	0x28
#define	CR_ERROR_RESET		0x30
#define	CR_HIGHEST_IUS_RESET	0x38


/* WR1: INT_AND_DMA_REG "IDR" */

#define	IDR_EXTSTAT_INT_ENAB	0x01
#define	IDR_TX_INT_ENAB		0x02
#define	IDR_PARERR_AS_SPCOND	0x04

#define	IDR_RX_INT_DISAB	0x00
#define	IDR_RX_INT_FIRST	0x08
#define	IDR_RX_INT_ALL		0x10
#define	IDR_RX_INT_SPCOND	0x18
#define	IDR_RX_INT_MASK		0x18

#define	IDR_WAITREQ_RX		0x20
#define	IDR_WAITREQ_IS_REQ	0x40
#define	IDR_WAITREQ_ENAB	0x80


/* WR3: RX_CTRL_REG "RCR" */

#define	RCR_RX_ENAB		0x01
#define	RCR_DISCARD_SYNC_CHARS	0x02
#define	RCR_ADDR_SEARCH		0x04
#define	RCR_CRC_ENAB		0x08
#define	RCR_SEARCH_MODE		0x10
#define	RCR_AUTO_ENAB_MODE	0x20

#define	RCR_CHSIZE_MASK		0xc0
#define	RCR_CHSIZE_5		0x00
#define	RCR_CHSIZE_6		0x40
#define	RCR_CHSIZE_7		0x80
#define	RCR_CHSIZE_8		0xc0


/* WR4: AUX1_CTRL_REG "A1CR" */

#define	A1CR_PARITY_MASK	0x03
#define	A1CR_PARITY_NONE	0x00
#define	A1CR_PARITY_ODD		0x01
#define	A1CR_PARITY_EVEN	0x03

#define	A1CR_MODE_MASK		0x0c
#define	A1CR_MODE_SYNCR		0x00
#define	A1CR_MODE_ASYNC_1	0x04
#define	A1CR_MODE_ASYNC_15	0x08
#define	A1CR_MODE_ASYNC_2	0x0c

#define	A1CR_SYNCR_MODE_MASK	0x30
#define	A1CR_SYNCR_MONOSYNC	0x00
#define	A1CR_SYNCR_BISYNC	0x10
#define	A1CR_SYNCR_SDLC		0x20
#define	A1CR_SYNCR_EXTCSYNC	0x30

#define	A1CR_CLKMODE_MASK	0xc0
#define	A1CR_CLKMODE_x1		0x00
#define	A1CR_CLKMODE_x16	0x40
#define	A1CR_CLKMODE_x32	0x80
#define	A1CR_CLKMODE_x64	0xc0


/* WR5: TX_CTRL_REG "TCR" */

#define	TCR_TX_CRC_ENAB		0x01
#define	TCR_RTS			0x02
#define	TCR_USE_CRC_CCITT	0x00
#define	TCR_USE_CRC_16		0x04
#define	TCR_TX_ENAB		0x08
#define	TCR_SEND_BREAK		0x10

#define	TCR_CHSIZE_MASK		0x60
#define	TCR_CHSIZE_5		0x00
#define	TCR_CHSIZE_6		0x20
#define	TCR_CHSIZE_7		0x40
#define	TCR_CHSIZE_8		0x60

#define	TCR_DTR			0x80


/* WR7': SLDC_OPTION_REG "SOR" */

#define	SOR_AUTO_TX_ENAB	0x01
#define	SOR_AUTO_EOM_RESET	0x02
#define	SOR_AUTO_RTS_MODE	0x04
#define	SOR_NRZI_DISAB_HIGH	0x08
#define	SOR_ALT_DTRREQ_TIMING	0x10
#define	SOR_READ_CRC_CHARS	0x20
#define	SOR_EXTENDED_REG_ACCESS	0x40


/* WR9: MASTER_INT_CTRL "MIC" */

#define	MIC_VEC_INCL_STAT	0x01
#define	MIC_NO_VECTOR		0x02
#define	MIC_DISAB_LOWER_CHAIN	0x04
#define	MIC_MASTER_INT_ENAB	0x08
#define	MIC_STATUS_HIGH		0x10
#define	MIC_IGN_INTACK		0x20

#define	MIC_NO_RESET		0x00
#define	MIC_CH_A_RESET		0x40
#define	MIC_CH_B_RESET		0x80
#define	MIC_HARD_RESET		0xc0


/* WR10: AUX2_CTRL_REG "A2CR" */

#define	A2CR_SYNC_6		0x01
#define	A2CR_LOOP_MODE		0x02
#define	A2CR_ABORT_ON_UNDERRUN	0x04
#define	A2CR_MARK_IDLE		0x08
#define	A2CR_GO_ACTIVE_ON_POLL	0x10

#define	A2CR_CODING_MASK	0x60
#define	A2CR_CODING_NRZ		0x00
#define	A2CR_CODING_NRZI	0x20
#define	A2CR_CODING_FM1		0x40
#define	A2CR_CODING_FM0		0x60

#define	A2CR_PRESET_CRC_1	0x80


/* WR11: CLK_CTRL_REG "CCR" */

#define	CCR_TRxCOUT_MASK	0x03
#define	CCR_TRxCOUT_XTAL	0x00
#define	CCR_TRxCOUT_TXCLK	0x01
#define	CCR_TRxCOUT_BRG		0x02
#define	CCR_TRxCOUT_DPLL	0x03

#define	CCR_TRxC_OUTPUT		0x04

#define	CCR_TXCLK_MASK		0x18
#define	CCR_TXCLK_RTxC		0x00
#define	CCR_TXCLK_TRxC		0x08
#define	CCR_TXCLK_BRG		0x10
#define	CCR_TXCLK_DPLL		0x18

#define	CCR_RXCLK_MASK		0x60
#define	CCR_RXCLK_RTxC		0x00
#define	CCR_RXCLK_TRxC		0x20
#define	CCR_RXCLK_BRG		0x40
#define	CCR_RXCLK_DPLL		0x60

#define	CCR_RTxC_XTAL		0x80


/* WR14: DPLL_CTRL_REG "DCR" */

#define	DCR_BRG_ENAB		0x01
#define	DCR_BRG_USE_PCLK	0x02
#define	DCR_DTRREQ_IS_REQ	0x04
#define	DCR_AUTO_ECHO		0x08
#define	DCR_LOCAL_LOOPBACK	0x10

#define	DCR_DPLL_EDGE_SEARCH	0x20
#define	DCR_DPLL_ERR_RESET	0x40
#define	DCR_DPLL_DISAB		0x60
#define	DCR_DPLL_CLK_BRG	0x80
#define	DCR_DPLL_CLK_RTxC	0xa0
#define	DCR_DPLL_FM		0xc0
#define	DCR_DPLL_NRZI		0xe0


/* WR15: INT_CTRL_REG "ICR" */

#define	ICR_OPTIONREG_SELECT	0x01
#define	ICR_ENAB_BRG_ZERO_INT	0x02
#define	ICR_USE_FS_FIFO		0x04
#define	ICR_ENAB_DCD_INT	0x08
#define	ICR_ENAB_SYNC_INT	0x10
#define	ICR_ENAB_CTS_INT	0x20
#define	ICR_ENAB_UNDERRUN_INT	0x40
#define	ICR_ENAB_BREAK_INT	0x80


/* RR0: STATUS_REG "SR" */

#define	SR_CHAR_AVAIL		0x01
#define	SR_BRG_ZERO		0x02
#define	SR_TX_BUF_EMPTY		0x04
#define	SR_DCD			0x08
#define	SR_SYNC_ABORT		0x10
#define	SR_CTS			0x20
#define	SR_TX_UNDERRUN		0x40
#define	SR_BREAK		0x80


/* RR1: SPCOND_STATUS_REG "SCSR" */

#define	SCSR_ALL_SENT		0x01
#define	SCSR_RESIDUAL_MASK	0x0e
#define	SCSR_PARITY_ERR		0x10
#define	SCSR_RX_OVERRUN		0x20
#define	SCSR_CRC_FRAME_ERR	0x40
#define	SCSR_END_OF_FRAME	0x80


/* RR3: INT_PENDING_REG "IPR" */

#define	IPR_B_EXTSTAT		0x01
#define	IPR_B_TX		0x02
#define	IPR_B_RX		0x04
#define	IPR_A_EXTSTAT		0x08
#define	IPR_A_TX		0x10
#define	IPR_A_RX		0x20


/* RR7: FS_FIFO_HIGH_REG "FFHR" */

#define	FFHR_CNT_MASK		0x3f
#define	FFHR_IS_FROM_FIFO	0x40
#define	FFHR_FIFO_OVERRUN	0x80


/* RR10: DPLL_STATUS_REG "DSR" */

#define	DSR_ON_LOOP		0x02
#define	DSR_ON_LOOP_SENDING	0x10
#define	DSR_TWO_CLK_MISSING	0x40
#define	DSR_ONE_CLK_MISSING	0x80

/***********************************************************************/
/*                                                                     */
/*                             Register Access                         */
/*                                                                     */
/***********************************************************************/

#define SCC_ACCESS_INIT(port)						\
	unsigned char *_scc_shadow = &scc_shadow[port->channel][0]

#define	SCCwrite(reg,val)	_SCCwrite(port,_scc_shadow,(reg),(val),1)
#define	SCCwrite_NB(reg,val)	_SCCwrite(port,_scc_shadow,(reg),(val),0)
#define	SCCread(reg)		_SCCread(port,_scc_shadow,(reg),1)
#define	SCCread_NB(reg)		_SCCread(port,_scc_shadow,(reg),0)

#define SCCmod(reg,and,or)	SCCwrite((reg),(SCCread(reg)&(and))|(or))

/* Shadows for all SCC write registers */
static unsigned char scc_shadow[2][16];

/* To keep track of STATUS_REG state for detection of Ext/Status int source */
static unsigned char scc_last_status_reg[2];

/* The SCC needs 3.5 PCLK cycles recovery time between to register
 * accesses. PCLK runs with 8 MHz on an Atari, so this delay is 3.5 *
 * 125 ns = 437.5 ns. This is too short for udelay().
 * 10/16/95: A tstb st_mfp.par_dt_reg takes 600ns (sure?) and thus should be
 * quite right
 */

#define scc_reg_delay()				\
    do {					\
	    __asm__ __volatile__ ( " nop; nop");\
    } while (0)

/* The following functions should relax the somehow complicated
 * register access of the SCC. _SCCwrite() stores all written values
 * (except for WR0 and WR8) in shadow registers for later recall. This
 * removes the burden of remembering written values as needed. The
 * extra work of storing the value doesn't count, since a delay is
 * needed after a SCC access anyway. Additionally, _SCCwrite() manages
 * writes to WR0 and WR8 differently, because these can be accessed
 * directly with less overhead. Another special case are WR7 and WR7'.
 * _SCCwrite automatically checks what of this registers is selected
 * and changes b0 of WR15 if needed.
 *
 * _SCCread() for standard read registers is straightforward, except
 * for RR2 (split into two "virtual" registers: one for the value
 * written to WR2 (from the shadow) and one for the vector including
 * status from RR2, Ch. B) and RR3. The latter must be read from
 * Channel A, because it reads as all zeros on Ch. B. RR0 and RR8 can
 * be accessed directly as before.
 *
 * The two inline function contain complicated switch statements. But
 * I rely on regno and final_delay being constants, so gcc can reduce
 * the whole stuff to just some assembler statements.
 *
 * _SCCwrite and _SCCread aren't intended to be used directly under
 * normal circumstances. The macros SCCread[_ND] and SCCwrite[_ND] are
 * for that purpose. They assume that a local variable 'port' is
 * declared and pointing to the port's scc_struct entry. The
 * variants with "_NB" appended should be used if no other SCC
 * accesses follow immediately (within 0.5 usecs). They just skip the
 * final delay nops.
 *
 * Please note that accesses to SCC registers should only take place
 * when interrupts are turned off (at least if SCC interrupts are
 * enabled). Otherwise, an interrupt could interfere with the
 * two-stage accessing process.
 *
 */
static void _SCCwrite(
	struct scc_port *port,
	unsigned char *shadow,
	int regno,
	unsigned char val, int final_delay )
{
	switch( regno ) {

	  case COMMAND_REG:
		/* WR0 can be written directly without pointing */
		*port->ctrlp = val;
		break;

	  case SYNC_CHAR_REG:
		/* For WR7, first set b0 of WR15 to 0, if needed */
		if (shadow[INT_CTRL_REG] & ICR_OPTIONREG_SELECT) {
			*port->ctrlp = 15;
			shadow[INT_CTRL_REG] &= ~ICR_OPTIONREG_SELECT;
			scc_reg_delay();
			*port->ctrlp = shadow[INT_CTRL_REG];
			scc_reg_delay();
		}
		goto normal_case;

	  case SDLC_OPTION_REG:
		/* For WR7', first set b0 of WR15 to 1, if needed */
		if (!(shadow[INT_CTRL_REG] & ICR_OPTIONREG_SELECT)) {
			*port->ctrlp = 15;
			shadow[INT_CTRL_REG] |= ICR_OPTIONREG_SELECT;
			scc_reg_delay();
			*port->ctrlp = shadow[INT_CTRL_REG];
			scc_reg_delay();
		}
		*port->ctrlp = 7;
		shadow[8] = val;	/* WR7' shadowed at WR8 */
		scc_reg_delay();
		*port->ctrlp = val;
		break;

	  case TX_DATA_REG:		/* WR8 */
		*port->ctrlp = regno;
		scc_reg_delay();
		*port->ctrlp = val;
		break;

	  case MASTER_INT_CTRL:
		*port->ctrlp = regno;
		val &= 0x3f;	/* bits 6..7 are the reset commands */
		scc_shadow[0][regno] = val;
		scc_reg_delay();
		*port->ctrlp = val;
		break;

	  case DPLL_CTRL_REG:
		*port->ctrlp = regno;
		val &= 0x1f;			/* bits 5..7 are the DPLL commands */
		shadow[regno] = val;
		scc_reg_delay();
		*port->ctrlp = val;
		break;

	  case 1 ... 6:
	  case 10 ... 13:
	  case 15:
	  normal_case:
		*port->ctrlp = regno;
		shadow[regno] = val;
		scc_reg_delay();
		*port->ctrlp = val;
		break;

	  default:
		printk( "Bad SCC write access to WR%d\n", regno );
		break;

	}

	if (final_delay)
		scc_reg_delay();
}

static unsigned char _SCCread(
	struct scc_port *port,
	unsigned char *shadow,
	int regno, int final_delay )
{
	unsigned char rv;

	switch( regno ) {

		/* --- real read registers --- */
	  case STATUS_REG:
		rv = *port->ctrlp;
		break;

	  case INT_PENDING_REG:
		/* RR3: read only from Channel A! */
		port = port->port_a;
		goto normal_case;

	  case RX_DATA_REG:
		*port->ctrlp = 8;
		scc_reg_delay();
		rv = *port->ctrlp;
		break;

	  case CURR_VECTOR_REG:
		/* RR2 (vector including status) from Ch. B */
		port = port->port_b;
		goto normal_case;

		/* --- reading write registers: access the shadow --- */
	  case 1 ... 7:
	  case 10 ... 15:
		return shadow[regno]; /* no final delay! */

		/* WR7' is special, because it is shadowed at the place of WR8 */
	  case SDLC_OPTION_REG:
		return shadow[8]; /* no final delay! */

		/* WR9 is special too, because it is common for both channels */
	  case MASTER_INT_CTRL:
		return scc_shadow[0][9]; /* no final delay! */

	  default:
		printk( "Bad SCC read access to %cR%d\n", (regno & 16) ? 'R' : 'W',
				regno & ~16 );
		break;

	  case SPCOND_STATUS_REG:
	  case FS_FIFO_LOW_REG:
	  case FS_FIFO_HIGH_REG:
	  case DPLL_STATUS_REG:
	  normal_case:
		*port->ctrlp = regno & 0x0f;
		scc_reg_delay();
		rv = *port->ctrlp;
		break;

	}

	if (final_delay)
		scc_reg_delay();
	return rv;
}

struct mvme147_scc_serial_port {
	struct device *dev;
	struct uart_port port;
	struct scc_port scc_port;
};

struct mvme147_scc_serial_port ports[MVME147_SCC_SERIAL_MAX_PORTS];
struct mvme147_scc_serial_port *console_ports[MVME147_SCC_SERIAL_MAX_PORTS] = { 0 };

static irqreturn_t scc_rx_int(int irq, void *data)
{
	struct mvme147_scc_serial_port *sccp = data;
	struct scc_port *port = &sccp->scc_port;
	SCC_ACCESS_INIT(port);

	unsigned char	ch;

	ch = SCCread_NB(RX_DATA_REG);
	uart_insert_char(&sccp->port, 0, 0, ch, TTY_NORMAL);
	sccp->port.icount.rx++;
	tty_flip_buffer_push(&sccp->port.state->port);
	SCCwrite_NB(COMMAND_REG, CR_HIGHEST_IUS_RESET);

	return IRQ_HANDLED;
#if 0

	if (!tty) {
		printk(KERN_WARNING "scc_rx_int with NULL tty!\n");
		SCCwrite_NB(COMMAND_REG, CR_HIGHEST_IUS_RESET);
		return IRQ_HANDLED;
	}
	tty_insert_flip_char(tty->port, ch, 0);

	/* Check if another character is already ready; in that case, the
	 * spcond_int() function must be used, because this character may have an
	 * error condition that isn't signalled by the interrupt vector used!
	 */
	if (SCCread(INT_PENDING_REG) &
	    (port->channel == CHANNEL_A ? IPR_A_RX : IPR_B_RX)) {
		scc_spcond_int (irq, data);
		return IRQ_HANDLED;
	}

#endif
}

static irqreturn_t scc_spcond_int(int irq, void *data)
{
	printk("%s:%d\n", __func__, __LINE__);

	struct mvme147_scc_serial_port *sccp = data;
	struct scc_port *port = &sccp->scc_port;
	SCC_ACCESS_INIT(port);

	SCCwrite(COMMAND_REG, CR_ERROR_RESET);
	SCCwrite_NB(COMMAND_REG, CR_HIGHEST_IUS_RESET);

#if 0
	struct scc_port *port = data;
	struct tty_struct *tty = port->gs.port.tty;
	unsigned char	stat, ch, err;
	int		int_pending_mask = port->channel == CHANNEL_A ?
			                   IPR_A_RX : IPR_B_RX;
	SCC_ACCESS_INIT(port);

	if (!tty) {
		printk(KERN_WARNING "scc_spcond_int with NULL tty!\n");
		return IRQ_HANDLED;
	}
	do {
		stat = SCCread(SPCOND_STATUS_REG);
		ch = SCCread_NB(RX_DATA_REG);

		if (stat & SCSR_RX_OVERRUN)
			err = TTY_OVERRUN;
		else if (stat & SCSR_PARITY_ERR)
			err = TTY_PARITY;
		else if (stat & SCSR_CRC_FRAME_ERR)
			err = TTY_FRAME;
		else
			err = 0;

		tty_insert_flip_char(tty->port, ch, err);

		/* ++TeSche: *All* errors have to be cleared manually,
		 * else the condition persists for the next chars
		 */
		if (err)
		  SCCwrite(COMMAND_REG, CR_ERROR_RESET);

	} while(SCCread(INT_PENDING_REG) & int_pending_mask);

	SCCwrite_NB(COMMAND_REG, CR_HIGHEST_IUS_RESET);

	tty_flip_buffer_push(tty->port);
#endif
	return IRQ_HANDLED;
}

static bool mvme147_scc_serial_can_transmit(struct mvme147_scc_serial_port *sccp)
{

	struct scc_port *port = &sccp->scc_port;
	SCC_ACCESS_INIT(port);

	if (SCCread_NB(STATUS_REG) & SR_TX_BUF_EMPTY)
		return true;

	return false;
}

static void scc_ch_write(struct mvme147_scc_serial_port *sccp, char ch)
{
	struct scc_port *port = &sccp->scc_port;
	SCC_ACCESS_INIT(port);

	SCCwrite(TX_DATA_REG, ch);
}

static void mvme147_scc_serial_transmit_char(struct mvme147_scc_serial_port *sccp, u8 ch)
{
	scc_ch_write(sccp, ch);
}

static irqreturn_t scc_tx_int(int irq, void *data)
{
	struct mvme147_scc_serial_port *sccp = data;
	struct scc_port *port = &sccp->scc_port;
	SCC_ACCESS_INIT(port);
	u8 ch;

	uart_port_lock(&sccp->port);
	uart_port_tx_limited(&sccp->port, ch, MVME147_SCC_TX_FIFO_DEPTH,
			     mvme147_scc_serial_can_transmit(sccp),
			     mvme147_scc_serial_transmit_char(sccp, ch),
			     ({}));

	//SCCmod (INT_AND_DMA_REG, ~IDR_TX_INT_ENAB, 0);
	SCCwrite(COMMAND_REG, CR_TX_PENDING_RESET);
	SCCwrite_NB(COMMAND_REG, CR_HIGHEST_IUS_RESET);

	uart_port_unlock(&sccp->port);

	return IRQ_HANDLED;
}

static irqreturn_t scc_stat_int(int irq, void *data)
{
	struct mvme147_scc_serial_port *sccp = data;
	struct scc_port *port = &sccp->scc_port;
	SCC_ACCESS_INIT(port);

	#if 0
	struct scc_port *port = data;
	unsigned channel = port->channel;
	unsigned char	last_sr, sr, changed;


	last_sr = scc_last_status_reg[channel];
	sr = scc_last_status_reg[channel] = SCCread_NB(STATUS_REG);
	changed = last_sr ^ sr;

	if (changed & SR_DCD) {
		port->c_dcd = !!(sr & SR_DCD);
		if (!(tty_port_check_carrier(&port->gs.port)))
			;	/* Don't report DCD changes */
		else if (port->c_dcd) {
			wake_up_interruptible(&port->gs.port.open_wait);
		}
		else {
			if (port->gs.port.tty)
				tty_hangup(port->gs.port.tty);
		}
	}
#endif

	SCCwrite(COMMAND_REG, CR_EXTSTAT_RESET);
	SCCwrite_NB(COMMAND_REG, CR_HIGHEST_IUS_RESET);

	return IRQ_HANDLED;
}

static int __init mvme147_scc_request_irqs(struct platform_device *pdev,
					   struct mvme147_scc_serial_port* sccp_a,
					   struct mvme147_scc_serial_port* sccp_b)
{
	struct device *dev = &pdev->dev;
	int error;

	int irq_a_tx, irq_a_stat, irq_a_rx, irq_a_spcond;
	int irq_b_tx, irq_b_stat, irq_b_rx, irq_b_spcond;

	irq_a_tx = platform_get_irq(pdev, 0);
	irq_a_stat = platform_get_irq(pdev, 1);
	irq_a_rx = platform_get_irq(pdev, 2);
	irq_a_spcond = platform_get_irq(pdev, 3);

	irq_b_tx = platform_get_irq(pdev, 4);
	irq_b_stat = platform_get_irq(pdev, 5);
	irq_b_rx = platform_get_irq(pdev, 6);
	irq_b_spcond = platform_get_irq(pdev, 7);

	error = devm_request_irq(dev, irq_a_tx, scc_tx_int, 0,
				 "SCC-A TX", sccp_a);
	if (error)
		return error;

	error = devm_request_irq(dev, irq_a_stat, scc_stat_int, 0,
				 "SCC-A status", sccp_a);
	if (error)
		return error;

	error = devm_request_irq(dev, irq_a_rx, scc_rx_int, 0,
				 "SCC-A RX", sccp_a);
	if (error)
		return error;

	error = devm_request_irq(dev, irq_a_spcond, scc_spcond_int,
				 0, "SCC-A special cond", sccp_a);
	if (error)
		return error;

	error = devm_request_irq(dev, irq_b_tx, scc_tx_int, 0,
				 "SCC-B TX", sccp_b);
	if (error)
		return error;

	error = devm_request_irq(dev, irq_b_stat, scc_stat_int, 0,
				 "SCC-B status", sccp_b);
	if (error)
		return error;

	error = devm_request_irq(dev, irq_b_rx, scc_rx_int, 0,
				 "SCC-B RX", sccp_b);
	if (error)
		return error;

	error = devm_request_irq(dev, irq_b_spcond, scc_spcond_int,
				 0, "SCC-B special cond", sccp_b);
	if (error)
		return error;

	return 0;
}

static int mvme147_scc_init(struct platform_device *pdev)
{
	struct mvme147_scc_serial_port* sccp;
	struct device *dev = &pdev->dev;
	struct scc_port *port;
	int error;
	u32 vector;

	pr_info("SCC: MVME147 Serial Driver\n");

	error = device_property_read_u32(dev, "scc,vector", &vector);
	if (error)
		return error;

	/* Init channel A */
	sccp = &ports[0];
	port = &ports[0].scc_port;
	port->channel = CHANNEL_A;
	port->ctrlp = (volatile unsigned char *)M147_SCC_A_ADDR;
	port->datap = port->ctrlp + 1;
	port->port_a = &ports[0].scc_port;
	port->port_b = &ports[1].scc_port;


	{
		SCC_ACCESS_INIT(port);

		/* disable interrupts for this channel */
		SCCwrite(INT_AND_DMA_REG, 0);
		/* Set the interrupt vector */
		SCCwrite(INT_VECTOR_REG, vector);
		/* Interrupt parameters: vector includes status, status low */
		SCCwrite(MASTER_INT_CTRL, MIC_VEC_INCL_STAT);
		SCCmod(MASTER_INT_CTRL, 0xff, MIC_MASTER_INT_ENAB);
	}

	/* Init channel B */
	sccp = &ports[1];
	port = &ports[1].scc_port;
	port->channel = CHANNEL_B;
	port->ctrlp = (volatile unsigned char *)M147_SCC_B_ADDR;
	port->datap = port->ctrlp + 1;
	port->port_a = &ports[0].scc_port;
	port->port_b = &ports[1].scc_port;


	{
		SCC_ACCESS_INIT(port);

		/* disable interrupts for this channel */
		SCCwrite(INT_AND_DMA_REG, 0);
	}

	/* Ensure interrupts are enabled in the PCC chip */
        m147_pcc->serial_cntrl=PCC_LEVEL_SERIAL|PCC_INT_ENAB;
	dev_info(sccp->dev, "PCC serial control register: 0x%02x\n", (unsigned int) m147_pcc->serial_cntrl);

	error = mvme147_scc_request_irqs(pdev, &ports[0], &ports[1]);
	if (error)
		return error;

	return 0;
}

static int __init mvme162_scc_init(void)
{
	struct mvme147_scc_serial_port* sccp;
	struct scc_port *port;
	int error;

	if (!(mvme16x_config & MVME16x_CONFIG_GOT_SCCA))
		return (-ENODEV);

	printk(KERN_INFO "SCC: MVME162 Serial Driver\n");
	/* Init channel A */
	sccp = &ports[0];
	port = &ports->scc_port;
	port->channel = CHANNEL_A;
	port->ctrlp = (volatile unsigned char *)MVME_SCC_A_ADDR;
	port->datap = port->ctrlp + 2;
	port->port_a = &ports[0].scc_port;
	port->port_b = &ports[1].scc_port;
	error = request_irq(MVME162_IRQ_SCCA_TX, scc_tx_int, 0,
		            "SCC-A TX", port);
	if (error)
		goto fail;
	error = request_irq(MVME162_IRQ_SCCA_STAT, scc_stat_int, 0,
		            "SCC-A status", port);
	if (error)
		goto fail_free_a_tx;
	error = request_irq(MVME162_IRQ_SCCA_RX, scc_rx_int, 0,
		            "SCC-A RX", port);
	if (error)
		goto fail_free_a_stat;
	error = request_irq(MVME162_IRQ_SCCA_SPCOND, scc_spcond_int,
			    0, "SCC-A special cond", port);
	if (error)
		goto fail_free_a_rx;

	{
		SCC_ACCESS_INIT(port);

		/* disable interrupts for this channel */
		SCCwrite(INT_AND_DMA_REG, 0);
		/* Set the interrupt vector */
		SCCwrite(INT_VECTOR_REG, MVME162_IRQ_SCC_BASE);
		/* Interrupt parameters: vector includes status, status low */
		SCCwrite(MASTER_INT_CTRL, MIC_VEC_INCL_STAT);
		SCCmod(MASTER_INT_CTRL, 0xff, MIC_MASTER_INT_ENAB);
	}

	/* Init channel B */
	sccp = &ports[1];
	port = &ports->scc_port;
	port->channel = CHANNEL_B;
	port->ctrlp = (volatile unsigned char *)MVME_SCC_B_ADDR;
	port->datap = port->ctrlp + 2;
	port->port_a = &ports[0].scc_port;
	port->port_b = &ports[1].scc_port;
	error = request_irq(MVME162_IRQ_SCCB_TX, scc_tx_int, 0,
		            "SCC-B TX", port);
	if (error)
		goto fail_free_a_spcond;
	error = request_irq(MVME162_IRQ_SCCB_STAT, scc_stat_int, 0,
		            "SCC-B status", port);
	if (error)
		goto fail_free_b_tx;
	error = request_irq(MVME162_IRQ_SCCB_RX, scc_rx_int, 0,
		            "SCC-B RX", port);
	if (error)
		goto fail_free_b_stat;
	error = request_irq(MVME162_IRQ_SCCB_SPCOND, scc_spcond_int,
			    0, "SCC-B special cond", port);
	if (error)
		goto fail_free_b_rx;

	{
		SCC_ACCESS_INIT(port);	/* Either channel will do */

		/* disable interrupts for this channel */
		SCCwrite(INT_AND_DMA_REG, 0);
	}

        /* Ensure interrupts are enabled in the MC2 chip */
        *(volatile char *)0xfff4201d = 0x14;

	return 0;

fail_free_b_rx:
	free_irq(MVME162_IRQ_SCCB_RX, port);
fail_free_b_stat:
	free_irq(MVME162_IRQ_SCCB_STAT, port);
fail_free_b_tx:
	free_irq(MVME162_IRQ_SCCB_TX, port);
fail_free_a_spcond:
	free_irq(MVME162_IRQ_SCCA_SPCOND, port);
fail_free_a_rx:
	free_irq(MVME162_IRQ_SCCA_RX, port);
fail_free_a_stat:
	free_irq(MVME162_IRQ_SCCA_STAT, port);
fail_free_a_tx:
	free_irq(MVME162_IRQ_SCCA_TX, port);
fail:
	return error;
}

static int __init bvme6000_scc_init(void)
{
	struct mvme147_scc_serial_port* sccp;
	struct scc_port *port;
	int error;

	printk(KERN_INFO "SCC: BVME6000 Serial Driver\n");
	/* Init channel A */
	sccp = &ports[0];
	port = &ports->scc_port;
	port->channel = CHANNEL_A;
	port->ctrlp = (volatile unsigned char *)BVME_SCC_A_ADDR;
	port->datap = port->ctrlp + 4;
	port->port_a = &ports[0].scc_port;
	port->port_b = &ports[1].scc_port;
	error = request_irq(BVME_IRQ_SCCA_TX, scc_tx_int, 0,
		            "SCC-A TX", port);
	if (error)
		goto fail;
	error = request_irq(BVME_IRQ_SCCA_STAT, scc_stat_int, 0,
		            "SCC-A status", port);
	if (error)
		goto fail_free_a_tx;
	error = request_irq(BVME_IRQ_SCCA_RX, scc_rx_int, 0,
		            "SCC-A RX", port);
	if (error)
		goto fail_free_a_stat;
	error = request_irq(BVME_IRQ_SCCA_SPCOND, scc_spcond_int,
			    0, "SCC-A special cond", port);
	if (error)
		goto fail_free_a_rx;

	{
		SCC_ACCESS_INIT(port);

		/* disable interrupts for this channel */
		SCCwrite(INT_AND_DMA_REG, 0);
		/* Set the interrupt vector */
		SCCwrite(INT_VECTOR_REG, BVME_IRQ_SCC_BASE);
		/* Interrupt parameters: vector includes status, status low */
		SCCwrite(MASTER_INT_CTRL, MIC_VEC_INCL_STAT);
		SCCmod(MASTER_INT_CTRL, 0xff, MIC_MASTER_INT_ENAB);
	}

	/* Init channel B */
	sccp = &ports[1];
	port = &ports->scc_port;
	port->channel = CHANNEL_B;
	port->ctrlp = (volatile unsigned char *)BVME_SCC_B_ADDR;
	port->datap = port->ctrlp + 4;
	port->port_a = &ports[0].scc_port;
	port->port_b = &ports[1].scc_port;
	error = request_irq(BVME_IRQ_SCCB_TX, scc_tx_int, 0,
		            "SCC-B TX", port);
	if (error)
		goto fail_free_a_spcond;
	error = request_irq(BVME_IRQ_SCCB_STAT, scc_stat_int, 0,
		            "SCC-B status", port);
	if (error)
		goto fail_free_b_tx;
	error = request_irq(BVME_IRQ_SCCB_RX, scc_rx_int, 0,
		            "SCC-B RX", port);
	if (error)
		goto fail_free_b_stat;
	error = request_irq(BVME_IRQ_SCCB_SPCOND, scc_spcond_int,
			    0, "SCC-B special cond", port);
	if (error)
		goto fail_free_b_rx;

	{
		SCC_ACCESS_INIT(port);	/* Either channel will do */

		/* disable interrupts for this channel */
		SCCwrite(INT_AND_DMA_REG, 0);
	}

	return 0;

fail:
	free_irq(BVME_IRQ_SCCA_STAT, port);
fail_free_a_tx:
	free_irq(BVME_IRQ_SCCA_RX, port);
fail_free_a_stat:
	free_irq(BVME_IRQ_SCCA_SPCOND, port);
fail_free_a_rx:
	free_irq(BVME_IRQ_SCCB_TX, port);
fail_free_a_spcond:
	free_irq(BVME_IRQ_SCCB_STAT, port);
fail_free_b_tx:
	free_irq(BVME_IRQ_SCCB_RX, port);
fail_free_b_stat:
	free_irq(BVME_IRQ_SCCB_SPCOND, port);
fail_free_b_rx:
	return error;
}

#define port_to_mvme147_scc_serial_port(p) (container_of((p), \
						    struct mvme147_scc_serial_port, \
						    port))

/*
 * Linux serial API functions
 */
static void scc_enable_tx_interrupts(struct mvme147_scc_serial_port *sccp)
{
	struct scc_port *port = &sccp->scc_port;
	unsigned long flags;
	SCC_ACCESS_INIT(port);

	local_irq_save(flags);
	SCCmod(INT_AND_DMA_REG, 0xff, IDR_TX_INT_ENAB);
	/* restart the transmitter */
	scc_tx_int(0, sccp);
	local_irq_restore(flags);
}

static void scc_disable_tx_interrupts(struct mvme147_scc_serial_port *sccp)
{
	struct scc_port *port = &sccp->scc_port;
	unsigned long flags;
	SCC_ACCESS_INIT(port);

	local_irq_save(flags);
	SCCmod(INT_AND_DMA_REG, ~IDR_TX_INT_ENAB, 0);
	local_irq_restore(flags);
}

static void mvme147_scc_serial_start_tx(struct uart_port *p)
{
	struct mvme147_scc_serial_port *sccp = port_to_mvme147_scc_serial_port(p);
	struct scc_port *port = &sccp->scc_port;
	SCC_ACCESS_INIT(port);
	unsigned int pending;
	u8 ch;

	uart_port_lock(&sccp->port);
	pending = uart_port_tx_limited(&sccp->port, ch, MVME147_SCC_TX_FIFO_DEPTH,
			     mvme147_scc_serial_can_transmit(sccp),
			     mvme147_scc_serial_transmit_char(sccp, ch),
			     ({}));
	uart_port_unlock(&sccp->port);

	if (pending)
		scc_enable_tx_interrupts(sccp);
}

static void mvme147_scc_serial_stop_tx(struct uart_port *port)
{
	struct mvme147_scc_serial_port *sccp = port_to_mvme147_scc_serial_port(port);

	//scc_disable_tx_interrupts(sccp);
}

static void scc_enable_rx_interrupts(struct mvme147_scc_serial_port *sccp)
{
	struct scc_port *port = &sccp->scc_port;
	unsigned long	flags;
	SCC_ACCESS_INIT(port);

	local_irq_save(flags);
	SCCmod(INT_AND_DMA_REG, 0xff,
		IDR_EXTSTAT_INT_ENAB|IDR_PARERR_AS_SPCOND|IDR_RX_INT_ALL);
	local_irq_restore(flags);
}

static void scc_disable_rx_interrupts(struct mvme147_scc_serial_port *sccp)
{
	struct scc_port *port = &sccp->scc_port;
	unsigned long	flags;
	SCC_ACCESS_INIT(port);

	local_irq_save(flags);
	SCCmod(INT_AND_DMA_REG,
	    ~(IDR_RX_INT_MASK|IDR_PARERR_AS_SPCOND|IDR_EXTSTAT_INT_ENAB), 0);
	local_irq_restore(flags);
}

static void mvme147_scc_serial_stop_rx(struct uart_port *port)
{
	struct mvme147_scc_serial_port *sccp = port_to_mvme147_scc_serial_port(port);

	scc_disable_rx_interrupts(sccp);
}

static unsigned int mvme147_scc_serial_tx_empty(struct uart_port *port)
{
	//struct mvme147_scc_serial_port *ssp = port_to_mvme147_scc_serial_port(port);

	return TIOCSER_TEMT;
}

static unsigned int mvme147_scc_serial_get_mctrl(struct uart_port *port)
{
	return TIOCM_CAR | TIOCM_CTS | TIOCM_DSR;
}

static void mvme147_scc_serial_set_mctrl(struct uart_port *port, unsigned int mctrl)
{

}

static void mvme147_scc_serial_break_ctl(struct uart_port *port, int break_state)
{

}

static int mvme147_scc_serial_startup(struct uart_port *p)
{
	struct mvme147_scc_serial_port *sccp = port_to_mvme147_scc_serial_port(p);
	struct scc_port *port = &sccp->scc_port;

	printk("%s:%d\n", __func__, __LINE__);

	SCC_ACCESS_INIT(port);
	static const struct {
		unsigned reg, val;
	} mvme_init_tab[] = {
		/* Values for MVME162 and MVME147 */
		/* no parity, 1 stop bit, async, 1:16 */
		{ AUX1_CTRL_REG, A1CR_PARITY_NONE|A1CR_MODE_ASYNC_1|A1CR_CLKMODE_x16 },
		/* parity error is special cond, ints disabled, no DMA */
		{ INT_AND_DMA_REG, IDR_PARERR_AS_SPCOND | IDR_RX_INT_DISAB },
		/* Rx 8 bits/char, no auto enable, Rx off */
		{ RX_CTRL_REG, RCR_CHSIZE_8 },
		/* DTR off, Tx 8 bits/char, RTS off, Tx off */
		{ TX_CTRL_REG, TCR_CHSIZE_8 },
		/* special features off */
		{ AUX2_CTRL_REG, 0 },
		{ CLK_CTRL_REG, CCR_RXCLK_BRG | CCR_TXCLK_BRG },
		{ DPLL_CTRL_REG, DCR_BRG_ENAB | DCR_BRG_USE_PCLK },
		/* Start Rx */
		{ RX_CTRL_REG, RCR_RX_ENAB | RCR_CHSIZE_8 },
		/* Start Tx */
		{ TX_CTRL_REG, TCR_TX_ENAB | TCR_RTS | TCR_DTR | TCR_CHSIZE_8 },
		/* Ext/Stat ints: DCD only */
		{ INT_CTRL_REG, ICR_ENAB_DCD_INT },
		/* Reset Ext/Stat ints */
		{ COMMAND_REG, CR_EXTSTAT_RESET },
		/* ...again */
		{ COMMAND_REG, CR_EXTSTAT_RESET },
	};

	static const struct {
		unsigned reg, val;
	} bvme_init_tab[] = {
		/* Values for BVME6000 */
		/* no parity, 1 stop bit, async, 1:16 */
		{ AUX1_CTRL_REG, A1CR_PARITY_NONE|A1CR_MODE_ASYNC_1|A1CR_CLKMODE_x16 },
		/* parity error is special cond, ints disabled, no DMA */
		{ INT_AND_DMA_REG, IDR_PARERR_AS_SPCOND | IDR_RX_INT_DISAB },
		/* Rx 8 bits/char, no auto enable, Rx off */
		{ RX_CTRL_REG, RCR_CHSIZE_8 },
		/* DTR off, Tx 8 bits/char, RTS off, Tx off */
		{ TX_CTRL_REG, TCR_CHSIZE_8 },
		/* special features off */
		{ AUX2_CTRL_REG, 0 },
		{ CLK_CTRL_REG, CCR_RTxC_XTAL | CCR_RXCLK_BRG | CCR_TXCLK_BRG },
		{ DPLL_CTRL_REG, DCR_BRG_ENAB },
		/* Start Rx */
		{ RX_CTRL_REG, RCR_RX_ENAB | RCR_CHSIZE_8 },
		/* Start Tx */
		{ TX_CTRL_REG, TCR_TX_ENAB | TCR_RTS | TCR_DTR | TCR_CHSIZE_8 },
		/* Ext/Stat ints: DCD only */
		{ INT_CTRL_REG, ICR_ENAB_DCD_INT },
		/* Reset Ext/Stat ints */
		{ COMMAND_REG, CR_EXTSTAT_RESET },
		/* ...again */
		{ COMMAND_REG, CR_EXTSTAT_RESET },
	};

	if (MACH_IS_MVME147 || MACH_IS_MVME16x) {
		for (int i = 0; i < ARRAY_SIZE(mvme_init_tab); ++i)
			SCCwrite(mvme_init_tab[i].reg, mvme_init_tab[i].val);
	}

	else if (MACH_IS_BVME6000) {
		for (int i = 0; i < ARRAY_SIZE(bvme_init_tab); ++i)
			SCCwrite(bvme_init_tab[i].reg, bvme_init_tab[i].val);
	}

#if 0
	/* remember status register for detection of DCD and CTS changes */
	scc_last_status_reg[channel] = SCCread(STATUS_REG);

	port->c_dcd = 0;	/* Prevent initial 1->0 interrupt */
	scc_setsignals (port, 1,1);
#endif

	scc_enable_rx_interrupts(sccp);

	return 0;
}

static void mvme147_scc_serial_shutdown(struct uart_port *port)
{
	//struct mvme147_scc_serial_port *ssp = port_to_mvme147_scc_serial_port(port);
}

static void mvme147_scc_serial_set_termios(struct uart_port *port,
				      struct ktermios *termios,
				      const struct ktermios *old)
{
	//struct mvme147_scc_serial_port *ssp = port_to_mvme147_scc_serial_port(port);
}

static void mvme147_scc_serial_release_port(struct uart_port *port)
{
}

static int mvme147_scc_serial_request_port(struct uart_port *port)
{
	return 0;
}

static void mvme147_scc_serial_config_port(struct uart_port *port, int flags)
{
	//struct mvme147_scc_serial_port *ssp = port_to_mvme147_scc_serial_port(port);

}

static int mvme147_scc_serial_verify_port(struct uart_port *port,
				     struct serial_struct *ser)
{
	return -EINVAL;
}

static const char *mvme147_scc_serial_type(struct uart_port *port)
{
	return NULL;
}

/*
 * Linux console interface
 */
static void mvme147_scc_serial_console_putchar(struct uart_port *port, unsigned char ch)
{
	struct mvme147_scc_serial_port *sccp = port_to_mvme147_scc_serial_port(port);

	while (!mvme147_scc_serial_can_transmit(sccp)) {
	};

	scc_ch_write(sccp, ch);
}

static void mvme147_scc_serial_console_write(struct console *co, const char *s,
					unsigned int count)
{
	struct mvme147_scc_serial_port *sccp = console_ports[co->index];

	if(!sccp)
		return;

	uart_port_lock(&sccp->port);
	uart_console_write(&sccp->port, s, count, mvme147_scc_serial_console_putchar);
	uart_port_unlock(&sccp->port);
}

static int mvme147_scc_serial_console_setup(struct console *co, char *options)
{
	struct mvme147_scc_serial_port *sccp;

	if (co->index < 0 || co->index >= MVME147_SCC_SERIAL_MAX_PORTS)
		return -ENODEV;

	sccp = console_ports[co->index];
        if (!sccp)
                return -ENODEV;

	return 0;
}

static struct uart_driver mvme147_scc_serial_uart_driver;

static struct console mvme147_scc_serial_console = {
	.name	= MVME147_SCC_TTY_PREFIX,
	.write	= mvme147_scc_serial_console_write,
	.device	= uart_console_device,
	.setup	= mvme147_scc_serial_console_setup,
	.flags	= CON_PRINTBUFFER,
	.index	= -1,
	.data	= &mvme147_scc_serial_uart_driver,
};

static int __init mvme147_scc_serial_console_init(void)
{
	if (vme_brdtype == VME_TYPE_MVME147 ||
		vme_brdtype == VME_TYPE_MVME162 ||
		vme_brdtype == VME_TYPE_MVME172 ||
		vme_brdtype == VME_TYPE_BVME4000 ||
		vme_brdtype == VME_TYPE_BVME6000) {
		register_console(&mvme147_scc_serial_console);
	}

	return 0;
}
console_initcall(mvme147_scc_serial_console_init);

#define MVME147_SCC_SERIAL_CONSOLE	(&mvme147_scc_serial_console)

static const struct uart_ops mvme147_scc_serial_uops = {
	.tx_empty	= mvme147_scc_serial_tx_empty,
	.set_mctrl	= mvme147_scc_serial_set_mctrl,
	.get_mctrl	= mvme147_scc_serial_get_mctrl,
	.start_tx	= mvme147_scc_serial_start_tx,
	.stop_tx	= mvme147_scc_serial_stop_tx,
	.stop_rx	= mvme147_scc_serial_stop_rx,
	.break_ctl	= mvme147_scc_serial_break_ctl,
	.startup	= mvme147_scc_serial_startup,
	.shutdown	= mvme147_scc_serial_shutdown,
	.set_termios	= mvme147_scc_serial_set_termios,
	.type		= mvme147_scc_serial_type,
	.release_port	= mvme147_scc_serial_release_port,
	.request_port	= mvme147_scc_serial_request_port,
	.config_port	= mvme147_scc_serial_config_port,
	.verify_port	= mvme147_scc_serial_verify_port,
};

static struct uart_driver mvme147_scc_serial_uart_driver = {
	.owner		= THIS_MODULE,
	.driver_name	= MVME147_SCC_SERIAL_NAME,
	.dev_name	= MVME147_SCC_TTY_PREFIX,
	.nr		= MVME147_SCC_SERIAL_MAX_PORTS,
	.cons		= MVME147_SCC_SERIAL_CONSOLE,
};

#if 0
static int mvme147_scc_serial_probe(struct platform_device *pdev)
{
	struct mvme147_scc_serial_port *ssp;
	int irq, id, r;


	mvme147_scc_serial_add_console_port(ssp);

	r = ;
	if (r != 0) {
		goto probe_out3;
	}

	return 0;

probe_out3:
	mvme147_scc_serial_remove_console_port(ssp);
	free_irq(ssp->port.irq, ssp);
probe_out2:
probe_out1:
	return r;
}
#endif

#if 0
static void mvme147_scc_serial_remove(struct platform_device *dev)
{
	struct mvme147_scc_serial_port *ssp = platform_get_drvdata(dev);

	mvme147_scc_serial_remove_console_port(ssp);
	uart_remove_one_port(&mvme147_scc_serial_uart_driver, &ssp->port);
	free_irq(ssp->port.irq, ssp);
}
#endif

static int mvme147_scc_probe(struct platform_device *pdev)
{
	struct device *dev;
	int res = -ENODEV;
	int i;

	dev = &pdev->dev;
	for (i = 0; i < MVME147_SCC_SERIAL_MAX_PORTS; i++) {
		struct mvme147_scc_serial_port *sccp = &ports[i];
		sccp->dev = dev;
	}

	res = uart_register_driver(&mvme147_scc_serial_uart_driver);
	if (res)
		return res;

	if (MACH_IS_MVME147)
		res = mvme147_scc_init(pdev);
	else if (MACH_IS_MVME16x)
		res = mvme162_scc_init();
	else if (MACH_IS_BVME6000)
		res = bvme6000_scc_init();
	else
		return 0;

	if (res)
		return res;

	for (i = 0; i < MVME147_SCC_SERIAL_MAX_PORTS; i++) {
		struct mvme147_scc_serial_port *sccp = &ports[i];
		struct uart_port *port = &sccp->port;

		port->dev = dev;
		port->type = 1;
		port->iotype = UPIO_MEM;
		port->fifosize = MVME147_SCC_TX_FIFO_DEPTH;
		port->ops = &mvme147_scc_serial_uops;
		port->line = i;
		port->mapbase = (resource_size_t) sccp->scc_port.ctrlp;
		port->membase = sccp->scc_port.ctrlp;

		console_ports[i] = sccp;

		res = uart_add_one_port(&mvme147_scc_serial_uart_driver, port);
		if (res) {
			dev_err(dev, "failed to register port %d: %d\n", i, res);
			break;
		}
	}

	return res;

	//return 0;

//init_out2:
//	uart_unregister_driver(&mvme147_scc_serial_uart_driver);
//init_out1:
//	return r;
}

//static void __exit mvme147_scc_serial_exit(void)
//{
//	uart_unregister_driver(&mvme147_scc_serial_uart_driver);
//}

static const struct of_device_id mvme147_scc_match_table[] = {
	{
		.compatible = "zilog,scc",
	},
	{ }
};
MODULE_DEVICE_TABLE(of, mvme147_scc_match_table);

static struct platform_driver mvme147_scc_driver = {
	.driver = {
		.name           = "mvme147-scc",
		.of_match_table = mvme147_scc_match_table,
	},
	.probe = mvme147_scc_probe,
};
module_platform_driver(mvme147_scc_driver);

MODULE_DESCRIPTION("MVME147 SCC driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Daniel Palmer <daniel@thingy.jp>");
