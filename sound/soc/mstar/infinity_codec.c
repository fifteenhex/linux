// SPDX-License-Identifier: GPL-2.0
/*
 *   Copyright (c) 2008 MStar Semiconductor, Inc.
 *
 *   This code is based on MStar's original driver that was shipped as part
 *   of a kernel source dump. It's assumed to be GPLv2.
 *
 */

#include <linux/module.h>
#include <linux/errno.h>
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/pm.h>
#include <linux/platform_device.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>

#include <linux/of.h>
#include <linux/of_address.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/initval.h>
#include <sound/tlv.h>
#include <sound/jack.h>

#include "infinity_pcm.h"
#include "infinity.h"

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
* Used to set internal debug sinegen gain level 0~4 (-6dB per step)
*/
#define SINEGEN_GAIN_LEVEL "SineGen Gain Level"

/**
* Used to set internal debug sinegen rate set 0~10 (0 for singen disabling)
*/
#define SINEGEN_RATE_SELECT "SineGen Rate Select"

/**
* Used to select audio playback input (DMA Reader, ADC In)
*/
#define MAIN_PLAYBACK_MUX "Main Playback Mux"

/**
* Used to select ADC input (Line-in, Mic-in)
*/
#define ADC_MUX "ADC Mux"

enum
{
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

static const char *infinity_output_select[]  = {"DMA Reader", "ADC In", "Sine Gen"};
static const struct soc_enum infinity_outsel_enum =
		SOC_ENUM_SINGLE(AUD_PLAYBACK_MUX, 0, 3, infinity_output_select);

#define OUTPUT_DMA	0
#define OUTPUT_ADC_IN	1
#define OUTPUT_SINE_GEN	2

static u16 codec_reg_backup[AUD_REG_LEN] =
{
	0x0,	//AUD_PLAYBACK_MUX
	0x1,	//AUD_ADC_MUX
	0x0,	//AUD_ATOP_PWR
	0x3,	//AUD_DPGA_PWR
	0,	//AUD_PLAYBACK_DPGA
	0,	//AUD_CAPTURE_DPGA
	0,	//AUD_MIC_GAIN
	0,	//AUD_LINEIN_GAIN
	0,	//AUD_DIGMIC_PWR
	0,	//AUD_DBG_SINERATE
	0,	//AUD_DBG_SINEGAIN
};

static struct infinity_pcm_dma_data infinity_pcm_dma_wr[] =
{
	{
		.name	 = "DMA writer",
		.channel = BACH_DMA_WRITER1,
	},
};

static struct infinity_pcm_dma_data infinity_pcm_dma_rd[] =
{
	{
		.name	 = "DMA reader",
		.channel = BACH_DMA_READER1,
	},
};

struct infinity_codec_data {
	struct device *dev;
	struct regmap *regmap;

	//
	struct regmap_field *mux0_mmc1_src;

	// sine gen knobs
	struct regmap_field *sine_gen_en;
	struct regmap_field *sine_gen_len;
	struct regmap_field *sine_gen_ren;
	struct regmap_field *sine_gen_gain;
	struct regmap_field *sine_gen_freq;
};

static struct snd_soc_dai_ops infinity_soc_codec_dai_ops =
{
};

static int infinity_soc_dai_probe(struct snd_soc_dai *dai)
{
  dai->playback_dma_data = (void *)&infinity_pcm_dma_rd[dai->id];
  dai->capture_dma_data = (void *)&infinity_pcm_dma_wr[dai->id];
  return 0;
}

static int infinity_soc_dai_remove(struct snd_soc_dai *dai)
{
  return 0;
}

static int infinity_soc_dai_suspend(struct snd_soc_dai *dai)
{
  return 0;
}

static int infinity_soc_dai_resume(struct snd_soc_dai *dai)
{
  return 0;
}


struct snd_soc_dai_driver infinity_soc_codec_dai_drv[] =
{
	{
		.name	= "infinity-codec-dai-main",
		.probe	= infinity_soc_dai_probe,
		.remove	= infinity_soc_dai_remove,
		/*.suspend = infinity_soc_dai_suspend,
		.resume	= infinity_soc_dai_resume,*/
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
		.ops	= &infinity_soc_codec_dai_ops,
	},
};

static int infinity_soc_codec_probe(struct snd_soc_component* component)
{
	struct regmap *atop;
	struct infinity_codec_data* codec;
	int i;

	atop = syscon_regmap_lookup_by_phandle(component->dev->of_node, "mstar, atop");


	codec = devm_kzalloc(component->dev, sizeof(*codec), GFP_KERNEL);
	if (IS_ERR(codec))
		return PTR_ERR(codec);

	codec->dev = component->dev;

	codec->regmap = syscon_regmap_lookup_by_phandle(component->dev->of_node,
			"mstar,bach");
	if (IS_ERR(codec->regmap))
		return PTR_ERR(codec->regmap);

	codec->mux0_mmc1_src = devm_regmap_field_alloc(component->dev,
			codec->regmap, mux0_mmc1_src_field);

	codec->sine_gen_en = devm_regmap_field_alloc(component->dev,
			codec->regmap, sine_gen_en_field);
	codec->sine_gen_len = devm_regmap_field_alloc(component->dev,
			codec->regmap, sine_gen_len_field);
	codec->sine_gen_ren = devm_regmap_field_alloc(component->dev,
			codec->regmap, sine_gen_ren_field);
	codec->sine_gen_gain = devm_regmap_field_alloc(component->dev,
			codec->regmap, sine_gen_gain_field);
	codec->sine_gen_freq = devm_regmap_field_alloc(component->dev,
			codec->regmap, sine_gen_freq_field);

	snd_soc_component_set_drvdata(component, codec);
	return 0;
}

static void infinity_soc_codec_remove(struct snd_soc_component *codec)
{
}

static int infinity_soc_codec_resume(struct snd_soc_component *codec)
{
  return 0;
}

static int infinity_soc_codec_suspend(struct snd_soc_component *codec)
{
  return 0;
}

//step:-6dB
#if 0
bool InfinitySineGenGain(U16 nGain)
{
    if(nGain<=4)
    {
        InfinityWriteReg(BACH_REG_BANK1, BACH_DMA_TEST_CTRL5, REG_SINE_GEN_GAIN_MSK, nGain<<REG_SINE_GEN_GAIN_POS );
        return true;
    }
    else
        return false;
}

bool InfinitySineGenRate(U16 nRate)
{
    if(nRate<BACH_SINERATE_NUM)
    {
        InfinityWriteReg(BACH_REG_BANK1, BACH_DMA_TEST_CTRL5, REG_SINE_GEN_FREQ_MSK, nRate<<REG_SINE_GEN_FREQ_POS );
        return true;
    }
    else
        return false;
}

void InfinitySineGenEnable(bool bEnable)
{
    U16 nConfigValue;
    nConfigValue = (bEnable? REG_SINE_GEN_EN | REG_SINE_GEN_L | REG_SINE_GEN_R : 0);
    InfinityWriteReg(BACH_REG_BANK1, BACH_DMA_TEST_CTRL5, REG_SINE_GEN_EN | REG_SINE_GEN_L | REG_SINE_GEN_R ,nConfigValue);
}

#endif

static unsigned int infinity_codec_read(struct snd_soc_component *component, unsigned int reg)
{
	struct infinity_codec_data *codec = snd_soc_component_get_drvdata(component);
	unsigned int val;
	switch(reg){
		case AUD_PLAYBACK_MUX:
			// if sine gen is enabled it trumps anything else
			regmap_field_read(codec->sine_gen_en, &val);
			if(val)
				return OUTPUT_SINE_GEN;
			regmap_field_read(codec->mux0_mmc1_src, &val);
			if(val)
				return OUTPUT_DMA;
			else
				return OUTPUT_ADC_IN;
		case AUD_DBG_SINERATE:
			regmap_field_read(codec->sine_gen_freq, &val);
			return val;
		case AUD_DBG_SINEGAIN:
			regmap_field_read(codec->sine_gen_gain, &val);
			return val;
		default:
			dev_info(codec->dev, "unhandled register read from %u", reg);
			return 0;
	}
}

static void infinity_codec_disable_sine_gen(struct infinity_codec_data *codec){
	regmap_field_write(codec->sine_gen_en, 0);
}

static int infinity_codec_write(struct snd_soc_component *component, unsigned int reg,
		unsigned int value) {
	struct infinity_codec_data *codec = snd_soc_component_get_drvdata(component);
	int ret = 0;

	switch (reg) {
		case AUD_PLAYBACK_MUX:
			switch(value){
			case OUTPUT_DMA:
				infinity_codec_disable_sine_gen(codec);
				regmap_field_write(codec->mux0_mmc1_src, 1);
				//InfinityDmaSetRate(BACH_DMA_READER1,
				//		InfinityRateFromU32(InfinityDmaGetRate(BACH_DMA_READER1)));
				break;
			case OUTPUT_ADC_IN:
				infinity_codec_disable_sine_gen(codec);
				regmap_field_write(codec->mux0_mmc1_src, 0);
				break;
			case OUTPUT_SINE_GEN:
				regmap_field_write(codec->sine_gen_len, 1);
				regmap_field_write(codec->sine_gen_ren, 1);
				regmap_field_write(codec->sine_gen_en, 1);
				//InfinityDmaSetRate(BACH_DMA_READER1, BACH_RATE_48K);
				//InfinitySineGenEnable(true);
				break;
			default:
				return -EINVAL;
			}
		break;
#if 0
	case AUD_ADC_MUX:
		ret = snd_soc_component_update_bits(codec, AUD_ATOP_PWR, 0x1,
				0);

		if (value == 0 || value == 1) {
			codec_reg[reg] = value;
		} else {
			return 0;
		}

		if (ret > 0)
			snd_soc_component_update_bits(codec, AUD_ATOP_PWR, 0x1,
					0x1);

		break;

	case AUD_ATOP_PWR:
		if ((codec_reg[reg] ^ value) & 0x1) {
			if (codec_reg[AUD_ADC_MUX] == 0)
				(value & 0x1) ? InfinityOpenAtop(
								BACH_ATOP_LINEIN) :
						InfinityCloseAtop(
								BACH_ATOP_LINEIN);
			else
				(value & 0x1) ? InfinityOpenAtop(
								BACH_ATOP_MIC) :
						InfinityCloseAtop(
								BACH_ATOP_MIC);
		}
		if ((codec_reg[reg] ^ value) & 0x2) {
			(value & 0x2) ? InfinityOpenAtop(BACH_ATOP_LINEOUT) : InfinityCloseAtop(
							BACH_ATOP_LINEOUT);
		}
		break;

	case AUD_DPGA_PWR:
		if ((codec_reg[reg] ^ value) & 0x1) {
			InfinitySetPathOnOff(BACH_PATH_PLAYBACK,
					(value & 0x1) ? TRUE : FALSE);
		}
		if ((codec_reg[reg] ^ value) & 0x2) {
			InfinitySetPathOnOff(BACH_PATH_CAPTURE,
					(value & 0x2) ? TRUE : FALSE);
		}
		break;

	case AUD_PLAYBACK_DPGA:
		InfinitySetPathGain(BACH_PATH_PLAYBACK,
				(S8) (BACH_DPGA_GAIN_MIN_DB + value));
		break;
	case AUD_CAPTURE_DPGA:
		InfinitySetPathGain(BACH_PATH_CAPTURE,
				(S8) (BACH_DPGA_GAIN_MIN_DB + value));
		break;
	case AUD_MIC_GAIN:
		InfinityAtopMicGain(value);
		break;
	case AUD_LINEIN_GAIN:
		InfinityAtopLineInGain(value);
		break;
	case AUD_DIGMIC_PWR:
		if ((codec_reg[reg] ^ value)) {
			if (!InfinityDigMicEnable(value)) {
				return -1;
			}
		}
		break;
#endif
	case AUD_DBG_SINERATE:
		regmap_field_write(codec->sine_gen_freq, value);
		break;
	case AUD_DBG_SINEGAIN:
		regmap_field_write(codec->sine_gen_gain, value);
		break;

	default:
		dev_info(codec->dev, "unhandled register write to %u = %u", reg, value);
		break;
	}
	return 0;
}

static const unsigned int infinity_dpga_tlv[] =
{
  TLV_DB_RANGE_HEAD(1),
  0, 76, TLV_DB_LINEAR_ITEM(-64, 12),
};

static const struct snd_kcontrol_new infinity_snd_controls[] =
{
	SOC_SINGLE_TLV(MAIN_PLAYBACK_VOLUME, AUD_PLAYBACK_DPGA, 0, 76, 0, infinity_dpga_tlv),
	SOC_SINGLE_TLV(MAIN_CAPTURE_VOLUME, AUD_CAPTURE_DPGA, 0, 76, 0, infinity_dpga_tlv),
	SOC_SINGLE_TLV(MIC_GAIN_SELECTION, AUD_MIC_GAIN, 0, 31, 0, NULL),
	SOC_SINGLE_TLV(LINEIN_GAIN_LEVEL, AUD_LINEIN_GAIN, 0, 7, 0, NULL),
	SOC_SINGLE_TLV(SINEGEN_GAIN_LEVEL, AUD_DBG_SINEGAIN, 0, 4, 0, NULL),
	SOC_SINGLE_RANGE(SINEGEN_RATE_SELECT, AUD_DBG_SINERATE, 0, 0, 10, 0)
};



static const char *infinity_adc_select[]     = {"Line-in", "Mic-in"};




static const struct soc_enum infinity_adcsel_enum =
  SOC_ENUM_SINGLE(AUD_ADC_MUX, 0, 2, infinity_adc_select);

static const struct snd_kcontrol_new infinity_output_mux_controls =
  SOC_DAPM_ENUM("Playback Select", infinity_outsel_enum);

static const struct snd_kcontrol_new infinity_adc_mux_controls =
  SOC_DAPM_ENUM("ADC Select", infinity_adcsel_enum);


static const struct snd_soc_dapm_widget infinity_dapm_widgets[] =
{
  SND_SOC_DAPM_MUX(MAIN_PLAYBACK_MUX, SND_SOC_NOPM, 0, 0, &infinity_output_mux_controls),

  SND_SOC_DAPM_OUTPUT("LINEOUT"),
  //SND_SOC_DAPM_OUTPUT("HPOUT"),

  //SND_SOC_DAPM_AIF_OUT("DMAWR1", "Sub Capture",  0, SND_SOC_NOPM, 0, 0),
  SND_SOC_DAPM_AIF_OUT("DMAWR", "Main Capture",   0, SND_SOC_NOPM, 0, 0),
  //SND_SOC_DAPM_AIF_IN("DMARD1",  "Main Playback", 0, SND_SOC_NOPM, 0, 0),
  //SND_SOC_DAPM_AIF_IN("DMARD2",  "Sub Playback",  0, SND_SOC_NOPM, 0, 0),
  SND_SOC_DAPM_AIF_IN("DIGMIC", NULL,   0, AUD_DIGMIC_PWR, 0, 0),

  SND_SOC_DAPM_INPUT("DMARD"),
  SND_SOC_DAPM_INPUT("LINEIN"),
  SND_SOC_DAPM_INPUT("MICIN"),


  SND_SOC_DAPM_DAC("DAC",   NULL, AUD_ATOP_PWR, 1, 0),
  SND_SOC_DAPM_DAC("Hp Amp",NULL, AUD_ATOP_PWR, 1, 0),
  SND_SOC_DAPM_ADC("ADC",   NULL, AUD_ATOP_PWR, 0, 0),
  //SND_SOC_DAPM_ADC("Mic Bias", NULL, AUD_ATOP_PWR, 2, 0),
  //SND_SOC_DAPM_MICBIAS("Mic Bias", AUD_ATOP_PWR, 5, 0),

  SND_SOC_DAPM_PGA("Main Playback DPGA", AUD_DPGA_PWR, 0, 0, NULL, 0),
  SND_SOC_DAPM_PGA("Main Capture DPGA",  AUD_DPGA_PWR, 1, 0, NULL, 0),

  SND_SOC_DAPM_MUX(ADC_MUX, SND_SOC_NOPM, 0, 0, &infinity_adc_mux_controls),

};

static const struct snd_soc_dapm_route infinity_codec_routes[] =
{
	{"Main Playback Mux", "DMA Reader", "DMARD"},
	{"Main Playback Mux", "ADC In",     "ADC"},
	{"Main Playback Mux", "Sine Gen",   "DMARD"},
	{"Main Playback DPGA", NULL, "Main Playback Mux"},

	{"DAC",     NULL, "Main Playback DPGA"},
	{"LINEOUT", NULL, "DAC"},

	{"DMAWR", NULL, "Main Capture DPGA"},
	{"Main Capture DPGA", NULL, "ADC"},

	{"ADC",     NULL, "ADC Mux"},
	{"ADC Mux", "Mic-in",  "MICIN"}
};

static const struct snd_soc_component_driver infinity_soc_codec_drv = {
	.probe =    infinity_soc_codec_probe,
	.remove =   infinity_soc_codec_remove,
	.suspend =  infinity_soc_codec_suspend,
	.resume =   infinity_soc_codec_resume,
	.write =    infinity_codec_write,
	.read  =    infinity_codec_read,
	.controls = infinity_snd_controls,
	.num_controls = ARRAY_SIZE(infinity_snd_controls),
	.dapm_widgets = infinity_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(infinity_dapm_widgets),
	.dapm_routes = infinity_codec_routes,
	.num_dapm_routes = ARRAY_SIZE(infinity_codec_routes),
};

static int infinity_codec_probe(struct platform_device *pdev)
{
	u32 val;
	int ret;
	struct device_node *node = pdev->dev.of_node; //(struct device_node *)platform_get_drvdata(pdev);

	ret = of_property_read_u32(node, "playback-volume-level", &val);
	if (ret == 0)
	{
    	    codec_reg_backup[AUD_PLAYBACK_DPGA] = val;
	}

	ret = of_property_read_u32(node, "capture-volume-level", &val);
	if (ret == 0)
	{
		codec_reg_backup[AUD_CAPTURE_DPGA] = val;
	}

	ret = of_property_read_u32(node, "micin-gain-sel", &val);
	if (ret == 0)
	{
		codec_reg_backup[AUD_MIC_GAIN] = val;
	}

	ret = of_property_read_u32(node, "linein-gain-level", &val);
	if (ret == 0)
	{
		codec_reg_backup[AUD_LINEIN_GAIN] = val;
	}
	return snd_soc_register_component(&pdev->dev, &infinity_soc_codec_drv,
			infinity_soc_codec_dai_drv, ARRAY_SIZE(infinity_soc_codec_dai_drv));
}

static int infinity_codec_remove(struct platform_device *pdev)
{
  snd_soc_unregister_component(&pdev->dev);
  return 0;
}

static const struct of_device_id infinity_codec_of_match[] = {
	{ .compatible = "mstar,snd-infinity-codec", },
	{ },
};
MODULE_DEVICE_TABLE(of, infinity_codec_of_match);

static struct platform_driver infinity_codec_driver =
{
	.driver = {
		.name = "infinity-codec",
		.owner = THIS_MODULE,
		.of_match_table = infinity_codec_of_match,
	},
	.probe = infinity_codec_probe,
	.remove = infinity_codec_remove,
};

module_platform_driver(infinity_codec_driver);

/* Module information */
MODULE_AUTHOR("Roger Lai, roger.lai@mstarsemi.com");
MODULE_DESCRIPTION("Infinity Bach Audio ALSA SoC Codec");
MODULE_LICENSE("GPL v2");
