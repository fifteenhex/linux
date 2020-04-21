/*------------------------------------------------------------------------------
 *   Copyright (c) 2008 MStar Semiconductor, Inc.  All rights reserved.
 *------------------------------------------------------------------------------*/

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

#include "infinity_pcm.h"
#include "infinity.h"

struct infinity_soc {
	struct snd_soc_dai_link_component cpu_dai_component;
	struct snd_soc_dai_link_component platform_component;
	struct snd_soc_dai_link_component codec_component;
	struct snd_soc_dai_link dai_link;
	struct snd_soc_card card;
};

static int infinity_soc_dai_link_init(struct snd_soc_pcm_runtime *rtd) {
	return 0;
}

static int infinity_soc_dai_link_hw_params(struct snd_pcm_substream *substream,
		struct snd_pcm_hw_params *params) {
	return 0;
}

static int infinity_soc_card_probe(struct snd_soc_card *card) {
	return 0;
}

static int infinity_soc_card_suspend_pre(struct snd_soc_card *card) {
	int i;

	struct snd_soc_dapm_widget *w;

#if 0
	for (i = 0; i < card->num_rtd; i++) {
		struct snd_soc_dai *codec_dai = card->rtd[i].codec_dai;

		if (card->rtd[i].dai_link->ignore_suspend)
			continue;

		w = codec_dai->playback_widget;
		if (w->active) {
			AUD_PRINTF(TRACE_LEVEL,
					"snd_soc_dapm_stream_event(): stop PLAYBACK %d\n",
					i);
			snd_soc_dapm_stream_event(&card->rtd[i],
					SNDRV_PCM_STREAM_PLAYBACK,
					SND_SOC_DAPM_STREAM_STOP);

			stream_playback_active |= 1 << i;

		}
		w = codec_dai->capture_widget;
		if (w->active) {
			AUD_PRINTF(TRACE_LEVEL,
					"snd_soc_dapm_stream_event(): stop CAPTURE %d\n",
					i);
			snd_soc_dapm_stream_event(&card->rtd[i],
					SNDRV_PCM_STREAM_CAPTURE,
					SND_SOC_DAPM_STREAM_STOP);

			stream_capture_active |= 1 << i;

		}
	}

#endif

#if 1
	snd_soc_dapm_disable_pin(&card->dapm, "DMARD");
	snd_soc_dapm_disable_pin(&card->dapm, "LINEIN");
	snd_soc_dapm_sync(&card->dapm);
#endif

	return 0;
}

static int infinity_soc_card_suspend_post(struct snd_soc_card *card) {
	return 0;

}

static int infinity_soc_card_resume_pre(struct snd_soc_card *card) {
	return 0;

}

static int infinity_soc_card_resume_post(struct snd_soc_card *card) {
	int i;

#if 1
	snd_soc_dapm_enable_pin(&card->dapm, "DMARD");
	snd_soc_dapm_enable_pin(&card->dapm, "LINEIN");
	snd_soc_dapm_sync(&card->dapm);
#endif

#if 0
	for (i = 0; i < card->num_rtd; i++) {
		//struct snd_soc_dai *codec_dai = card->rtd[i].codec_dai;

		if (stream_playback_active & (1 << i)) {
			snd_soc_dapm_stream_event(&card->rtd[i],
					SNDRV_PCM_STREAM_PLAYBACK,
					SND_SOC_DAPM_STREAM_START);
			stream_playback_active &= ~(1 << i);

		}

		if (stream_capture_active & (1 << i)) {
			snd_soc_dapm_stream_event(&card->rtd[i],
					SNDRV_PCM_STREAM_CAPTURE,
					SND_SOC_DAPM_STREAM_START);
			stream_capture_active &= ~(1 << i);
		}
	}
#endif

	return 0;

}

static struct snd_soc_ops infinity_soc_ops = {
	.hw_params = infinity_soc_dai_link_hw_params,
};

static int infinity_audio_probe(struct platform_device *pdev) {
	struct device *dev = &pdev->dev;
	struct infinity_soc *soc;
	struct device_node* device_node;
	int ret;

	soc = devm_kzalloc(dev, sizeof(*soc), GFP_KERNEL);
	if(IS_ERR(soc))
		return PTR_ERR(soc);

	device_node = of_parse_phandle(dev->of_node, "mstar,bach-cpu", 0);
	if(IS_ERR(device_node))
		return PTR_ERR(device_node);
	soc->cpu_dai_component.of_node = device_node;

	device_node = of_parse_phandle(dev->of_node, "mstar,bach-platform", 0);
		if(IS_ERR(device_node))
			return PTR_ERR(device_node);
	soc->platform_component.of_node = device_node;

	device_node = of_parse_phandle(dev->of_node, "mstar,bach-codec", 0);
		if(IS_ERR(device_node))
			return PTR_ERR(device_node);
	soc->codec_component.of_node = device_node;
	soc->codec_component.dai_name = "infinity-codec-dai-main",

	soc->dai_link.name = "Infinity Soc Dai Link";
	soc->dai_link.stream_name = "msb2501_dai_stream";
	soc->dai_link.cpus = &soc->cpu_dai_component;
	soc->dai_link.num_cpus = 1;
	soc->dai_link.codecs = &soc->codec_component;
	soc->dai_link.num_codecs = 1;
	soc->dai_link.platforms = &soc->platform_component;
	soc->dai_link.num_platforms = 1;
	soc->dai_link.init = infinity_soc_dai_link_init;
	soc->dai_link.ops = &infinity_soc_ops;

	soc->card.name = "infinity_snd_machine";
	soc->card.owner = THIS_MODULE;
	soc->card.dai_link = &soc->dai_link;
	soc->card.num_links = 1;
	soc->card.probe = infinity_soc_card_probe;
	soc->card.suspend_pre = infinity_soc_card_suspend_pre;
	soc->card.suspend_post = infinity_soc_card_suspend_post;
	soc->card.resume_pre = infinity_soc_card_resume_pre;
	soc->card.resume_post = infinity_soc_card_resume_post;

	soc->card.dev = dev;
	dev_set_drvdata(dev, soc);

	dev_info(dev, "registering card");
	ret = devm_snd_soc_register_card(dev, &soc->card);

	if(!ret){
		dev_info(dev, "registered");
	}

	return ret;
}

int infinity_audio_remove(struct platform_device *pdev) {
	struct infinity_soc *soc = dev_get_drvdata(&pdev->dev);
	snd_soc_unregister_card(&soc->card);
	return 0;
}

static const struct of_device_id infinity_audio_of_match[] = {
		{ .compatible = "mstar,snd-infinity", },
		{ },
};
MODULE_DEVICE_TABLE(of, infinity_audio_of_match);

static struct platform_driver infinity_audio = {
	.driver = {
		.name = "infinity-audio",
		.owner = THIS_MODULE,
		.pm = &snd_soc_pm_ops,
		.of_match_table = infinity_audio_of_match,
	},
	.probe = infinity_audio_probe,
	.remove = infinity_audio_remove,
};

module_platform_driver(infinity_audio);

MODULE_AUTHOR("Roger Lai, roger.lai@mstarsemi.com");
MODULE_DESCRIPTION("iNfinity Bach Audio ASLA SoC Machine");
MODULE_LICENSE("GPL v2");
