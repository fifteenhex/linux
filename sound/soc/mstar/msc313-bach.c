// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021 Daniel Palmer <daniel@thingy.jp>
 */

#include <linux/clk.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>

#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/jack.h>
#include <sound/pcm_params.h>
#include <sound/initval.h>
#include <sound/tlv.h>
#include <sound/dmaengine_pcm.h>

#include <linux/mfd/syscon.h>

#include "infinity.h"

#define DRIVER_NAME "msc313-bach"

struct msc313_bach_dma_sub_channel {
	struct regmap_field *overrunthreshold;
};

struct msc313_bach_dma_channel {
	struct regmap_field *rst;
	struct regmap_field *en;
	struct regmap_field *rd_underrun_int_clear;
	struct regmap_field *rd_underrun_int_en;

	struct msc313_bach_dma_sub_channel reader_writer[2];
};

struct msc313_bach {
	struct clk *clk;

	struct snd_soc_dai_link_component cpu_dai_component;
	struct snd_soc_dai_link_component platform_component;
	struct snd_soc_dai_link_component codec_component;
	struct snd_soc_dai_link dai_link;
	struct snd_soc_card card;

	struct regmap *audiotop;
	struct regmap *bach;

	/* DMA */
	struct msc313_bach_dma_channel dma_channels[1];
};

/* Bank 1 */
#define REG_MUX0SEL	0xc
#define REG_SINEGEN	0x1d4
/* Bank 2 */
#define REG_DMA_INT	0x21c
#define REG_DMA_INT_EN	BIT(1)

/* Audio top */
#define REG_ATOP_OFFSET 0x1000
#define REG_ATOP_ANALOG_CTRL0	(REG_ATOP_OFFSET + 0)
#define REG_ATOP_ANALOG_CTRL1	(REG_ATOP_OFFSET + 0x4)
#define REG_ATOP_ANALOG_CTRL3	(REG_ATOP_OFFSET + 0xc)

/* cpu dai */
struct snd_soc_dai_driver msc313_bach_cpu_dai_drv = {
	.name = "msc313-bach-cpu-dai",
	.playback =
	{
		.channels_min	= 1,
		.channels_max	= 2,
		.rates		= SNDRV_PCM_RATE_8000_48000,
		.formats	= SNDRV_PCM_FMTBIT_S16_LE
	},
	.capture =
	{
		.channels_min	= 1,
		.channels_max	= 2,
		.rates		= SNDRV_PCM_RATE_8000_48000,
		.formats	= SNDRV_PCM_FMTBIT_S16_LE,
	},
};

static const struct snd_soc_component_driver msc313_bach_cpu_component = {
	.name = "msc313-bach",
};
/* cpu dai */

/* codec */
/**
* Used to set Playback volume 0~76 (mapping to -64dB~12dB)
*/
#define MAIN_PLAYBACK_VOLUME "Main Playback Volume"

/**
* Used to set Capture volume 0~76 (mapping to -64dB~12dB)
*/
#define MAIN_CAPTURE_VOLUME "Main Capture Volume"

/**
* Used to set microphone gain, total 5 bits ,
* it consists of the upper 2 bits(4 levels) + the lower 3 bits (8 levels)
*/
#define MIC_GAIN_SELECTION "Mic Gain Selection"

/**
* Used to set line-in gain level 0~7
*/
#define LINEIN_GAIN_LEVEL "LineIn Gain Level"

/**
* Used to select ADC input (Line-in, Mic-in)
*/
#define ADC_MUX "ADC Mux"

enum {
	AUD_PLAYBACK_MUX = 0,
	AUD_ADC_MUX,    //0x1
	AUD_ATOP_PWR,   //0x2
	AUD_DPGA_PWR,   //0x3
	AUD_PLAYBACK_DPGA,
	AUD_CAPTURE_DPGA,
	AUD_MIC_GAIN,
	AUD_LINEIN_GAIN,
	AUD_DIGMIC_PWR,
	AUD_DBG_SINERATE,
	AUD_DBG_SINEGAIN,
	AUD_REG_LEN,
};

struct snd_soc_dai_driver msc313_bach_codec_dai_drv = {
	.name	= "Codec",
	.playback =
	{
		.stream_name	= "Main Playback",
		.channels_min	= 1,
		.channels_max	= 2,
		.rates		= SNDRV_PCM_RATE_8000_48000,
		.formats	= SNDRV_PCM_FMTBIT_S16_LE |
				  SNDRV_PCM_FMTBIT_S24_LE |
				  SNDRV_PCM_FMTBIT_S32_LE,
	},
	.capture =
	{
		.stream_name	= "Main Capture",
		.channels_min	= 1,
		.channels_max	= 2,
		.rates		= SNDRV_PCM_RATE_8000_48000,
		.formats	= SNDRV_PCM_FMTBIT_S16_LE,
	},
};

static unsigned int msc313_bach_codec_read(struct snd_soc_component *component, unsigned int reg)
{
	struct msc313_bach *bach = snd_soc_card_get_drvdata(component->card);
	unsigned int val;
	int ret;

	if(reg >= REG_ATOP_OFFSET)
		ret = regmap_read(bach->audiotop, reg - REG_ATOP_OFFSET, &val);
	else
		ret = regmap_read(bach->bach, reg, &val);

	if(ret)
		return ret;

	return val;
}

static int msc313_bach_codec_write(struct snd_soc_component *component,
		unsigned int reg, unsigned int value)
{
	struct msc313_bach *bach = snd_soc_card_get_drvdata(component->card);
	int ret;

	if(reg >= REG_ATOP_OFFSET)
		ret = regmap_write(bach->audiotop, reg - REG_ATOP_OFFSET, value);
	else
		ret = regmap_write(bach->bach, reg, value);

	return ret;
}

static const unsigned int infinity_dpga_tlv[] =
{
	TLV_DB_RANGE_HEAD(1), 0, 76, TLV_DB_LINEAR_ITEM(-64, 12),
};

static const struct snd_kcontrol_new msc313_bach_controls[] =
{
	//SOC_SINGLE_TLV(MAIN_PLAYBACK_VOLUME, AUD_PLAYBACK_DPGA, 0, 76, 0, infinity_dpga_tlv),
	//SOC_SINGLE_TLV(MAIN_CAPTURE_VOLUME, AUD_CAPTURE_DPGA, 0, 76, 0, infinity_dpga_tlv),
	//SOC_SINGLE_TLV(MIC_GAIN_SELECTION, AUD_MIC_GAIN, 0, 31, 0, NULL),
	//SOC_SINGLE_TLV(LINEIN_GAIN_LEVEL, AUD_LINEIN_GAIN, 0, 7, 0, NULL),

	/* sinegen */
	SOC_SINGLE("SineGen Enable", REG_SINEGEN, 15, 1, 0),
	SOC_SINGLE("SineGen Gain Level", REG_SINEGEN, 4, 15, 0),
	SOC_SINGLE("SineGen Rate Select", REG_SINEGEN, 0, 15, 0),
};

static const char *infinity_adc_select[] = {
	"Line-in",
	"Mic-in",
};

/* Main output mux control */
static const char *msc313_bach_output_select[] = {
	"ADC In",
	"DMA Reader",
};

static const struct soc_enum msc313_bach_outsel_enum =
		SOC_ENUM_SINGLE(REG_MUX0SEL, 5, 1, msc313_bach_output_select);

static const struct snd_kcontrol_new msc313_bach_output_mux_controls =
	SOC_DAPM_ENUM("Playback Select", msc313_bach_outsel_enum);

//static const struct soc_enum infinity_adcsel_enum =
//		SOC_ENUM_SINGLE(AUD_ADC_MUX, 0, 2, infinity_adc_select);
//static const struct snd_kcontrol_new infinity_adc_mux_controls =
//		SOC_DAPM_ENUM("ADC Select", infinity_adcsel_enum);

static const struct snd_soc_dapm_widget infinity_dapm_widgets[] =
{
	/* top bits */
	//SND_SOC_DAPM_DAC("DAC",   NULL, REG_ATOP_ANALOG_CTRL1, 1, 0),
	SND_SOC_DAPM_ADC("ADC", NULL, REG_ATOP_ANALOG_CTRL1, 0, 0),
	SND_SOC_DAPM_INPUT("DMARD"),
	SND_SOC_DAPM_AIF_IN("DMARD",  "Main Playback", 0, SND_SOC_NOPM, 0, 0),




	//SND_SOC_DAPM_AIF_OUT("DMAWR1", "Sub Capture",  0, SND_SOC_NOPM, 0, 0),
	//SND_SOC_DAPM_AIF_OUT("DMAWR", "Main Capture",   0, SND_SOC_NOPM, 0, 0),
	//SND_SOC_DAPM_AIF_IN("DMARD2",  "Sub Playback",  0, SND_SOC_NOPM, 0, 0),
	//SND_SOC_DAPM_AIF_IN("DIGMIC", NULL,   0, AUD_DIGMIC_PWR, 0, 0),


	//SND_SOC_DAPM_INPUT("LINEIN"),
	//SND_SOC_DAPM_INPUT("MICIN"),


	//SND_SOC_DAPM_ADC("Mic Bias", NULL, AUD_ATOP_PWR, 2, 0),
	//SND_SOC_DAPM_MICBIAS("Mic Bias", AUD_ATOP_PWR, 5, 0),

	//SND_SOC_DAPM_PGA("Main Playback DPGA", AUD_DPGA_PWR, 0, 0, NULL, 0),
	//SND_SOC_DAPM_PGA("Main Capture DPGA",  AUD_DPGA_PWR, 1, 0, NULL, 0),

	//SND_SOC_DAPM_MUX(ADC_MUX, SND_SOC_NOPM, 0, 0, &infinity_adc_mux_controls),

	/* Line out */
	SND_SOC_DAPM_MUX("Output Mux", SND_SOC_NOPM, 0, 0,
			&msc313_bach_output_mux_controls),
	SND_SOC_DAPM_OUTPUT("LINEOUT"),
};

#define MAIN_PLAYBACK_MUX "Main Playback Mux"

static const struct snd_soc_dapm_route infinity_codec_routes[] =
{
	{"Output Mux", NULL, "DMARD"},
	{"Output Mux", NULL, "ADC"},
//	{"Main Playback DPGA", NULL, "Main Playback Mux"},

//	{"DAC", NULL, "Main Playback DPGA"},
//	{"LINEOUT", NULL, "DAC"},

//	{"DMAWR", NULL, "Main Capture DPGA"},
//	{"Main Capture DPGA", NULL, "ADC"},

//	{"ADC",     NULL, "ADC Mux"},
//	{"ADC Mux", "Mic-in",  "MICIN"}
};

static const struct snd_soc_component_driver msc313_bach_codec_drv = {
	.write			= msc313_bach_codec_write,
	.read			= msc313_bach_codec_read,
	.controls		= msc313_bach_controls,
	.num_controls		= ARRAY_SIZE(msc313_bach_controls),
	.dapm_widgets		= infinity_dapm_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(infinity_dapm_widgets),
	.dapm_routes		= infinity_codec_routes,
	.num_dapm_routes	= ARRAY_SIZE(infinity_codec_routes),
};
/* codec */

/* pcm */
int msc313_bach_pcm_construct(struct snd_soc_component *component,
		struct snd_soc_pcm_runtime *rtd)
{
	size_t size = 4 * 0x8000;
	struct snd_card *card = rtd->card->snd_card;

	printk("%s:%d\n", __func__, __LINE__);

	snd_pcm_set_managed_buffer_all(rtd->pcm, SNDRV_DMA_TYPE_DEV,
				       card->dev, size, size);

	return 0;
}

static const struct snd_pcm_hardware msc313_bach_pcm_playback_hardware =
{
	.info			= SNDRV_PCM_INFO_MMAP |
				  SNDRV_PCM_INFO_MMAP_VALID |
				  SNDRV_PCM_INFO_INTERLEAVED,
	.formats		= SNDRV_PCM_FMTBIT_S16_LE |
				  SNDRV_PCM_FMTBIT_S24_LE |
				  SNDRV_PCM_FMTBIT_S32_LE,
	.rates			= SNDRV_PCM_RATE_8000_48000,
	.rate_min		= 8000,
	.rate_max		= 48000,
	.channels_min		= 1,
	.channels_max		= 2,
	.buffer_bytes_max	= 96 * 1024,
	.period_bytes_min	= 8 * 1024,
	.period_bytes_max	= 24 * 1024,
	.periods_min		= 4,
	.periods_max		= 8,
	.fifo_size		= 32,

	.buffer_bytes_max = 4 * 0x8000,
};

static const struct snd_pcm_hardware msc313_bach_pcm_capture_hardware =
{
	.info			= SNDRV_PCM_INFO_INTERLEAVED |
				  SNDRV_PCM_INFO_MMAP |
				  SNDRV_PCM_INFO_MMAP_VALID,
	.formats		= SNDRV_PCM_FMTBIT_S16_LE,
	.rates			= SNDRV_PCM_RATE_8000_48000,
	.rate_min		= 8000,
	.rate_max		= 48000,
	.channels_min		= 1,
	.channels_max		= 2,
	.buffer_bytes_max	= 80 * 1024,
	.period_bytes_min	= 1 * 1024,
	.period_bytes_max	= 10 * 1024,
	.periods_min		= 4,
	.periods_max		= 12,
	.fifo_size		= 32,

	.buffer_bytes_max = 4 * 0x8000,
};

#define PERIOD_BYTES_MIN 0x100

static int msc313_bach_pcm_open(struct snd_soc_component *component,
		struct snd_pcm_substream *substream)
{
	struct msc313_bach *bach = snd_soc_card_get_drvdata(component->card);
	struct snd_pcm_runtime *runtime = substream->runtime;
	int ret;

	printk("%s:%d\n", __func__, __LINE__);

	switch(substream->stream){
	case SNDRV_PCM_STREAM_PLAYBACK:
		snd_soc_set_runtime_hwparams(substream, &msc313_bach_pcm_playback_hardware);
		break;
	case SNDRV_PCM_STREAM_CAPTURE:
		snd_soc_set_runtime_hwparams(substream, &msc313_bach_pcm_capture_hardware);
		break;
	default:
		return -EINVAL;
	}

	regmap_field_write(bach->dma_channels[0].rst, 0);

	return 0;
}

static int msc313_bach_pcm_close(struct snd_soc_component *component,
		struct snd_pcm_substream *substream)
{
	struct msc313_bach *bach = snd_soc_card_get_drvdata(component->card);
	struct snd_pcm_runtime *runtime = substream->runtime;
	int ret;

	printk("%s:%d\n", __func__, __LINE__);

	regmap_field_write(bach->dma_channels[0].rst, 1);

	return 0;
}

static int msc313_bach_pcm_trigger(struct snd_soc_component *component,
		struct snd_pcm_substream *substream, int cmd)
{
	struct msc313_bach *bach = snd_soc_card_get_drvdata(component->card);
	int ret = 0;

	printk("%s:%d\n", __func__, __LINE__);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		//~dgp disabled this for now because it causes an interrupt storm
		//regmap_field_write(bach->dma_channels[0].rd_underrun_int_en, 1);
		regmap_field_write(bach->dma_channels[0].en, 1);
		break;

	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		//~dgp disabled this for now because it causes an interrupt storm
		//regmap_field_write(bach->dma_channels[0].rd_underrun_int_en, 0);
		regmap_field_write(bach->dma_channels[0].en, 0);
		break;
	default:
		ret = -EINVAL;
	}

	return 0;
}

static snd_pcm_uframes_t msc313_bach_pcm_pointer(struct snd_soc_component *component,
		struct snd_pcm_substream *substream)
{
	printk("%s:%d\n", __func__, __LINE__);

	return 0;
}

static const struct snd_soc_component_driver msc313_soc_pcm_drv = {
	.pcm_construct	= msc313_bach_pcm_construct,
	.open		= msc313_bach_pcm_open,
	.trigger	= msc313_bach_pcm_trigger,
	.pointer	= msc313_bach_pcm_pointer,
	.close		= msc313_bach_pcm_close,
};
/* pcm */

static const struct regmap_config msc313_bach_regmap_config = {
	.reg_bits = 16,
	.val_bits = 16,
	.reg_stride = 4,
};

static irqreturn_t msc313_bach_irq(int irq, void *data)
{
	struct msc313_bach *bach = data;
	int i;

	//printk("bach irq\n");

	for (i = 0; i < ARRAY_SIZE(bach->dma_channels); i++)
		regmap_field_force_write(bach->dma_channels[i].rd_underrun_int_clear, 1);

	return IRQ_HANDLED;
}

static int msc313_bach_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *device_node;
	struct snd_soc_dai_link *link;
	struct dma_device *dma_dev;
	struct snd_soc_card *card;
	struct msc313_bach *bach;
	void __iomem *base;
	int i, j, ret, irq;

	/* Get the resources we need to probe the components */
	bach = devm_kzalloc(dev, sizeof(*bach), GFP_KERNEL);
	if(IS_ERR(bach))
		return PTR_ERR(bach);

	bach->clk = devm_clk_get(&pdev->dev, NULL);
	clk_prepare_enable(bach->clk);

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	bach->bach = devm_regmap_init_mmio(dev, base, &msc313_bach_regmap_config);
	if (IS_ERR(bach->bach))
		return PTR_ERR(bach->bach);

	bach->audiotop = syscon_regmap_lookup_by_phandle(dev->of_node, "mstar,audiotop");
	if(IS_ERR(bach->audiotop))
		return PTR_ERR(bach->audiotop);

	for (i = 0; i < ARRAY_SIZE(bach->dma_channels); i++){
		struct msc313_bach_dma_channel *chan = &bach->dma_channels[i];
		unsigned int chan_offset = 0x100 + (0x40 * i);
		struct reg_field chan_rst_field = REG_FIELD(chan_offset + 0x0, 0, 0);
		struct reg_field chan_en_field = REG_FIELD(chan_offset + 0x0, 1, 1);
		struct reg_field chan_rd_underrun_int_clear_field = REG_FIELD(chan_offset + 0x0, 8, 8);
		struct reg_field chan_rd_underrun_int_en_field = REG_FIELD(chan_offset + 0x0, 13, 13);

		chan->rst = devm_regmap_field_alloc(dev, bach->bach, chan_rst_field);
		chan->en = devm_regmap_field_alloc(dev, bach->bach, chan_en_field);
		chan->rd_underrun_int_clear = devm_regmap_field_alloc(dev, bach->bach, chan_rd_underrun_int_clear_field);
		chan->rd_underrun_int_en = devm_regmap_field_alloc(dev, bach->bach, chan_rd_underrun_int_en_field);

		for (j = 0; j < ARRAY_SIZE(chan->reader_writer); j++){
			struct msc313_bach_dma_sub_channel *sub = &chan->reader_writer[j];
			unsigned int sub_chan_offset = chan_offset + 4 + (0x20 * j);
			struct reg_field sub_chan_overrunthreshold_field = REG_FIELD(sub_chan_offset + 0x10, 0, 15);
			struct reg_field sub_chan_underrunthreshold_field = REG_FIELD(sub_chan_offset + 0x14, 0, 15);
		}

		regmap_field_write(chan->rst, 1);
	}

	/* probe the components */
	ret = devm_snd_soc_register_component(dev,
			&msc313_bach_codec_drv,
			&msc313_bach_codec_dai_drv,
			1);
	if(ret)
		return ret;

	ret = devm_snd_soc_register_component(dev,
			&msc313_bach_cpu_component,
			&msc313_bach_cpu_dai_drv,
			1);
	if(ret)
		return ret;

	ret = devm_snd_soc_register_component(dev,
			&msc313_soc_pcm_drv,
			NULL,
			0);
	if(ret)
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
	link->codecs->dai_name = "Codec";
	link->cpus->dai_name = dev_name(dev);
	link->codecs->name = dev_name(dev);
	link->platforms->name = dev_name(dev);

	card = &bach->card;
	card->dev = dev;
	card->owner = THIS_MODULE;
	card->name = DRIVER_NAME;
	card->dai_link = link;
	card->num_links = 1;
	card->fully_routed = true;

	snd_soc_card_set_drvdata(&bach->card, bach);

	ret = snd_soc_of_parse_aux_devs(&bach->card, "audio-aux-devs");
	if(ret)
		return ret;

	ret = devm_snd_soc_register_card(dev, &bach->card);
	if(ret)
		return ret;

	irq = irq_of_parse_and_map(pdev->dev.of_node, 0);
	if (!irq)
		return -EINVAL;
	ret = devm_request_irq(&pdev->dev, irq, msc313_bach_irq, IRQF_SHARED,
		dev_name(&pdev->dev), bach);

	regmap_update_bits(bach->bach, REG_DMA_INT, REG_DMA_INT_EN, REG_DMA_INT_EN);

	return ret;
}

static const struct of_device_id msc313_bach_of_match[] = {
		{ .compatible = "mstar,msc313-bach", },
		{ },
};
MODULE_DEVICE_TABLE(of, msc313_bach_of_match);

static struct platform_driver msc313_bach_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.owner = THIS_MODULE,
		.pm = &snd_soc_pm_ops,
		.of_match_table = msc313_bach_of_match,
	},
	.probe = msc313_bach_probe,
};
module_platform_driver(msc313_bach_driver);

MODULE_AUTHOR("Daniel Palmer <daniel@thingy.jp>");
MODULE_DESCRIPTION("MStar MSC313 BACH sound");
MODULE_LICENSE("GPL v2");
