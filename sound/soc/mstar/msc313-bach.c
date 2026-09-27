// SPDX-License-Identifier: GPL-2.0
/*
 * MStar/SigmaStar "BACH" audio: on-chip codec (audio top), DPGAs and the
 * DMA reader/writer pair that feed and drain it.
 *
 * Copyright (C) 2021 Daniel Palmer <daniel@thingy.jp>
 *
 * The DMA engine is not a free-running ring: each direction keeps a level
 * counter of the bytes it may work on. The host writes samples into the
 * ring and then "triggers" that many bytes, which adds them to the reader's
 * level; the reader consumes them and the level drops. The writer fills its
 * ring and its level grows; the host reads samples out and triggers that
 * many bytes to release them. Threshold interrupts (level below a value for
 * the reader, above it for the writer) give the period ticks, and the
 * pointer comes straight from the level. The vendor HAL (MHAL_AUDIO in
 * mhal.ko) works the same way and the register semantics below follow it.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include <sound/core.h>
#include <sound/jack.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/tlv.h>

#define DRIVER_NAME "msc313-bach"

/*
 * The DMA works in "miu" units of 8 (msc313) or 16 (ssd210) bytes; all
 * addresses, sizes, levels and thresholds are in those units. Buffers and
 * periods are kept to a multiple of 16 bytes so both work.
 */
#define MSC313_BACH_ALIGNMENT	16

/* Bank 0 */
#define REG_SR_SEL		0x004
#define REG_MUX0SEL		0x00c
#define REG_DPGA_PLAYBACK	0x084	/* MMC1 DPGA CFG2: gain L (7:0), R (15:8) */
#define REG_DPGA_CAPTURE	0x094
#define REG_DMA_CTRL		0x100
#define REG_DMA_RD		0x104	/* reader sub-channel */
#define REG_DMA_FLAGS		0x120
#define REG_DMA_WR		0x124	/* writer sub-channel */
#define REG_SINEGEN		0x1d4
#define REG_DMA_TEST_CTRL7	0x1dc
#define REG_DMA_INT		0x21c

/* sub-channel register offsets */
#define SUB_CTRL		0x00	/* address low bits and control bits */
#define SUB_ADDR_HI		0x04
#define SUB_SIZE		0x08
#define SUB_TRIGGER		0x0c
#define SUB_OVERRUN_THR		0x10
#define SUB_UNDERRUN_THR	0x14
#define SUB_LEVEL		0x18

/* Audio top (analog) registers, reached through the codec as 0x1000 + offset */
#define REG_ATOP_OFFSET		0x1000
#define REG_ATOP_ANALOG_CTRL0	(REG_ATOP_OFFSET + 0x00)
#define REG_ATOP_ANALOG_CTRL1	(REG_ATOP_OFFSET + 0x04)
#define REG_ATOP_ANALOG_CTRL3	(REG_ATOP_OFFSET + 0x0c)	/* power downs, 1 = down */
#define REG_ATOP_ADC_MUX	(REG_ATOP_OFFSET + 0x14)
#define REG_ATOP_ADC_GAIN	(REG_ATOP_OFFSET + 0x18)
#define REG_ATOP_MIC_GAIN	(REG_ATOP_OFFSET + 0x20)

/* REG_ATOP_ANALOG_CTRL3: the vendor's three "atop paths" */
#define ATOP_PD_ADC_BIAS	(BIT(0) | BIT(2) | BIT(5))
#define ATOP_PD_ADC		(BIT(7) | BIT(8))
#define ATOP_PD_DAC		(BIT(1) | BIT(4) | BIT(6) | BIT(9) | BIT(10))
#define ATOP_PD_ALL		(BIT(11) | BIT(12))

#define ATOP_ADC_MUX_LINEIN	0x00
#define ATOP_ADC_MUX_MICIN	0x77

struct msc313_bach;

struct msc313_bach_sub {
	struct msc313_bach *bach;
	bool writer;

	struct regmap_field *en;
	struct regmap_field *init;
	struct regmap_field *trigger;
	struct regmap_field *count;
	struct regmap_field *addr_lo, *addr_hi;
	struct regmap_field *size;
	struct regmap_field *trigger_level;
	struct regmap_field *overrun_thr;
	struct regmap_field *underrun_thr;
	struct regmap_field *level;
	struct regmap_field *int_clear;
	struct regmap_field *int_thr_en;	/* reader: underrun, writer: overrun */
	struct regmap_field *int_edge_en;	/* reader: empty, writer: full */
	struct regmap_field *flag_thr;
	struct regmap_field *flag_edge;
	struct regmap_field *mono, *mono2;
	struct regmap_field *rate_sel, *rate_sel2;

	struct snd_pcm_substream *substream;

	/* stream state, under bach->lock */
	bool running;
	size_t buf_bytes;
	size_t period_bytes;
	size_t queued;		/* bytes triggered so far: to play, or released for capture */
	size_t pending;		/* bytes from the application not yet triggered */
	size_t next_period;	/* stream position of the next period boundary */
	snd_pcm_uframes_t last_appl;
};

struct msc313_bach_data {
	unsigned int addr_sz_shift;
};

struct msc313_bach {
	struct device *dev;
	const struct msc313_bach_data *data;
	struct clk *clk;

	struct regmap *bach;
	struct regmap *audiotop;

	/* Serialises the DMA registers between the PCM callbacks and the IRQ */
	spinlock_t lock;

	struct regmap_field *dma_rst;
	struct regmap_field *dma_en;
	struct regmap_field *dma_live_count_en;
	struct regmap_field *dma_int_en;
	struct msc313_bach_sub reader;
	struct msc313_bach_sub writer;
	unsigned int open_streams;

	struct gpio_desc *amp_gpio;
	struct snd_soc_jack hp_jack;
	struct snd_soc_jack_gpio hp_jack_gpio;

	struct snd_soc_dai_link_component cpu_dai_component;
	struct snd_soc_dai_link_component platform_component;
	struct snd_soc_dai_link_component codec_component;
	struct snd_soc_dai_link dai_link;
	struct snd_soc_card card;
};

/* Registering the card claims the device's driver data, so go through it */
static inline struct msc313_bach *msc313_bach_from_component(struct snd_soc_component *component)
{
	return snd_soc_card_get_drvdata(component->card);
}

#define TO_MIU(_bach, _x)	((_x) >> (_bach)->data->addr_sz_shift)
#define FROM_MIU(_bach, _x)	((size_t)(_x) << (_bach)->data->addr_sz_shift)

/*
 * Register defaults, from a running vendor system. The DMA sub-channel
 * registers are programmed per stream and are left out.
 */
static const struct reg_sequence msc313_bach_atop_init[] = {
	{ 0x00, 0x0a14 }, { 0x04, 0x0030 }, { 0x08, 0x0080 },
	/* everything analog powered down; DAPM brings the paths up */
	{ 0x0c, ATOP_PD_ADC_BIAS | ATOP_PD_ADC | ATOP_PD_DAC | ATOP_PD_ALL },
	{ 0x10, 0 }, { 0x14, 0 }, { 0x18, 0 }, { 0x1c, 0 }, { 0x20, 0x3000 },
	{ 0x24, 0 }, { 0x28, 0 }, { 0x2c, 0 }, { 0x30, 0 }, { 0x34, 0 },
	{ 0x38, 0 }, { 0x3c, 0 }, { 0x40, 0 }, { 0x44, 0 }, { 0x48, 0 },
	{ 0x4c, 0 }, { 0x50, 0 }, { 0x54, 0 }, { 0x58, 0 }, { 0x5c, 0 },
	{ 0x60, 0 }, { 0x64, 0 }, { 0x68, 0 }, { 0x6c, 0 }, { 0x70, 0 },
	{ 0x74, 0 }, { 0x78, 0 }, { 0x7c, 0 }, { 0x80, 0 }, { 0x84, 0x3c1e },
	{ 0x88, 0 }, { 0x8c, 0 }, { 0x90, 0 }, { 0x94, 0 }, { 0x98, 0 },
	{ 0x9c, 0 }, { 0xa0, 0 }, { 0xa4, 0 }, { 0xa8, 0 }, { 0xac, 0 },
	{ 0xb0, 0 }, { 0xb4, 0 }, { 0xb8, 0 }, { 0xbc, 0 }, { 0xc0, 0 },
	{ 0xc4, 0 }, { 0xc8, 0 }, { 0xcc, 0 }, { 0xd0, 0 }, { 0xd4, 0 },
	{ 0xd8, 0 }, { 0xdc, 0 }, { 0xe0, 0 }, { 0xe4, 0 }, { 0xe8, 0 },
	{ 0xec, 0 }, { 0xf0, 0 }, { 0xf4, 0 }, { 0xf8, 0 }, { 0xfc, 0 },
};

static const struct reg_sequence msc313_bach_init[] = {
	{ 0x000, 0x89ff }, { 0x004, 0xff00 }, { 0x008, 0x0003 },
	{ REG_MUX0SEL, 0x19b4 }, { 0x010, 0xf000 }, { 0x014, 0x8000 },
	{ 0x018, 0xc09a }, { 0x01c, 0x555a }, { 0x020, 0 }, { 0x024, 0x0209 },
	{ 0x028, 0 }, { 0x02c, 0x007d }, { 0x030, 0 }, { 0x034, 0 },
	{ 0x038, 0x3017 }, { 0x03c, 0x0002 },
	/* DPGAs */
	{ 0x040, 0x9400 }, { 0x044, 0x9400 }, { 0x048, 0x9400 }, { 0x04c, 0xd400 },
	{ 0x050, 0x8400 }, { 0x054, 0xd000 }, { 0x058, 0x9400 }, { 0x05c, 0x9400 },
	{ 0x060, 0x8400 }, { 0x064, 0 }, { 0x068, 0 }, { 0x06c, 0 }, { 0x070, 0 },
	{ 0x074, 0 }, { 0x078, 0 }, { 0x07c, 0 }, { 0x080, 0x0005 },
	{ REG_DPGA_PLAYBACK, 0 }, { 0x088, 0x0007 }, { 0x08c, 0 }, { 0x090, 0x0037 },
	{ REG_DPGA_CAPTURE, 0 }, { 0x098, 0x0007 }, { 0x09c, 0 }, { 0x0a0, 0x0037 },
	{ 0x0a4, 0 }, { 0x0a8, 0x0007 }, { 0x0ac, 0 }, { 0x0b0, 0x0007 }, { 0x0b4, 0 },
	{ 0x0b8, 0x0007 }, { 0x0bc, 0 }, { 0x0c0, 0x0037 }, { 0x0c4, 0 }, { 0x0c8, 0x0007 },
	{ 0x0cc, 0 }, { 0x0d0, 0 }, { 0x0d4, 0 }, { 0x0d8, 0 }, { 0x0dc, 0 }, { 0x0e0, 0 },
	{ 0x0e4, 0 }, { 0x0e8, 0 }, { 0x0ec, 0 }, { 0x0f0, 0 }, { 0x0f4, 0 }, { 0x0f8, 0 },
	{ 0x0fc, 0 },
	/* DMA test/misc */
	{ 0x140, 0 }, { 0x144, 0 }, { 0x148, 0 }, { 0x14c, 0 }, { 0x150, 0 },
	{ 0x154, 0 }, { 0x158, 0 }, { 0x15c, 0 }, { 0x160, 0 }, { 0x164, 0 },
	{ 0x168, 0 }, { 0x16c, 0 }, { 0x170, 0 }, { 0x174, 0 }, { 0x178, 0 },
	{ 0x17c, 0 }, { 0x180, 0 }, { 0x184, 0 }, { 0x188, 0 }, { 0x18c, 0 },
	{ 0x190, 0 }, { 0x194, 0 }, { 0x198, 0 }, { 0x19c, 0 }, { 0x1a0, 0 },
	{ 0x1a4, 0 }, { 0x1a8, 0 }, { 0x1ac, 0 }, { 0x1b0, 0 }, { 0x1b4, 0 },
	{ 0x1b8, 0 }, { 0x1bc, 0 }, { 0x1c0, 0 }, { 0x1c4, 0 }, { 0x1c8, 0 },
	{ 0x1cc, 0x00e3 }, { 0x1d0, 0x0097 },
	/* sine generator: off, into the reader path */
	{ REG_SINEGEN, 0x6000 },
	{ 0x1d8, 0 }, { REG_DMA_TEST_CTRL7, 0x0400 }, { 0x1e0, 0 }, { 0x1e4, 0 },
	{ 0x1e8, 0 }, { 0x1ec, 0 }, { 0x1f0, 0 }, { 0x1f4, 0 }, { 0x1f8, 0 },
	{ 0x1fc, 0 },
	/* Bank 1 */
	{ 0x200, 0 }, { 0x204, 0 }, { 0x208, 0 }, { 0x20c, 0 }, { 0x210, 0x4000 },
	{ 0x214, 0x0100 }, { 0x218, 0x03e8 }, { 0x220, 0 }, { 0x224, 0 },
	{ 0x228, 0 }, { 0x22c, 0 }, { 0x230, 0 }, { 0x234, 0 }, { 0x238, 0x0003 },
	{ 0x23c, 0 }, { 0x240, 0x38c0 }, { 0x244, 0x3838 }, { 0x248, 0x0c04 },
	{ 0x24c, 0x1c14 }, { 0x250, 0x0001 }, { 0x254, 0 }, { 0x258, 0x0003 },
	{ 0x25c, 0 }, { 0x260, 0 }, { 0x264, 0 }, { 0x268, 0 }, { 0x26c, 0x0202 },
	{ 0x270, 0 }, { 0x274, 0 }, { 0x278, 0 }, { 0x27c, 0 }, { 0x280, 0 },
	{ 0x284, 0 }, { 0x288, 0 }, { 0x28c, 0 }, { 0x290, 0 }, { 0x294, 0x1234 },
	{ 0x298, 0x5678 }, { 0x29c, 0 }, { 0x2a0, 0 }, { 0x2a4, 0 }, { 0x2a8, 0 },
	{ 0x2ac, 0 }, { 0x2b0, 0 }, { 0x2b4, 0 }, { 0x2b8, 0 }, { 0x2bc, 0 },
	{ 0x2c0, 0 }, { 0x2c4, 0 }, { 0x2c8, 0 }, { 0x2cc, 0 }, { 0x2d0, 0 },
	{ 0x2d4, 0 }, { 0x2d8, 0 }, { 0x2dc, 0 }, { 0x2e0, 0 }, { 0x2e4, 0 },
	{ 0x2e8, 0 }, { 0x2ec, 0 }, { 0x2f0, 0 }, { 0x2f4, 0 }, { 0x2f8, 0 },
	{ 0x2fc, 0 }, { 0x300, 0 }, { 0x304, 0 }, { 0x308, 0 }, { 0x30c, 0 },
	{ 0x310, 0 }, { 0x314, 0 }, { 0x318, 0 }, { 0x31c, 0 }, { 0x320, 0 },
	{ 0x324, 0 }, { 0x328, 0 }, { 0x32c, 0x0001 }, { 0x330, 0 }, { 0x334, 0 },
	{ 0x338, 0 }, { 0x33c, 0 }, { 0x340, 0 }, { 0x344, 0 }, { 0x348, 0 },
	{ 0x34c, 0 }, { 0x350, 0 }, { 0x354, 0 }, { 0x358, 0 }, { 0x35c, 0 },
	{ 0x360, 0 }, { 0x364, 0 }, { 0x368, 0 }, { 0x36c, 0 }, { 0x370, 0 },
	{ 0x374, 0 }, { 0x378, 0 }, { 0x37c, 0x0080 }, { 0x380, 0 }, { 0x384, 0 },
	{ 0x388, 0xff34 }, { 0x38c, 0 }, { 0x390, 0x7fff }, { 0x394, 0x7fe9 },
	{ 0x398, 0 }, { 0x39c, 0 }, { 0x3a0, 0 }, { 0x3a4, 0 }, { 0x3a8, 0 },
	{ 0x3ac, 0xfea6 }, { 0x3b0, 0x019d }, { 0x3b4, 0 }, { 0x3b8, 0 },
	{ 0x3bc, 0x78f4 }, { 0x3c0, 0 }, { 0x3c4, 0 }, { 0x3c8, 0x10d3 },
	{ 0x3cc, 0x0942 }, { 0x3d0, 0 }, { 0x3d4, 0 }, { 0x3d8, 0xfdb6 },
	{ 0x3dc, 0xf291 }, { 0x3e0, 0x78f4 }, { 0x3e4, 0 }, { 0x3e8, 0 },
	{ 0x3ec, 0 }, { 0x3f0, 0x7fff }, { 0x3f4, 0 }, { 0x3f8, 0x0001 }, { 0x3fc, 0 },
	/* Bank 2 */
	{ 0x400, 0 }, { 0x404, 0x0021 }, { 0x408, 0 }, { 0x40c, 0 }, { 0x410, 0x000a },
	{ 0x414, 0x8000 }, { 0x418, 0x011f }, { 0x41c, 0 }, { 0x420, 0 }, { 0x424, 0 },
	{ 0x428, 0 }, { 0x42c, 0 }, { 0x430, 0 }, { 0x434, 0 }, { 0x438, 0 },
	{ 0x43c, 0xffff }, { 0x440, 0 }, { 0x444, 0x0001 }, { 0x448, 0x8000 },
	{ 0x44c, 0x0001 }, { 0x450, 0x8000 }, { 0x454, 0 }, { 0x458, 0 }, { 0x45c, 0 },
	{ 0x460, 0 }, { 0x464, 0 }, { 0x468, 0 }, { 0x46c, 0 }, { 0x470, 0 },
	{ 0x474, 0 }, { 0x478, 0 }, { 0x47c, 0 }, { 0x480, 0x0001 }, { 0x484, 0 },
	{ 0x488, 0 }, { 0x48c, 0 }, { 0x490, 0 }, { 0x494, 0 }, { 0x498, 0 },
	{ 0x49c, 0 }, { 0x4a0, 0 }, { 0x4a4, 0 }, { 0x4a8, 0 }, { 0x4ac, 0 },
	{ 0x4b0, 0 }, { 0x4b4, 0 }, { 0x4b8, 0 }, { 0x4bc, 0 }, { 0x4c0, 0 },
	{ 0x4c4, 0 }, { 0x4c8, 0 }, { 0x4cc, 0 }, { 0x4d0, 0 }, { 0x4d4, 0 },
	{ 0x4d8, 0 }, { 0x4dc, 0 }, { 0x4e0, 0 }, { 0x4e4, 0 }, { 0x4e8, 0 },
	{ 0x4ec, 0 }, { 0x4f0, 0 }, { 0x4f4, 0 }, { 0x4f8, 0 }, { 0x4fc, 0 },
	{ 0x500, 0x0080 }, { 0x504, 0x0078 }, { 0x508, 0 }, { 0x50c, 0 }, { 0x510, 0 },
	{ 0x514, 0 }, { 0x518, 0 }, { 0x51c, 0 }, { 0x520, 0 }, { 0x524, 0 },
	{ 0x528, 0 }, { 0x52c, 0 }, { 0x530, 0 }, { 0x534, 0 }, { 0x538, 0 },
	{ 0x53c, 0 }, { 0x540, 0 }, { 0x544, 0 }, { 0x548, 0 }, { 0x54c, 0 },
	{ 0x550, 0 }, { 0x554, 0 }, { 0x558, 0 }, { 0x55c, 0 }, { 0x560, 0 },
	{ 0x564, 0 }, { 0x568, 0 }, { 0x56c, 0 }, { 0x570, 0 }, { 0x574, 0 },
	{ 0x578, 0 }, { 0x57c, 0 }, { 0x580, 0 }, { 0x584, 0 }, { 0x588, 0 },
	{ 0x58c, 0 }, { 0x590, 0 }, { 0x594, 0 }, { 0x598, 0 }, { 0x59c, 0 },
	{ 0x5a0, 0 }, { 0x5a4, 0 }, { 0x5a8, 0 }, { 0x5ac, 0 }, { 0x5b0, 0 },
	{ 0x5b4, 0 }, { 0x5b8, 0 }, { 0x5bc, 0 }, { 0x5c0, 0 }, { 0x5c4, 0x0b0b },
	{ 0x5c8, 0 }, { 0x5cc, 0x4a4a }, { 0x5d0, 0x4a4a }, { 0x5d4, 0 }, { 0x5d8, 0 },
	{ 0x5dc, 0x4949 }, { 0x5e0, 0x4949 }, { 0x5e4, 0 }, { 0x5e8, 0 }, { 0x5ec, 0 },
	{ 0x5f0, 0 }, { 0x5f4, 0 }, { 0x5f8, 0 }, { 0x5fc, 0 },
};

/* --- DMA sub-channel helpers, called with bach->lock held --- */

static unsigned int msc313_bach_sub_level(struct msc313_bach_sub *sub)
{
	unsigned int level;

	regmap_field_force_write(sub->count, 1);
	regmap_field_read(sub->level, &level);
	regmap_field_force_write(sub->count, 0);

	return level;
}

static size_t msc313_bach_sub_level_bytes(struct msc313_bach_sub *sub)
{
	return FROM_MIU(sub->bach, msc313_bach_sub_level(sub));
}

/* Hand @bytes (a multiple of the alignment) to the hardware */
static void msc313_bach_sub_push(struct msc313_bach_sub *sub, size_t bytes)
{
	unsigned int trig;

	if (!bytes)
		return;

	regmap_field_write(sub->trigger_level, TO_MIU(sub->bach, bytes));
	regmap_field_read(sub->trigger, &trig);
	regmap_field_force_write(sub->trigger, !trig);
	sub->queued += bytes;
}

static void msc313_bach_sub_push_pending(struct msc313_bach_sub *sub)
{
	size_t bytes = sub->pending - (sub->pending % MSC313_BACH_ALIGNMENT);

	msc313_bach_sub_push(sub, bytes);
	sub->pending -= bytes;
}

/* Stream position in bytes since the stream was prepared */
static size_t msc313_bach_sub_position(struct msc313_bach_sub *sub)
{
	size_t level = msc313_bach_sub_level_bytes(sub);

	if (sub->writer)
		return sub->queued + level;
	if (level > sub->queued)
		return sub->queued;
	return sub->queued - level;
}

static void msc313_bach_sub_clear_irq(struct msc313_bach_sub *sub)
{
	regmap_field_force_write(sub->int_clear, 1);
	regmap_field_force_write(sub->int_clear, 0);
}

/*
 * Point the threshold interrupt at the next period boundary. The flag bits
 * in REG_DMA_FLAGS read as set all the time, so only the level is trusted:
 * the reader's interrupt asserts while its level is below the threshold and
 * the writer's while it is above, and both are quiet again once the
 * threshold is rewritten for a boundary that has not been reached.
 *
 * Returns true when a boundary has been passed (or the stream ran dry),
 * in which case the caller owes ALSA a period tick and should arm again.
 */
static bool msc313_bach_sub_arm(struct msc313_bach_sub *sub)
{
	struct msc313_bach *bach = sub->bach;
	size_t level = msc313_bach_sub_level_bytes(sub);
	size_t pos, thr_bytes;
	bool passed = false;

	if (sub->writer) {
		pos = sub->queued + level;
		if (pos >= sub->next_period) {
			sub->next_period += sub->period_bytes;
			passed = true;
		} else if (level >= sub->buf_bytes) {
			/* full and nobody reading: leave it to ALSA's overrun */
			regmap_field_write(sub->int_thr_en, 0);
			return true;
		}
		/* the level the writer reaches at the boundary */
		thr_bytes = sub->next_period - sub->queued;
		if (thr_bytes > sub->buf_bytes)
			thr_bytes = sub->buf_bytes;
		regmap_field_write(sub->overrun_thr, TO_MIU(bach, thr_bytes));
		regmap_field_write(sub->int_thr_en, 1);
		return passed;
	}

	pos = level > sub->queued ? sub->queued : sub->queued - level;
	if (pos >= sub->next_period) {
		sub->next_period += sub->period_bytes;
		passed = true;
	} else if (!level && sub->queued) {
		/* ran dry before the boundary: an underrun for ALSA to see */
		regmap_field_write(sub->int_thr_en, 0);
		return true;
	}
	/*
	 * The level left when the boundary is consumed (the interrupt fires
	 * at level <= threshold); a boundary beyond what has been queued so
	 * far means an interrupt when it runs dry.
	 */
	thr_bytes = sub->next_period > sub->queued ? 0 : sub->queued - sub->next_period;
	regmap_field_write(sub->underrun_thr, TO_MIU(bach, thr_bytes));
	regmap_field_write(sub->int_thr_en, 1);
	return passed;
}

static void msc313_bach_sub_disarm(struct msc313_bach_sub *sub)
{
	regmap_field_write(sub->int_thr_en, 0);
	regmap_field_write(sub->int_edge_en, 0);
	msc313_bach_sub_clear_irq(sub);
}

/* --- PCM --- */

static const struct snd_pcm_hardware msc313_bach_pcm_hardware = {
	.info			= SNDRV_PCM_INFO_MMAP |
				  SNDRV_PCM_INFO_MMAP_VALID |
				  SNDRV_PCM_INFO_INTERLEAVED,
	.formats		= SNDRV_PCM_FMTBIT_S16_LE,
	.rates			= SNDRV_PCM_RATE_8000_48000 |
				  SNDRV_PCM_RATE_12000 | SNDRV_PCM_RATE_24000,
	.rate_min		= 8000,
	.rate_max		= 48000,
	.channels_min		= 1,
	.channels_max		= 2,
	.buffer_bytes_max	= SZ_128K,
	.period_bytes_min	= 512,
	.period_bytes_max	= SZ_32K,
	.periods_min		= 2,
	.periods_max		= 128,
	.fifo_size		= 32,
};

/* The reader resamples from any of these; the writer only from four of them */
static const unsigned int msc313_bach_reader_rates[] = {
	8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000,
};

static const unsigned int msc313_bach_writer_rates[] = {
	8000, 16000, 32000, 48000,
};

static const struct snd_pcm_hw_constraint_list msc313_bach_writer_rate_list = {
	.count = ARRAY_SIZE(msc313_bach_writer_rates),
	.list = msc313_bach_writer_rates,
};

static struct msc313_bach_sub *msc313_bach_substream_sub(struct msc313_bach *bach,
							 struct snd_pcm_substream *substream)
{
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		return &bach->reader;
	return &bach->writer;
}

static int msc313_bach_pcm_construct(struct snd_soc_component *component,
				     struct snd_soc_pcm_runtime *rtd)
{
	snd_pcm_set_managed_buffer_all(rtd->pcm, SNDRV_DMA_TYPE_DEV,
				       component->dev,
				       msc313_bach_pcm_hardware.buffer_bytes_max,
				       msc313_bach_pcm_hardware.buffer_bytes_max);
	return 0;
}

static int msc313_bach_pcm_open(struct snd_soc_component *component,
				struct snd_pcm_substream *substream)
{
	struct msc313_bach *bach = msc313_bach_from_component(component);
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct msc313_bach_sub *sub = msc313_bach_substream_sub(bach, substream);
	unsigned long flags;
	int ret;

	snd_soc_set_runtime_hwparams(substream, &msc313_bach_pcm_hardware);

	/* The DMA works in aligned units; have ALSA pick sizes that fit */
	ret = snd_pcm_hw_constraint_step(runtime, 0, SNDRV_PCM_HW_PARAM_PERIOD_BYTES,
					 MSC313_BACH_ALIGNMENT);
	if (ret < 0)
		return ret;
	ret = snd_pcm_hw_constraint_step(runtime, 0, SNDRV_PCM_HW_PARAM_BUFFER_BYTES,
					 MSC313_BACH_ALIGNMENT);
	if (ret < 0)
		return ret;
	if (sub->writer) {
		ret = snd_pcm_hw_constraint_list(runtime, 0, SNDRV_PCM_HW_PARAM_RATE,
						 &msc313_bach_writer_rate_list);
		if (ret < 0)
			return ret;
	}

	spin_lock_irqsave(&bach->lock, flags);
	sub->substream = substream;
	sub->running = false;
	if (!bach->open_streams++)
		regmap_field_force_write(bach->dma_live_count_en, 1);
	spin_unlock_irqrestore(&bach->lock, flags);

	return 0;
}

static int msc313_bach_pcm_close(struct snd_soc_component *component,
				 struct snd_pcm_substream *substream)
{
	struct msc313_bach *bach = msc313_bach_from_component(component);
	struct msc313_bach_sub *sub = msc313_bach_substream_sub(bach, substream);
	unsigned long flags;

	spin_lock_irqsave(&bach->lock, flags);
	msc313_bach_sub_disarm(sub);
	sub->running = false;
	sub->substream = NULL;
	bach->open_streams--;
	spin_unlock_irqrestore(&bach->lock, flags);

	return 0;
}

static int msc313_bach_pcm_prepare(struct snd_soc_component *component,
				   struct snd_pcm_substream *substream)
{
	struct msc313_bach *bach = msc313_bach_from_component(component);
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct msc313_bach_sub *sub = msc313_bach_substream_sub(bach, substream);
	unsigned int mono = runtime->channels == 1;
	unsigned int miu_addr = TO_MIU(bach, runtime->dma_addr);
	unsigned long flags;
	int i, rate_sel = -1;

	if ((runtime->dma_addr % MSC313_BACH_ALIGNMENT) ||
	    (runtime->dma_bytes % MSC313_BACH_ALIGNMENT))
		return -EINVAL;

	if (sub->writer) {
		for (i = 0; i < ARRAY_SIZE(msc313_bach_writer_rates); i++)
			if (msc313_bach_writer_rates[i] == runtime->rate)
				rate_sel = i;
	} else {
		for (i = 0; i < ARRAY_SIZE(msc313_bach_reader_rates); i++)
			if (msc313_bach_reader_rates[i] == runtime->rate)
				rate_sel = i;
	}
	if (rate_sel < 0)
		return -EINVAL;

	spin_lock_irqsave(&bach->lock, flags);

	msc313_bach_sub_disarm(sub);
	regmap_field_write(sub->en, 0);
	sub->running = false;
	sub->buf_bytes = runtime->dma_bytes;
	sub->period_bytes = frames_to_bytes(runtime, runtime->period_size);
	sub->queued = 0;
	sub->pending = 0;
	sub->next_period = sub->period_bytes;
	sub->last_appl = 0;

	/* reset the level counter */
	regmap_field_force_write(sub->trigger, 0);
	regmap_field_force_write(sub->init, 1);
	regmap_field_force_write(sub->init, 0);

	regmap_field_write(sub->addr_hi, miu_addr >> 12);
	regmap_field_write(sub->addr_lo, miu_addr & 0xfff);
	regmap_field_write(sub->size, TO_MIU(bach, runtime->dma_bytes));
	regmap_field_write(sub->overrun_thr, 0);
	regmap_field_write(sub->underrun_thr, 0);

	regmap_field_write(sub->mono, mono);
	if (sub->mono2)
		regmap_field_write(sub->mono2, mono);
	regmap_field_write(sub->rate_sel, rate_sel);
	if (sub->rate_sel2)
		regmap_field_write(sub->rate_sel2, rate_sel);

	spin_unlock_irqrestore(&bach->lock, flags);

	return 0;
}

/*
 * Called without bach->lock: turn any passed boundaries into period ticks.
 * The trigger and ack callbacks run under the PCM stream lock, from where
 * snd_pcm_period_elapsed() would deadlock.
 */
static void msc313_bach_sub_tick(struct msc313_bach_sub *sub, bool passed, bool stream_locked)
{
	struct msc313_bach *bach = sub->bach;
	unsigned long flags;
	int guard = 64;

	while (passed && guard--) {
		if (stream_locked)
			snd_pcm_period_elapsed_under_stream_lock(sub->substream);
		else
			snd_pcm_period_elapsed(sub->substream);
		spin_lock_irqsave(&bach->lock, flags);
		passed = sub->running && msc313_bach_sub_arm(sub);
		spin_unlock_irqrestore(&bach->lock, flags);
	}
}

static int msc313_bach_pcm_trigger(struct snd_soc_component *component,
				   struct snd_pcm_substream *substream, int cmd)
{
	struct msc313_bach *bach = msc313_bach_from_component(component);
	struct msc313_bach_sub *sub = msc313_bach_substream_sub(bach, substream);
	unsigned long flags;
	bool passed = false;
	int ret = 0;

	spin_lock_irqsave(&bach->lock, flags);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		msc313_bach_sub_clear_irq(sub);
		if (!sub->writer) {
			/* the channel enable has to go right before the reader's */
			regmap_field_write(bach->dma_en, 1);
			udelay(10);
		}
		regmap_field_write(sub->en, 1);
		udelay(10);
		sub->running = true;
		/* whatever the application queued before starting */
		msc313_bach_sub_push_pending(sub);
		passed = msc313_bach_sub_arm(sub);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		msc313_bach_sub_disarm(sub);
		sub->running = false;
		regmap_field_write(sub->en, 0);
		udelay(10);
		if (!sub->writer)
			regmap_field_write(bach->dma_en, 0);
		break;
	default:
		ret = -EINVAL;
	}

	spin_unlock_irqrestore(&bach->lock, flags);

	if (!ret)
		msc313_bach_sub_tick(sub, passed, true);

	return ret;
}

static snd_pcm_uframes_t msc313_bach_pcm_pointer(struct snd_soc_component *component,
						 struct snd_pcm_substream *substream)
{
	struct msc313_bach *bach = msc313_bach_from_component(component);
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct msc313_bach_sub *sub = msc313_bach_substream_sub(bach, substream);
	unsigned long flags;
	size_t pos;

	spin_lock_irqsave(&bach->lock, flags);
	pos = msc313_bach_sub_position(sub);
	spin_unlock_irqrestore(&bach->lock, flags);

	return bytes_to_frames(runtime, pos % runtime->dma_bytes);
}

/*
 * The application moved its pointer: samples were written (playback) or
 * read (capture). Hand that many bytes to the hardware.
 */
static int msc313_bach_pcm_ack(struct snd_soc_component *component,
			       struct snd_pcm_substream *substream)
{
	struct msc313_bach *bach = msc313_bach_from_component(component);
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct msc313_bach_sub *sub = msc313_bach_substream_sub(bach, substream);
	snd_pcm_uframes_t appl = READ_ONCE(runtime->control->appl_ptr);
	snd_pcm_uframes_t delta;
	unsigned long flags;
	bool passed = false;

	spin_lock_irqsave(&bach->lock, flags);
	if (appl >= sub->last_appl)
		delta = appl - sub->last_appl;
	else
		delta = appl + runtime->boundary - sub->last_appl;
	sub->last_appl = appl;
	sub->pending += frames_to_bytes(runtime, delta);
	if (sub->running) {
		msc313_bach_sub_push_pending(sub);
		passed = msc313_bach_sub_arm(sub);
	}
	spin_unlock_irqrestore(&bach->lock, flags);

	msc313_bach_sub_tick(sub, passed, true);

	return 0;
}

static irqreturn_t msc313_bach_irq(int irq, void *data)
{
	struct msc313_bach *bach = data;
	struct msc313_bach_sub *subs[] = { &bach->reader, &bach->writer };
	irqreturn_t ret = IRQ_NONE;
	unsigned long flags;
	int i;

	for (i = 0; i < ARRAY_SIZE(subs); i++) {
		struct msc313_bach_sub *sub = subs[i];
		unsigned int en = 0;
		bool passed = false;

		spin_lock_irqsave(&bach->lock, flags);
		regmap_field_read(sub->int_thr_en, &en);
		if (en && sub->running && sub->substream) {
			ret = IRQ_HANDLED;
			msc313_bach_sub_clear_irq(sub);
			passed = msc313_bach_sub_arm(sub);
		}
		spin_unlock_irqrestore(&bach->lock, flags);

		if (passed)
			msc313_bach_sub_tick(sub, passed, false);
	}

	return ret;
}

static const struct snd_soc_component_driver msc313_bach_pcm_component = {
	.name		= "msc313-bach-pcm",
	.debugfs_prefix	= "pcm",
	.pcm_new	= msc313_bach_pcm_construct,
	.open		= msc313_bach_pcm_open,
	.close		= msc313_bach_pcm_close,
	.prepare	= msc313_bach_pcm_prepare,
	.trigger	= msc313_bach_pcm_trigger,
	.pointer	= msc313_bach_pcm_pointer,
	.ack		= msc313_bach_pcm_ack,
};

/* --- CPU DAI --- */

static struct snd_soc_dai_driver msc313_bach_cpu_dai_drv = {
	.name = "msc313-bach-cpu-dai",
	.playback = {
		.stream_name	= "CPU Playback",
		.channels_min	= 1,
		.channels_max	= 2,
		.rates		= SNDRV_PCM_RATE_8000_48000 |
				  SNDRV_PCM_RATE_12000 | SNDRV_PCM_RATE_24000,
		.formats	= SNDRV_PCM_FMTBIT_S16_LE,
	},
	.capture = {
		.stream_name	= "CPU Capture",
		.channels_min	= 1,
		.channels_max	= 2,
		.rates		= SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_16000 |
				  SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_48000,
		.formats	= SNDRV_PCM_FMTBIT_S16_LE,
	},
};

static const struct snd_soc_component_driver msc313_bach_cpu_component = {
	.name = "msc313-bach-cpu",
	.debugfs_prefix = "cpu",
};

/* --- Codec --- */

static struct snd_soc_dai_driver msc313_bach_codec_dai_drv = {
	.name = "Codec",
	.playback = {
		.stream_name	= "Main Playback",
		.channels_min	= 1,
		.channels_max	= 2,
		.rates		= SNDRV_PCM_RATE_8000_48000 |
				  SNDRV_PCM_RATE_12000 | SNDRV_PCM_RATE_24000,
		.formats	= SNDRV_PCM_FMTBIT_S16_LE,
	},
	.capture = {
		.stream_name	= "Main Capture",
		.channels_min	= 1,
		.channels_max	= 2,
		.rates		= SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_16000 |
				  SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_48000,
		.formats	= SNDRV_PCM_FMTBIT_S16_LE,
	},
};

static unsigned int msc313_bach_codec_read(struct snd_soc_component *component,
					   unsigned int reg)
{
	struct msc313_bach *bach = msc313_bach_from_component(component);
	unsigned int val;
	int ret;

	if (reg >= REG_ATOP_OFFSET)
		ret = regmap_read(bach->audiotop, reg - REG_ATOP_OFFSET, &val);
	else
		ret = regmap_read(bach->bach, reg, &val);

	return ret ? 0 : val;
}

static int msc313_bach_codec_write(struct snd_soc_component *component,
				   unsigned int reg, unsigned int value)
{
	struct msc313_bach *bach = msc313_bach_from_component(component);

	if (reg >= REG_ATOP_OFFSET)
		return regmap_write(bach->audiotop, reg - REG_ATOP_OFFSET, value);
	return regmap_write(bach->bach, reg, value);
}

/*
 * The DPGA gain fields are signed 8-bit in -0.5 dB steps: 0 is 0 dB,
 * 0x7e is -63 dB and 0x7f is mute. Only attenuation is offered.
 */
static const DECLARE_TLV_DB_SCALE(msc313_bach_dpga_tlv, -6350, 50, 1);

static const struct snd_kcontrol_new msc313_bach_controls[] = {
	SOC_DOUBLE_TLV("Main Playback Volume", REG_DPGA_PLAYBACK, 0, 8, 0x7f, 1,
		       msc313_bach_dpga_tlv),
	SOC_DOUBLE_TLV("Main Capture Volume", REG_DPGA_CAPTURE, 0, 8, 0x7f, 1,
		       msc313_bach_dpga_tlv),
	SOC_DOUBLE("ADC Capture Volume", REG_ATOP_ADC_GAIN, 0, 4, 7, 0),
	SOC_SINGLE("Mic Gain", REG_ATOP_MIC_GAIN, 4, 3, 0),
	/* the built-in test tone, see the "SineGen Switch" below */
	SOC_SINGLE("SineGen Gain", REG_SINEGEN, 4, 15, 0),
	SOC_SINGLE("SineGen Rate", REG_SINEGEN, 0, 15, 0),
};

/*
 * The sine generator is a DAPM switch rather than a plain control so that
 * turning it on powers the DAC and whatever the board has behind it, which
 * makes it a test of the analog side that needs no stream at all.
 */
static const struct snd_kcontrol_new msc313_bach_sinegen_switch =
	SOC_DAPM_SINGLE("Switch", REG_SINEGEN, 15, 1, 0);

static const char * const msc313_bach_output_select[] = { "ADC", "DMA Reader" };
static SOC_ENUM_SINGLE_DECL(msc313_bach_output_enum, REG_MUX0SEL, 5,
			    msc313_bach_output_select);
static const struct snd_kcontrol_new msc313_bach_output_mux =
	SOC_DAPM_ENUM("Playback Source", msc313_bach_output_enum);

static const char * const msc313_bach_adc_select[] = { "Line-in", "Mic-in" };
static const unsigned int msc313_bach_adc_values[] = {
	ATOP_ADC_MUX_LINEIN, ATOP_ADC_MUX_MICIN,
};
static SOC_VALUE_ENUM_SINGLE_DECL(msc313_bach_adc_enum, REG_ATOP_ADC_MUX, 0, 0xff,
				  msc313_bach_adc_select, msc313_bach_adc_values);
static const struct snd_kcontrol_new msc313_bach_adc_mux =
	SOC_DAPM_ENUM("ADC Source", msc313_bach_adc_enum);

static int msc313_bach_atop_power(struct snd_soc_dapm_widget *w, unsigned int mask, bool on)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);
	struct msc313_bach *bach = msc313_bach_from_component(component);

	return regmap_update_bits(bach->audiotop, REG_ATOP_ANALOG_CTRL3 - REG_ATOP_OFFSET,
				  mask, on ? 0 : mask);
}

static int msc313_bach_atop_event(struct snd_soc_dapm_widget *w,
				  struct snd_kcontrol *kcontrol, int event)
{
	return msc313_bach_atop_power(w, ATOP_PD_ALL, SND_SOC_DAPM_EVENT_ON(event));
}

static int msc313_bach_dac_event(struct snd_soc_dapm_widget *w,
				 struct snd_kcontrol *kcontrol, int event)
{
	int ret = msc313_bach_atop_power(w, ATOP_PD_DAC, SND_SOC_DAPM_EVENT_ON(event));

	/* the vendor gives the DAC a moment before letting an amplifier at it */
	if (!ret && SND_SOC_DAPM_EVENT_ON(event))
		msleep(10);
	return ret;
}

static int msc313_bach_adc_event(struct snd_soc_dapm_widget *w,
				 struct snd_kcontrol *kcontrol, int event)
{
	int ret = msc313_bach_atop_power(w, ATOP_PD_ADC_BIAS | ATOP_PD_ADC,
					 SND_SOC_DAPM_EVENT_ON(event));

	if (!ret && SND_SOC_DAPM_EVENT_ON(event))
		msleep(50);
	return ret;
}

static const struct snd_soc_dapm_widget msc313_bach_dapm_widgets[] = {
	SND_SOC_DAPM_SUPPLY("Analog Power", SND_SOC_NOPM, 0, 0, msc313_bach_atop_event,
			    SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_AIF_IN("DMARD", "Main Playback", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_MUX("Playback Mux", SND_SOC_NOPM, 0, 0, &msc313_bach_output_mux),
	SND_SOC_DAPM_SIGGEN("Sine Generator"),
	SND_SOC_DAPM_SWITCH("SineGen", SND_SOC_NOPM, 0, 0, &msc313_bach_sinegen_switch),
	SND_SOC_DAPM_DAC_E("DAC", NULL, SND_SOC_NOPM, 0, 0, msc313_bach_dac_event,
			   SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_OUTPUT("LINEOUT"),

	SND_SOC_DAPM_INPUT("LINEIN"),
	SND_SOC_DAPM_INPUT("MICIN"),
	SND_SOC_DAPM_MUX("ADC Mux", SND_SOC_NOPM, 0, 0, &msc313_bach_adc_mux),
	SND_SOC_DAPM_ADC_E("ADC", NULL, SND_SOC_NOPM, 0, 0, msc313_bach_adc_event,
			   SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_AIF_OUT("DMAWR", "Main Capture", 0, SND_SOC_NOPM, 0, 0),
};

static const struct snd_soc_dapm_route msc313_bach_dapm_routes[] = {
	{ "Playback Mux", "DMA Reader", "DMARD" },
	{ "Playback Mux", "ADC", "ADC" },
	{ "DAC", NULL, "Playback Mux" },
	{ "SineGen", "Switch", "Sine Generator" },
	{ "DAC", NULL, "SineGen" },
	{ "DAC", NULL, "Analog Power" },
	{ "LINEOUT", NULL, "DAC" },

	{ "ADC Mux", "Line-in", "LINEIN" },
	{ "ADC Mux", "Mic-in", "MICIN" },
	{ "ADC", NULL, "ADC Mux" },
	{ "ADC", NULL, "Analog Power" },
	{ "DMAWR", NULL, "ADC" },
};

static const struct snd_soc_component_driver msc313_bach_codec_drv = {
	.name			= "msc313-bach-codec",
	.debugfs_prefix		= "codec",
	.write			= msc313_bach_codec_write,
	.read			= msc313_bach_codec_read,
	.controls		= msc313_bach_controls,
	.num_controls		= ARRAY_SIZE(msc313_bach_controls),
	.dapm_widgets		= msc313_bach_dapm_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(msc313_bach_dapm_widgets),
	.dapm_routes		= msc313_bach_dapm_routes,
	.num_dapm_routes	= ARRAY_SIZE(msc313_bach_dapm_routes),
	.idle_bias_on		= 1,
	.use_pmdown_time	= 1,
	.endianness		= 1,
};

/* --- Card: the board's speaker amplifier and headphone jack --- */

static int msc313_bach_spk_event(struct snd_soc_dapm_widget *w,
				 struct snd_kcontrol *kcontrol, int event)
{
	struct msc313_bach *bach = snd_soc_card_get_drvdata(snd_soc_dapm_to_card(w->dapm));

	if (bach->amp_gpio)
		gpiod_set_value_cansleep(bach->amp_gpio, SND_SOC_DAPM_EVENT_ON(event));
	return 0;
}

static const struct snd_soc_dapm_widget msc313_bach_card_widgets[] = {
	SND_SOC_DAPM_SPK("Speaker", msc313_bach_spk_event),
	SND_SOC_DAPM_HP("Headphone", NULL),
	SND_SOC_DAPM_MIC("Mic", NULL),
	SND_SOC_DAPM_LINE("Line In", NULL),
};

static const struct snd_soc_dapm_route msc313_bach_card_routes[] = {
	{ "Speaker", NULL, "LINEOUT" },
	{ "Headphone", NULL, "LINEOUT" },
	{ "MICIN", NULL, "Mic" },
	{ "LINEIN", NULL, "Line In" },
};

static struct snd_soc_jack_pin msc313_bach_jack_pins[] = {
	{ .pin = "Headphone", .mask = SND_JACK_HEADPHONE },
	{ .pin = "Speaker", .mask = SND_JACK_HEADPHONE, .invert = true },
};

static int msc313_bach_card_late_probe(struct snd_soc_card *card)
{
	struct msc313_bach *bach = snd_soc_card_get_drvdata(card);
	int ret;

	if (!of_property_present(card->dev->of_node, "hp-det-gpios"))
		return 0;

	ret = snd_soc_card_jack_new_pins(card, "Headphone Jack", SND_JACK_HEADPHONE,
					 &bach->hp_jack, msc313_bach_jack_pins,
					 ARRAY_SIZE(msc313_bach_jack_pins));
	if (ret)
		return ret;

	bach->hp_jack_gpio.name = "hp-det";
	bach->hp_jack_gpio.report = SND_JACK_HEADPHONE;
	bach->hp_jack_gpio.debounce_time = 150;
	return snd_soc_jack_add_gpiods(card->dev, &bach->hp_jack, 1, &bach->hp_jack_gpio);
}

/* --- Probe --- */

static const struct regmap_config msc313_bach_regmap_config = {
	.name = "bach",
	.reg_bits = 16,
	.val_bits = 16,
	.reg_stride = 4,
};

static struct regmap_field *msc313_bach_field(struct device *dev, struct regmap *map,
					      unsigned int reg, unsigned int lsb,
					      unsigned int msb, int *err)
{
	struct reg_field field = REG_FIELD(reg, lsb, msb);
	struct regmap_field *f = devm_regmap_field_alloc(dev, map, field);

	if (IS_ERR(f) && !*err)
		*err = PTR_ERR(f);
	return f;
}

static int msc313_bach_sub_init(struct msc313_bach *bach, struct msc313_bach_sub *sub,
				bool writer)
{
	struct device *dev = bach->dev;
	struct regmap *map = bach->bach;
	unsigned int base = writer ? REG_DMA_WR : REG_DMA_RD;
	int err = 0;

	sub->bach = bach;
	sub->writer = writer;

	sub->addr_lo = msc313_bach_field(dev, map, base + SUB_CTRL, 0, 11, &err);
	sub->count = msc313_bach_field(dev, map, base + SUB_CTRL, 12, 12, &err);
	sub->trigger = msc313_bach_field(dev, map, base + SUB_CTRL, 13, 13, &err);
	sub->init = msc313_bach_field(dev, map, base + SUB_CTRL, 14, 14, &err);
	sub->en = msc313_bach_field(dev, map, base + SUB_CTRL, 15, 15, &err);
	sub->addr_hi = msc313_bach_field(dev, map, base + SUB_ADDR_HI, 0, 14, &err);
	sub->size = msc313_bach_field(dev, map, base + SUB_SIZE, 0, 15, &err);
	sub->trigger_level = msc313_bach_field(dev, map, base + SUB_TRIGGER, 0, 15, &err);
	sub->overrun_thr = msc313_bach_field(dev, map, base + SUB_OVERRUN_THR, 0, 15, &err);
	sub->underrun_thr = msc313_bach_field(dev, map, base + SUB_UNDERRUN_THR, 0, 15, &err);
	sub->level = msc313_bach_field(dev, map, base + SUB_LEVEL, 0, 15, &err);

	if (writer) {
		sub->int_clear = msc313_bach_field(dev, map, REG_DMA_CTRL, 9, 9, &err);
		sub->int_edge_en = msc313_bach_field(dev, map, REG_DMA_CTRL, 11, 11, &err);
		sub->int_thr_en = msc313_bach_field(dev, map, REG_DMA_CTRL, 14, 14, &err);
		sub->flag_thr = msc313_bach_field(dev, map, REG_DMA_FLAGS, 1, 1, &err);
		sub->flag_edge = msc313_bach_field(dev, map, REG_DMA_FLAGS, 5, 5, &err);
		sub->mono = msc313_bach_field(dev, map, REG_DMA_TEST_CTRL7, 14, 14, &err);
		sub->rate_sel = msc313_bach_field(dev, map, REG_SR_SEL, 8, 9, &err);
		sub->rate_sel2 = msc313_bach_field(dev, map, REG_SR_SEL, 10, 11, &err);
	} else {
		sub->int_clear = msc313_bach_field(dev, map, REG_DMA_CTRL, 8, 8, &err);
		sub->int_edge_en = msc313_bach_field(dev, map, REG_DMA_CTRL, 10, 10, &err);
		sub->int_thr_en = msc313_bach_field(dev, map, REG_DMA_CTRL, 13, 13, &err);
		sub->flag_thr = msc313_bach_field(dev, map, REG_DMA_FLAGS, 2, 2, &err);
		sub->flag_edge = msc313_bach_field(dev, map, REG_DMA_FLAGS, 4, 4, &err);
		sub->mono = msc313_bach_field(dev, map, REG_DMA_TEST_CTRL7, 15, 15, &err);
		sub->mono2 = msc313_bach_field(dev, map, REG_DMA_TEST_CTRL7, 13, 13, &err);
		sub->rate_sel = msc313_bach_field(dev, map, REG_SR_SEL, 4, 7, &err);
	}

	if (!err) {
		regmap_field_write(sub->en, 0);
		msc313_bach_sub_disarm(sub);
	}

	return err;
}

static int msc313_bach_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct snd_soc_dai_link *link;
	struct snd_soc_card *card;
	struct msc313_bach *bach;
	void __iomem *base;
	int ret, irq, err = 0;

	bach = devm_kzalloc(dev, sizeof(*bach), GFP_KERNEL);
	if (!bach)
		return -ENOMEM;

	bach->dev = dev;
	bach->data = device_get_match_data(dev);
	if (!bach->data)
		return -EINVAL;
	spin_lock_init(&bach->lock);

	bach->clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(bach->clk))
		return dev_err_probe(dev, PTR_ERR(bach->clk), "no clock\n");

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	bach->bach = devm_regmap_init_mmio(dev, base, &msc313_bach_regmap_config);
	if (IS_ERR(bach->bach))
		return PTR_ERR(bach->bach);

	bach->audiotop = syscon_regmap_lookup_by_phandle(dev->of_node, "mstar,audiotop");
	if (IS_ERR(bach->audiotop))
		return dev_err_probe(dev, PTR_ERR(bach->audiotop), "no audiotop\n");

	bach->amp_gpio = devm_gpiod_get_optional(dev, "amp", GPIOD_OUT_LOW);
	if (IS_ERR(bach->amp_gpio))
		return dev_err_probe(dev, PTR_ERR(bach->amp_gpio), "amp gpio\n");

	bach->dma_rst = msc313_bach_field(dev, bach->bach, REG_DMA_CTRL, 0, 0, &err);
	bach->dma_en = msc313_bach_field(dev, bach->bach, REG_DMA_CTRL, 1, 1, &err);
	bach->dma_live_count_en = msc313_bach_field(dev, bach->bach, REG_DMA_CTRL, 2, 2, &err);
	bach->dma_int_en = msc313_bach_field(dev, bach->bach, REG_DMA_INT, 1, 1, &err);
	if (err)
		return err;

	ret = regmap_multi_reg_write(bach->audiotop, msc313_bach_atop_init,
				     ARRAY_SIZE(msc313_bach_atop_init));
	if (ret)
		return ret;
	ret = regmap_multi_reg_write(bach->bach, msc313_bach_init,
				     ARRAY_SIZE(msc313_bach_init));
	if (ret)
		return ret;

	/* reset the DMA engine once, then leave it to the streams */
	regmap_field_force_write(bach->dma_rst, 1);
	udelay(10);
	regmap_field_force_write(bach->dma_rst, 0);
	udelay(10);
	regmap_field_write(bach->dma_en, 0);

	ret = msc313_bach_sub_init(bach, &bach->reader, false);
	if (ret)
		return ret;
	ret = msc313_bach_sub_init(bach, &bach->writer, true);
	if (ret)
		return ret;

	irq = irq_of_parse_and_map(dev->of_node, 0);
	if (!irq)
		return -EINVAL;
	ret = devm_request_irq(dev, irq, msc313_bach_irq, IRQF_SHARED, dev_name(dev), bach);
	if (ret)
		return ret;
	regmap_field_write(bach->dma_int_en, 1);

	ret = devm_snd_soc_register_component(dev, &msc313_bach_codec_drv,
					      &msc313_bach_codec_dai_drv, 1);
	if (ret)
		return ret;
	ret = devm_snd_soc_register_component(dev, &msc313_bach_cpu_component,
					      &msc313_bach_cpu_dai_drv, 1);
	if (ret)
		return ret;
	ret = devm_snd_soc_register_component(dev, &msc313_bach_pcm_component, NULL, 0);
	if (ret)
		return ret;

	link = &bach->dai_link;
	link->cpus = &bach->cpu_dai_component;
	link->codecs = &bach->codec_component;
	link->platforms = &bach->platform_component;
	link->num_cpus = 1;
	link->num_codecs = 1;
	link->num_platforms = 1;
	link->name = "cdc";
	link->stream_name = "CDC PCM";
	link->cpus->dai_name = "msc313-bach-cpu-dai";
	link->codecs->dai_name = "Codec";
	link->codecs->name = dev_name(dev);
	link->platforms->name = dev_name(dev);

	card = &bach->card;
	card->dev = dev;
	card->owner = THIS_MODULE;
	card->name = DRIVER_NAME;
	card->dai_link = link;
	card->num_links = 1;
	card->dapm_widgets = msc313_bach_card_widgets;
	card->num_dapm_widgets = ARRAY_SIZE(msc313_bach_card_widgets);
	card->dapm_routes = msc313_bach_card_routes;
	card->num_dapm_routes = ARRAY_SIZE(msc313_bach_card_routes);
	card->late_probe = msc313_bach_card_late_probe;
	card->fully_routed = true;
	snd_soc_card_set_drvdata(card, bach);

	ret = snd_soc_of_parse_aux_devs(card, "audio-aux-devs");
	if (ret)
		return ret;

	return devm_snd_soc_register_card(dev, card);
}

static const struct msc313_bach_data msc313_data = {
	.addr_sz_shift = 3,
};

static const struct msc313_bach_data ssd210_data = {
	.addr_sz_shift = 4,
};

static const struct of_device_id msc313_bach_of_match[] = {
	{ .compatible = "mstar,msc313-bach", .data = &msc313_data },
	{ .compatible = "mstar,ssd210-bach", .data = &ssd210_data },
	{ },
};
MODULE_DEVICE_TABLE(of, msc313_bach_of_match);

static struct platform_driver msc313_bach_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.pm = &snd_soc_pm_ops,
		.of_match_table = msc313_bach_of_match,
	},
	.probe = msc313_bach_probe,
};
module_platform_driver(msc313_bach_driver);

MODULE_AUTHOR("Daniel Palmer <daniel@thingy.jp>");
MODULE_DESCRIPTION("MStar MSC313 BACH sound");
MODULE_LICENSE("GPL v2");
