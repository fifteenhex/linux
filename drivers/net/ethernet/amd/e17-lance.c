// SPDX-License-Identifier: GPL-2.0-only
/*
 * e17-lance.c - On-board Ethernet driver for the ELTEC Eurocom E17
 *               (EUROCOM-17-5xx) VMEbus 68040/68060 SBC.
 *
 * The E17 carries an AMD Am79C900 "ILACC" (Integrated Local Area
 * Communications Controller) - the 32-bit member of the LANCE family - at
 * physical 0xfec68000.  Unlike the classic Am7990 (16-bit descriptors,
 * 24-bit DMA addresses) that drivers/net/ethernet/amd/7990.c supports,
 * the ILACC uses a 32-bit initialisation block and 32-bit (16-byte)
 * descriptors, so this is a self-contained driver rather than a 7990.c
 * platform glue file.
 *
 * Hardware facts (see DESIGN.md for the derivation and citations):
 *
 *  - Register access: the two LANCE registers are 16-bit and sit on the
 *    LOW half of 32-bit bus lanes.  RDP (data)   is at base + 0x2,
 *    RAP (address) is at base + 0x6.  Accesses are big-endian 16-bit
 *    (word) accesses (HW manual, Table 35).
 *
 *  - DMA byte order: the ILACC hangs off the big-endian 68k bus with the
 *    descriptor/init-block bytes reversed within every 32-bit word.  On
 *    real hardware CSR3.ACON + CSR4.BACON(680x0) make the chip present
 *    68000 byte ordering; the QEMU model reproduces the same 4-byte-group
 *    reversal.  The net effect for this big-endian CPU is that full
 *    32-bit fields (buffer addresses, ring pointers) are written as plain
 *    native u32, while fields that pack two 16-bit values into one 32-bit
 *    word must be assembled with the two halves in the (high << 16 | low)
 *    order derived in DESIGN.md.  All packing here follows that rule and
 *    matches the proven u-boot ILACC driver.
 *
 *  - Interrupt: the ILACC IRQ pin feeds VIC068A local input 7 (LIRQ7),
 *    which the VIC raises to the CPU at level 3 and for which the VIC
 *    supplies the vector LIVBR|7 = 0x40|7 = 0x47 (HW manual Table 48).
 *    With no VIC irqchip yet, the DT routes it through the generic
 *    user-vector controller: cell 7 -> CPU vector 0x47 -> Linux IRQ 15.
 *    This driver unmasks/levels VIC LICR7 directly, exactly as the E17
 *    CD2401 console driver drives LICR6.
 *
 *  - Cache coherency: the ILACC is a bus master DMAing to/from cached
 *    68040 DRAM.  The init block, descriptor rings and packet buffers are
 *    allocated with dma_alloc_coherent(), which on m68k/040 maps them
 *    cache-inhibited (_PAGE_NOCACHE_S), so no manual flush/invalidate is
 *    needed and the historical "LANCE DMA corrupts RAM" failures (using
 *    cached DRAM or the aliased DRAM mirror) cannot occur.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>
#include <linux/of.h>
#include <linux/of_net.h>
#include <linux/io.h>
#include <linux/bitops.h>

#include <asm/io.h>

#define DRV_NAME	"e17-lance"

/* --- LANCE CSR0 bits ---------------------------------------------------- */
#define CSR0_INIT	BIT(0)
#define CSR0_STRT	BIT(1)
#define CSR0_STOP	BIT(2)
#define CSR0_TDMD	BIT(3)
#define CSR0_TXON	BIT(4)
#define CSR0_RXON	BIT(5)
#define CSR0_INEA	BIT(6)
#define CSR0_INTR	BIT(7)
#define CSR0_IDON	BIT(8)
#define CSR0_TINT	BIT(9)
#define CSR0_RINT	BIT(10)
#define CSR0_MERR	BIT(11)
#define CSR0_MISS	BIT(12)
#define CSR0_CERR	BIT(13)
#define CSR0_BABL	BIT(14)
#define CSR0_ERR	BIT(15)

/* Interrupt/error bits that are write-1-to-clear in CSR0 */
#define CSR0_ACK	(CSR0_BABL | CSR0_CERR | CSR0_MISS | CSR0_MERR | \
			 CSR0_RINT | CSR0_TINT | CSR0_IDON)

/*
 * Causes the interrupt handler actually acts on and loops to drain.  This
 * deliberately excludes CERR (collision/no-SQE-heartbeat): it is set after
 * every transmit on transceivers without a heartbeat and is purely
 * informational, so it must not keep the service loop spinning.
 */
#define CSR0_WORK	(CSR0_RINT | CSR0_TINT | CSR0_MISS | CSR0_MERR | CSR0_BABL)

/* CSR3 (bus control) and CSR4 (features) - ILACC 68k configuration */
#define CSR3_ACON	BIT(1)		/* ALE control: match 680x0 bus */
#define CSR4_BACON_680X0 (0x1 << 6)	/* byte/word ordering = 680x0 */
/*
 * CSR4 extended-interrupt masks.  Each source (cause bit) is enabled unless
 * its mask bit is set.  TXSTRT (transmit-start) fires on *every* transmit;
 * left unmasked it holds the chip's INT pin asserted, and because the VIC
 * LIRQ7 input is edge-triggered the next receive never produces an edge -
 * the DHCP OFFER is never serviced.  Mask all four so only the CSR0 RX/TX
 * interrupts drive the line.
 */
#define CSR4_JABM	BIT(0)		/* mask jabber                     */
#define CSR4_TXSTRTM	BIT(2)		/* mask transmit-start             */
#define CSR4_RCVCCOM	BIT(4)		/* mask rx collision cnt overflow  */
#define CSR4_MFCOM	BIT(8)		/* mask missed frame cnt overflow  */
#define CSR4_IMASK	(CSR4_JABM | CSR4_TXSTRTM | CSR4_RCVCCOM | CSR4_MFCOM)

/* Init-block MODE (CSR15) bits */
#define MODE_DRX	BIT(0)
#define MODE_DTX	BIT(1)
#define MODE_PROM	BIT(15)

/*
 * Descriptor status bits, expressed in the position they occupy in the
 * driver's native u32 "flags" word (chip 16-bit status field << 16).
 */
#define DESC_OWN	BIT(31)		/* chip status OWN  (0x8000 << 16) */
#define DESC_ERR	BIT(30)		/*             ERR  (0x4000 << 16) */
#define DESC_FRAM	BIT(29)		/* RMD: framing error              */
#define DESC_OFLO	BIT(28)		/* RMD: overflow                   */
#define DESC_CRC	BIT(27)		/* RMD: CRC error                  */
#define DESC_BUFF	BIT(26)		/* RMD/TMD: buffer error           */
#define DESC_STP	BIT(25)		/* start of packet (0x0200 << 16)  */
#define DESC_ENP	BIT(24)		/* end   of packet (0x0100 << 16)  */

/* TMD "misc"/error word (native u32, low 16 bits are the chip TMD2) */
#define TMD_MISC_UFLO	BIT(14)
#define TMD_MISC_BUFF	BIT(15)
#define TMD_MISC_RTRY	BIT(10)

/* Ring sizes: RLEN/TLEN in the init block encode log2(#descriptors). */
#define RX_LOG		4
#define TX_LOG		2
#define RX_RING_SIZE	(1 << RX_LOG)
#define TX_RING_SIZE	(1 << TX_LOG)
#define RX_RING_MASK	(RX_RING_SIZE - 1)
#define TX_RING_MASK	(TX_RING_SIZE - 1)

/* Per-buffer size.  1518 = max untagged Ethernet frame incl. FCS. */
#define PKT_BUF_SZ	1518

/* Byte-count field for a descriptor: ones in 15:12, 2s-complement in 11:0 */
#define BCNT(len)	((0xf << 12) | ((-(len)) & 0xfff))

/*
 * Station-address PROM.  The six MAC bytes live in the byte-wide PROM that
 * overlays the LANCE window at base + 0x1d81 (physical 0xfec69d81), each byte
 * stored as two nibbles on ascending odd byte lanes, stride 2, high nibble
 * first.  This encoding was recovered from the RMON monitor's own station-
 * address reader (ROM fe816c28: base 0xfec68000, +0x1d01, +0x80, 12 reads at
 * stride 2, low nibble of each) - it is exactly how the factory firmware
 * obtains the address, so it yields the real per-board MAC on hardware.
 * (QEMU does not model this PROM and returns 0, so the read is validated and
 * falls back gracefully.)
 */
#define E17_LANCE_MAC_PROM	0xfec69d00
#define E17_LANCE_MAC_PROM_SZ	0x100
#define E17_LANCE_MAC_PROM_OFF	0x81	/* first nibble: 0xfec69d81 */
#define E17_LANCE_MAC_PROM_STRIDE 4	/* per MAC byte (two nibbles, +0/+2)  */

/*
 * The ILACC IRQ pin feeds VIC068A local input LIRQ7.  Masking/levelling LICR7
 * (and the LIVBR-vectored delivery at 0x47) is now owned by the VIC irqchip
 * (drivers/irqchip/irq-e17-vic068a.c); this driver just request_irq()s the
 * cascaded IRQ and enable/disable_irq()s it, so it no longer touches the VIC.
 */

/*
 * The initialisation block, 32-bit ("SSIZE32") ILACC layout, expressed as
 * the native u32 words this big-endian CPU writes.  See the file header
 * and DESIGN.md: each field already accounts for the on-the-wire 4-byte
 * lane reversal.
 */
struct e17lance_initblk {
	u32 w0;		/* (TLEN<<28) | (RLEN<<20) | MODE                   */
	u32 padr_lo;	/* mac[3]<<24 | mac[2]<<16 | mac[1]<<8 | mac[0]     */
	u32 padr_hi;	/* mac[5]<<8  | mac[4]                              */
	u32 ladrf01;	/* ladrf[1]<<16 | ladrf[0]                          */
	u32 ladrf23;	/* ladrf[3]<<16 | ladrf[2]                          */
	u32 rdra;	/* RX descriptor ring DMA base (16-byte aligned)   */
	u32 tdra;	/* TX descriptor ring DMA base (16-byte aligned)   */
	u32 pad;
};

/* One 32-bit descriptor (16 bytes), used for both RX and TX. */
struct e17lance_desc {
	u32 addr;	/* buffer DMA address (full 32-bit)                */
	u32 flags;	/* (status << 16) | byte-count field               */
	u32 mcnt;	/* RX: message length; TX: TMD2 misc/error word    */
	u32 res;
};

/* Complete DMA-coherent block handed to the chip. */
struct e17lance_dma {
	struct e17lance_initblk initblk;
	struct e17lance_desc rx_ring[RX_RING_SIZE];
	struct e17lance_desc tx_ring[TX_RING_SIZE];
	u8 rx_buf[RX_RING_SIZE][PKT_BUF_SZ];
	u8 tx_buf[TX_RING_SIZE][PKT_BUF_SZ];
};

struct e17lance_priv {
	struct net_device *dev;
	struct device *pdev;
	void __iomem *base;		/* RDP/RAP window                  */
	unsigned int rdp_off, rap_off;	/* byte offsets of RDP / RAP       */

	struct e17lance_dma *dma;	/* CPU (cache-inhibited) view      */
	dma_addr_t dma_handle;		/* device view (== physical)       */

	unsigned int rx_new;		/* next RX descriptor to inspect   */
	unsigned int tx_new;		/* next free TX descriptor         */
	unsigned int tx_old;		/* oldest TX awaiting completion   */
	u16 tx_len[TX_RING_SIZE];	/* length queued per TX descriptor */

	spinlock_t lock;
};

/* --- register access ---------------------------------------------------- */

static void lance_writereg(struct e17lance_priv *lp, unsigned int reg, u16 val)
{
	out_be16(lp->base + lp->rap_off, reg);
	out_be16(lp->base + lp->rdp_off, val);
}

/* CSR0 is left selected in RAP after most sequences; read it cheaply. */
static u16 lance_rdcsr0(struct e17lance_priv *lp)
{
	out_be16(lp->base + lp->rap_off, 0);
	return in_be16(lp->base + lp->rdp_off);
}

static void lance_wrcsr0(struct e17lance_priv *lp, u16 val)
{
	out_be16(lp->base + lp->rap_off, 0);
	out_be16(lp->base + lp->rdp_off, val);
}

/* --- init block / descriptor construction ------------------------------ */

static dma_addr_t rx_buf_dma(struct e17lance_priv *lp, int i)
{
	return lp->dma_handle + offsetof(struct e17lance_dma, rx_buf[i]);
}

static dma_addr_t tx_buf_dma(struct e17lance_priv *lp, int i)
{
	return lp->dma_handle + offsetof(struct e17lance_dma, tx_buf[i]);
}

static void lance_arm_rx_desc(struct e17lance_priv *lp, int i)
{
	struct e17lance_desc *rd = &lp->dma->rx_ring[i];

	rd->addr = rx_buf_dma(lp, i);
	rd->mcnt = 0;
	rd->res = 0;
	/* Hand ownership to the chip last. */
	wmb();
	rd->flags = DESC_OWN | BCNT(PKT_BUF_SZ);
}

static void lance_setup_rings(struct e17lance_priv *lp)
{
	struct e17lance_initblk *ib = &lp->dma->initblk;
	const u8 *mac = lp->dev->dev_addr;
	int i;

	for (i = 0; i < RX_RING_SIZE; i++)
		lance_arm_rx_desc(lp, i);

	for (i = 0; i < TX_RING_SIZE; i++) {
		struct e17lance_desc *td = &lp->dma->tx_ring[i];

		td->addr = tx_buf_dma(lp, i);
		td->flags = 0;		/* not owned by chip */
		td->mcnt = 0;
		td->res = 0;
	}

	ib->w0 = ((u32)TX_LOG << 28) | ((u32)RX_LOG << 20) | 0 /* MODE */;
	ib->padr_lo = ((u32)mac[3] << 24) | ((u32)mac[2] << 16) |
		      ((u32)mac[1] << 8) | mac[0];
	ib->padr_hi = ((u32)mac[5] << 8) | mac[4];
	ib->ladrf01 = 0;
	ib->ladrf23 = 0;
	ib->rdra = lp->dma_handle + offsetof(struct e17lance_dma, rx_ring);
	ib->tdra = lp->dma_handle + offsetof(struct e17lance_dma, tx_ring);
	ib->pad = 0;

	lp->rx_new = 0;
	lp->tx_new = 0;
	lp->tx_old = 0;
	wmb();
}

/* Update MODE and the logical-address filter for the current rx flags. */
static void lance_load_multicast(struct e17lance_priv *lp)
{
	struct net_device *dev = lp->dev;
	struct e17lance_initblk *ib = &lp->dma->initblk;
	/*
	 * Run promiscuous and filter in software.  The ILACC's hardware
	 * address filter does not accept our unicast on this board even though
	 * CSR12-14 read back the correct MAC (the receiver only ever sees
	 * broadcast otherwise, so the unicast DHCP OFFER is dropped and DHCP
	 * never completes).  The reference ROM monitor drives this NIC the same
	 * way.  The bounded RX service loop keeps the extra broadcast traffic
	 * from overrunning us.
	 */
	u16 mode = MODE_PROM;
	u16 ladrf[4] = { 0, 0, 0, 0 };

	if (dev->flags & IFF_PROMISC) {
		mode |= MODE_PROM;
	} else if ((dev->flags & IFF_ALLMULTI) ||
		   !netdev_mc_empty(dev)) {
		/*
		 * Accept-all-multicast keeps this simple and swap-invariant.
		 * A precise 64-bit hash filter would need the same 16-bit
		 * pair packing as padr/ladrf (see DESIGN.md); allmulti is a
		 * correct, conservative choice for a 10base5 SBC NIC.
		 */
		ladrf[0] = ladrf[1] = ladrf[2] = ladrf[3] = 0xffff;
	}

	ib->w0 = ((u32)TX_LOG << 28) | ((u32)RX_LOG << 20) | mode;
	ib->ladrf01 = ((u32)ladrf[1] << 16) | ladrf[0];
	ib->ladrf23 = ((u32)ladrf[3] << 16) | ladrf[2];
	wmb();
}

/* --- chip start / stop -------------------------------------------------- */

static int lance_init_chip(struct e17lance_priv *lp)
{
	u32 ib = lp->dma_handle + offsetof(struct e17lance_dma, initblk);
	u16 csr0;
	int i;

	lance_wrcsr0(lp, CSR0_STOP);
	udelay(100);

	/* ILACC bus configuration for the 680x0 big-endian bus. */
	lance_writereg(lp, 3, CSR3_ACON);
	lance_writereg(lp, 4, CSR4_BACON_680X0 | CSR4_IMASK);

	/* Point CSR1/CSR2 at the init block (word aligned). */
	lance_writereg(lp, 1, ib & 0xfffe);
	lance_writereg(lp, 2, (ib >> 16) & 0xffff);

	lance_wrcsr0(lp, CSR0_INIT);

	/*
	 * Poll for IDON with a delay-bounded loop (this runs with IRQs
	 * disabled under lp->lock, so jiffies must not be relied upon).
	 * INIT completes in well under a millisecond; 100 ms is generous.
	 */
	for (i = 0; i < 1000; i++) {
		csr0 = lance_rdcsr0(lp);
		if (csr0 & CSR0_ERR)
			return -EIO;
		if (csr0 & CSR0_IDON)
			return 0;
		udelay(100);
	}

	return -ETIMEDOUT;
}

static void lance_stop_chip(struct e17lance_priv *lp)
{
	lance_wrcsr0(lp, CSR0_STOP);
}

/* --- RX / TX ------------------------------------------------------------ */

static void lance_rx(struct net_device *dev)
{
	struct e17lance_priv *lp = netdev_priv(dev);
	int guard = RX_RING_SIZE;	/* never process more than one ring's worth */

	while (guard-- > 0) {
		struct e17lance_desc *rd = &lp->dma->rx_ring[lp->rx_new];
		u32 flags = rd->flags;
		int len;

		if (flags & DESC_OWN)		/* still owned by chip */
			break;

		if ((flags & (DESC_STP | DESC_ENP)) != (DESC_STP | DESC_ENP) ||
		    (flags & DESC_ERR)) {
			dev->stats.rx_errors++;
			if (flags & DESC_FRAM)
				dev->stats.rx_frame_errors++;
			if (flags & DESC_CRC)
				dev->stats.rx_crc_errors++;
			if (flags & DESC_OFLO)
				dev->stats.rx_over_errors++;
			if (flags & DESC_BUFF)
				dev->stats.rx_fifo_errors++;
			goto next;
		}

		len = (int)(rd->mcnt & 0xfff) - 4;	/* strip FCS */

		if (len >= ETH_ZLEN && len <= PKT_BUF_SZ) {
			struct sk_buff *skb = netdev_alloc_skb(dev, len + 2);

			if (skb) {
				skb_reserve(skb, 2);	/* 16-byte align IP */
				skb_put(skb, len);
				skb_copy_to_linear_data(skb,
					lp->dma->rx_buf[lp->rx_new], len);
				skb->protocol = eth_type_trans(skb, dev);
				netif_rx(skb);
				dev->stats.rx_packets++;
				dev->stats.rx_bytes += len;
			} else {
				dev->stats.rx_dropped++;
			}
		} else {
			dev->stats.rx_length_errors++;
		}
next:
		lance_arm_rx_desc(lp, lp->rx_new);
		lp->rx_new = (lp->rx_new + 1) & RX_RING_MASK;
	}
}

static void lance_tx(struct net_device *dev)
{
	struct e17lance_priv *lp = netdev_priv(dev);

	while (lp->tx_old != lp->tx_new) {
		struct e17lance_desc *td = &lp->dma->tx_ring[lp->tx_old];
		u32 flags = td->flags;

		if (flags & DESC_OWN)		/* not sent yet */
			break;

		if (flags & DESC_ERR) {
			u32 misc = td->mcnt;

			dev->stats.tx_errors++;
			if (misc & TMD_MISC_UFLO)
				dev->stats.tx_fifo_errors++;
			if (misc & TMD_MISC_BUFF)
				dev->stats.tx_aborted_errors++;
			if (misc & TMD_MISC_RTRY)
				dev->stats.tx_window_errors++;
		} else {
			dev->stats.tx_packets++;
			dev->stats.tx_bytes += lp->tx_len[lp->tx_old];
		}

		lp->tx_old = (lp->tx_old + 1) & TX_RING_MASK;
	}

	if (netif_queue_stopped(dev) &&
	    ((lp->tx_new + 1) & TX_RING_MASK) != lp->tx_old)
		netif_wake_queue(dev);
}

static irqreturn_t lance_interrupt(int irq, void *dev_id)
{
	struct net_device *dev = dev_id;
	struct e17lance_priv *lp = netdev_priv(dev);
	int guard = 4 * RX_RING_SIZE;
	u16 csr0;

	spin_lock(&lp->lock);

	csr0 = lance_rdcsr0(lp);
	if (!(csr0 & CSR0_INTR)) {		/* not our line (shared-safe) */
		spin_unlock(&lp->lock);
		return IRQ_NONE;
	}

	/*
	 * Service in a bounded loop until no actionable cause remains.  The
	 * VIC068A local input for the ILACC is edge-sensitive while the chip's
	 * INT pin is level-held, so a frame that latches after we ACK but
	 * before we return would otherwise leave INT asserted with no new edge
	 * and never be serviced.  Draining here re-arms the edge.  We loop on
	 * CSR0_WORK (not the INTR summary): CERR is set after every transmit on
	 * a heartbeat-less transceiver and would otherwise spin us forever with
	 * IRQs off (that hangs real hardware; QEMU never sets CERR).  The guard
	 * caps the work per interrupt so a busy segment cannot livelock us.
	 */
	do {
		/* Acknowledge the latched sources; keep interrupts enabled. */
		lance_wrcsr0(lp, CSR0_INEA | (csr0 & CSR0_ACK));

		if (csr0 & CSR0_RINT)
			lance_rx(dev);
		if (csr0 & CSR0_TINT)
			lance_tx(dev);

		if (csr0 & CSR0_MERR)
			dev->stats.rx_errors++;
		if (csr0 & CSR0_MISS)
			dev->stats.rx_missed_errors++;
		if (csr0 & CSR0_BABL)
			dev->stats.tx_errors++;

		csr0 = lance_rdcsr0(lp);
	} while ((csr0 & CSR0_WORK) && --guard > 0);

	spin_unlock(&lp->lock);
	return IRQ_HANDLED;
}

static netdev_tx_t lance_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct e17lance_priv *lp = netdev_priv(dev);
	unsigned int entry;
	unsigned long flags;
	int len;

	len = skb->len;
	if (len > PKT_BUF_SZ) {
		dev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	spin_lock_irqsave(&lp->lock, flags);

	entry = lp->tx_new;
	if (lp->dma->tx_ring[entry].flags & DESC_OWN) {
		/* ring full */
		netif_stop_queue(dev);
		spin_unlock_irqrestore(&lp->lock, flags);
		return NETDEV_TX_BUSY;
	}

	skb_copy_from_linear_data(skb, lp->dma->tx_buf[entry], len);
	if (len < ETH_ZLEN) {
		memset(lp->dma->tx_buf[entry] + len, 0, ETH_ZLEN - len);
		len = ETH_ZLEN;
	}

	lp->dma->tx_ring[entry].addr = tx_buf_dma(lp, entry);
	lp->dma->tx_ring[entry].mcnt = 0;
	lp->dma->tx_ring[entry].res = 0;
	lp->tx_len[entry] = len;
	wmb();
	lp->dma->tx_ring[entry].flags =
		DESC_OWN | DESC_STP | DESC_ENP | BCNT(len);

	lp->tx_new = (lp->tx_new + 1) & TX_RING_MASK;

	/* Kick the transmitter; completion is reaped in the interrupt (TINT). */
	lance_wrcsr0(lp, CSR0_INEA | CSR0_TDMD);

	if (lp->dma->tx_ring[lp->tx_new].flags & DESC_OWN)
		netif_stop_queue(dev);

	spin_unlock_irqrestore(&lp->lock, flags);

	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

static void lance_tx_timeout(struct net_device *dev, unsigned int txqueue)
{
	struct e17lance_priv *lp = netdev_priv(dev);
	unsigned long flags;

	netdev_warn(dev, "transmit timed out, resetting\n");

	spin_lock_irqsave(&lp->lock, flags);
	lance_stop_chip(lp);
	lance_setup_rings(lp);
	lance_load_multicast(lp);
	if (lance_init_chip(lp) == 0)
		lance_wrcsr0(lp, CSR0_STRT | CSR0_INEA);
	spin_unlock_irqrestore(&lp->lock, flags);

	netif_trans_update(dev);
	netif_wake_queue(dev);
}

/* --- open / stop -------------------------------------------------------- */

static int lance_open(struct net_device *dev)
{
	struct e17lance_priv *lp = netdev_priv(dev);
	unsigned long flags;
	int ret;

	/*
	 * Request masked (NO_AUTOEN); the VIC line stays masked until the chip
	 * is initialised and started, then we enable_irq() to unmask LICR7.
	 */
	ret = request_irq(dev->irq, lance_interrupt, IRQF_NO_AUTOEN,
			  dev->name, dev);
	if (ret) {
		netdev_err(dev, "cannot get IRQ %d\n", dev->irq);
		return ret;
	}

	spin_lock_irqsave(&lp->lock, flags);

	lance_setup_rings(lp);
	lance_load_multicast(lp);

	ret = lance_init_chip(lp);
	if (ret) {
		spin_unlock_irqrestore(&lp->lock, flags);
		netdev_err(dev, "chip init failed (%d)\n", ret);
		free_irq(dev->irq, dev);
		return ret;
	}

	/* Start the chip with interrupts enabled, then unmask VIC LIRQ7. */
	lance_wrcsr0(lp, CSR0_STRT | CSR0_INEA);
	enable_irq(dev->irq);		/* VIC irqchip unmasks LICR7 */

	spin_unlock_irqrestore(&lp->lock, flags);

	netif_start_queue(dev);
	return 0;
}

static int lance_close(struct net_device *dev)
{
	struct e17lance_priv *lp = netdev_priv(dev);
	unsigned long flags;

	netif_stop_queue(dev);

	spin_lock_irqsave(&lp->lock, flags);
	/* IRQ_DISABLE_UNLAZY: masks LICR7 at the VIC immediately, non-sleeping. */
	disable_irq_nosync(dev->irq);
	lance_stop_chip(lp);
	spin_unlock_irqrestore(&lp->lock, flags);

	free_irq(dev->irq, dev);
	return 0;
}

static void lance_set_multicast(struct net_device *dev)
{
	struct e17lance_priv *lp = netdev_priv(dev);
	unsigned long flags;

	if (!netif_running(dev))
		return;

	spin_lock_irqsave(&lp->lock, flags);
	/* MODE/LADRF changes only take effect across a re-INIT. */
	lance_stop_chip(lp);
	lance_setup_rings(lp);
	lance_load_multicast(lp);
	if (lance_init_chip(lp) == 0)
		lance_wrcsr0(lp, CSR0_STRT | CSR0_INEA);
	spin_unlock_irqrestore(&lp->lock, flags);
}

static const struct net_device_ops e17lance_netdev_ops = {
	.ndo_open		= lance_open,
	.ndo_stop		= lance_close,
	.ndo_start_xmit		= lance_start_xmit,
	.ndo_set_rx_mode	= lance_set_multicast,
	.ndo_tx_timeout		= lance_tx_timeout,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_set_mac_address	= eth_mac_addr,
};

/* --- MAC address -------------------------------------------------------- */

/*
 * Obtain the station address.  Priority:
 *   1. a "local-mac-address"/"mac-address"/nvmem DT property (u-boot fills
 *      this on the real board, and it is the reliable source);
 *   2. the on-board station-address PROM at 0xfec69d81 (the same source and
 *      nibble encoding the RMON firmware reads - see E17_LANCE_MAC_PROM; the
 *      QEMU model returns 0 here, so it is validated before use);
 *   3. a random address with the ELTEC OUI, as a last resort.
 */
static void lance_get_mac(struct platform_device *pdev, struct net_device *dev)
{
	u8 mac[ETH_ALEN];
	void __iomem *prom;
	int i;

	if (of_get_mac_address(pdev->dev.of_node, mac) == 0 &&
	    is_valid_ether_addr(mac)) {
		eth_hw_addr_set(dev, mac);
		return;
	}

	prom = ioremap(E17_LANCE_MAC_PROM, E17_LANCE_MAC_PROM_SZ);
	if (prom) {
		/*
		 * Each of the six address bytes is two nibbles at +0x81+i*4
		 * (high) and +0x83+i*4 (low), i.e. odd byte lanes at stride 2,
		 * exactly as RMON's fe816c28 reads them.  Assemble and validate.
		 */
		for (i = 0; i < ETH_ALEN; i++) {
			unsigned int o = E17_LANCE_MAC_PROM_OFF +
					 i * E17_LANCE_MAC_PROM_STRIDE;
			u8 hi = readb(prom + o) & 0x0f;
			u8 lo = readb(prom + o + 2) & 0x0f;

			mac[i] = (hi << 4) | lo;
		}
		iounmap(prom);

		if (is_valid_ether_addr(mac)) {
			eth_hw_addr_set(dev, mac);
			return;
		}
	}

	/* ELTEC OUI 00:00:5B, random low bytes. */
	eth_random_addr(mac);
	mac[0] = 0x00;
	mac[1] = 0x00;
	mac[2] = 0x5b;
	eth_hw_addr_set(dev, mac);
	dev_warn(&pdev->dev, "no MAC from DT/PROM, using random %pM\n", mac);
}

/* --- probe / remove ----------------------------------------------------- */

static int e17lance_probe(struct platform_device *pdev)
{
	struct net_device *dev;
	struct e17lance_priv *lp;
	struct resource *res;
	int irq, ret;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	dev = alloc_etherdev(sizeof(*lp));
	if (!dev)
		return -ENOMEM;

	SET_NETDEV_DEV(dev, &pdev->dev);
	lp = netdev_priv(dev);
	lp->dev = dev;
	lp->pdev = &pdev->dev;
	spin_lock_init(&lp->lock);

	lp->base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(lp->base)) {
		ret = PTR_ERR(lp->base);
		goto out_free;
	}
	dev->base_addr = res->start;

	if (of_property_read_u32(pdev->dev.of_node, "rdp-offset", &lp->rdp_off))
		lp->rdp_off = 0x2;
	if (of_property_read_u32(pdev->dev.of_node, "rap-offset", &lp->rap_off))
		lp->rap_off = 0x6;

	lp->dma = dma_alloc_coherent(&pdev->dev, sizeof(*lp->dma),
				     &lp->dma_handle, GFP_KERNEL);
	if (!lp->dma) {
		ret = -ENOMEM;
		goto out_free;
	}

	dev->irq = irq;
	dev->netdev_ops = &e17lance_netdev_ops;
	dev->watchdog_timeo = 5 * HZ;

	lance_get_mac(pdev, dev);

	/*
	 * Make sure the chip is quiet.  The VIC line comes up masked (the
	 * irqchip masks LICR7 when the IRQ is mapped) and stays masked until
	 * lance_open() enables it.
	 */
	lance_stop_chip(lp);

	ret = register_netdev(dev);
	if (ret)
		goto out_dma;

	platform_set_drvdata(pdev, dev);
	netdev_info(dev, "E17 ILACC at 0x%08lx, IRQ %d, MAC %pM\n",
		    dev->base_addr, dev->irq, dev->dev_addr);
	return 0;

out_dma:
	dma_free_coherent(&pdev->dev, sizeof(*lp->dma), lp->dma, lp->dma_handle);
out_free:
	free_netdev(dev);
	return ret;
}

static void e17lance_remove(struct platform_device *pdev)
{
	struct net_device *dev = platform_get_drvdata(pdev);
	struct e17lance_priv *lp = netdev_priv(dev);

	unregister_netdev(dev);
	dma_free_coherent(&pdev->dev, sizeof(*lp->dma), lp->dma, lp->dma_handle);
	free_netdev(dev);
}

static const struct of_device_id e17lance_of_match[] = {
	{ .compatible = "amd,am79900" },
	{ .compatible = "eltec,e17-lance" },
	{ }
};
MODULE_DEVICE_TABLE(of, e17lance_of_match);

static struct platform_driver e17lance_driver = {
	.probe	= e17lance_probe,
	.remove	= e17lance_remove,
	.driver	= {
		.name		= DRV_NAME,
		.of_match_table	= e17lance_of_match,
	},
};
module_platform_driver(e17lance_driver);

MODULE_DESCRIPTION("ELTEC Eurocom E17 Am79C900 ILACC Ethernet driver");
MODULE_LICENSE("GPL");
