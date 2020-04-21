/*------------------------------------------------------------------------------
 *   Copyright (c) 2008 MStar Semiconductor, Inc.  All rights reserved.
 *------------------------------------------------------------------------------*/

#include <linux/module.h>
#include <linux/device.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/initval.h>
#include <sound/soc.h>

static struct snd_soc_dai_ops infinity_soc_cpu_dai_ops =
{
};

struct snd_soc_dai_driver infinity_soc_cpu_dai_drv =
{
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
	.ops = &infinity_soc_cpu_dai_ops,
};

static const struct snd_soc_component_driver infinity_soc_component = {
	.name = "mstar-bach",
};

static int infinity_cpu_dai_probe(struct platform_device *pdev)
{
	dev_info(&pdev->dev, "probe");
	return snd_soc_register_component(&pdev->dev, &infinity_soc_component,
			&infinity_soc_cpu_dai_drv, 1);
}

static int infinity_cpu_dai_remove(struct platform_device *pdev)
{
  snd_soc_unregister_component(&pdev->dev);
  return 0;
}

static const struct of_device_id infinity_cpu_of_match[] = {
	{ .compatible = "mstar,snd-infinity-cpu", },
	{ },
};
MODULE_DEVICE_TABLE(of, infinity_cpu_of_match);

static struct platform_driver infinity_cpu_dai_driver =
{
	.driver = {
		.name = "infinity-cpu-dai",
		.owner = THIS_MODULE,
		.of_match_table = infinity_cpu_of_match,
	},
	.probe = infinity_cpu_dai_probe,
	.remove = infinity_cpu_dai_remove,
};

module_platform_driver(infinity_cpu_dai_driver);

/* Module information */
MODULE_AUTHOR("Trevor Wu, trevor.wu@mstarsemi.com");
MODULE_DESCRIPTION("Infinity Bach Audio ALSA SoC Dai");
MODULE_LICENSE("GPL v2");
