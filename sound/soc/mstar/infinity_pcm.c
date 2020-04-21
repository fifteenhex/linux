/*------------------------------------------------------------------------------
 *   Copyright (c) 2008 MStar Semiconductor, Inc.  All rights reserved.
 *  ------------------------------------------------------------------------------*/

#include <linux/module.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <linux/mfd/syscon.h>
#include <linux/delay.h>
#include <linux/spinlock.h>

#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#include "infinity_pcm.h"
#include "infinity.h"

static struct platform_device *infinity_dma_device = NULL;
static BOOL fClk = false;
static BOOL isDmaWork[2] = { false, false };

#define params_period_bytes(p) \
	(hw_param_interval_c((p), SNDRV_PCM_HW_PARAM_PERIOD_BYTES)->min)

struct infinity_pcm_runtime_data {
	spinlock_t lock;
	U32 state;
	size_t dma_level_count;
	size_t int_level_count;
	struct infinity_pcm_dma_data *dma_data;
	void *private_data;
	size_t remain_count;
};

static const struct snd_pcm_hardware infinity_pcm_playback_hardware = { .info =
SNDRV_PCM_INFO_INTERLEAVED, .formats = SNDRV_PCM_FMTBIT_S16_LE |
SNDRV_PCM_FMTBIT_S24_LE |
SNDRV_PCM_FMTBIT_S32_LE, .rates = SNDRV_PCM_RATE_8000_48000, .rate_min = 8000,
		.rate_max = 48000, .channels_min = 1, .channels_max = 2,
		.buffer_bytes_max = 96 * 1024, .period_bytes_min = 8 * 1024,
		.period_bytes_max = 24 * 1024, .periods_min = 4, .periods_max =
				8, .fifo_size = 32, };

static const struct snd_pcm_hardware infinity_pcm_capture_hardware = { .info =
SNDRV_PCM_INFO_INTERLEAVED, .formats = SNDRV_PCM_FMTBIT_S16_LE, .rates =
SNDRV_PCM_RATE_8000_48000, .rate_min = 8000, .rate_max = 48000, .channels_min =
		1, .channels_max = 2, .buffer_bytes_max = 80 * 1024,
		.period_bytes_min = 1 * 1024, .period_bytes_max = 10 * 1024,
		.periods_min = 4, .periods_max = 12, .fifo_size = 32, };

#if 0
static irqreturn_t infinity_pcm_dma_irq(int irq, void *dev_id) {
	struct snd_pcm_substream *substream = (struct snd_pcm_substream*) dev_id;
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct infinity_pcm_runtime_data *prtd = runtime->private_data;
	struct infinity_pcm_dma_data *dma_data = prtd->dma_data;

	unsigned long flags, fgs;
	//unsigned int size = 0;
	unsigned int state = 0;
	//unsigned long time;
	//snd_pcm_uframes_t offset;
	// AUD_PRINTF(PLAYBACK_IRQ_LEVEL, "in %s,status 0x%x\n",  __func__,InfinityReadReg2Byte(0x1009bc));
	spin_lock_irqsave(&prtd->lock, flags);
	if ((SNDRV_PCM_STREAM_PLAYBACK == substream->stream)
			&& snd_pcm_running(substream)) {
		if (prtd->state != DMA_EMPTY) {
			//time = jiffies_to_msecs(jiffies);

			if (InfinityDmaIsEmpty(dma_data->channel)) {
				prtd->state = DMA_EMPTY;

				spin_unlock_irqrestore(&prtd->lock, flags);

				//snd_pcm_stream_lock_irq(substream);
				snd_pcm_stream_lock_irqsave(substream, fgs);
				if (snd_pcm_running(substream))
					snd_pcm_stop(substream,
							SNDRV_PCM_STATE_XRUN);
				snd_pcm_stream_unlock_irqrestore(substream,
						fgs);
				//snd_pcm_stream_unlock_irq(substream);

				//snd_pcm_period_elapsed(substream);
				spin_lock_irqsave(&prtd->lock, flags);

			} else if ((prtd->state != DMA_UNDERRUN)
					&& InfinityDmaIsUnderrun(
							dma_data->channel)) {
				//size = BachDmaGetLevelCnt(dma_data->channel);

				//BachDmaMaskInt(dma_data->channel, BACH_DMA_INT_OVERRUN, false);
				InfinityDmaMaskInt(dma_data->channel,
						BACH_DMA_INT_UNDERRUN, true);

				prtd->state = DMA_UNDERRUN;

				spin_unlock_irqrestore(&prtd->lock, flags);
				snd_pcm_period_elapsed(substream);
				// AUD_PRINTF(PLAYBACK_IRQ_LEVEL, "UNDER: chanId = %d, frameCnt = 0x%x, T = %d\n",
				//            dma_data->channel, (unsigned int)bytes_to_frames(runtime, InfinityDmaGetLevelCnt(dma_data->channel)), (unsigned int)jiffies_to_msecs(jiffies));
				spin_lock_irqsave(&prtd->lock, flags);
			}
		}
	} else if ((SNDRV_PCM_STREAM_CAPTURE == substream->stream)
			&& snd_pcm_running(substream))  // CAPTURE device
					{
		state = prtd->state;

		if ((prtd->state != DMA_UNDERRUN)
				&& InfinityDmaIsUnderrun(dma_data->channel)) {

			InfinityDmaMaskInt(dma_data->channel,
					BACH_DMA_INT_UNDERRUN, true);
			InfinityDmaMaskInt(dma_data->channel,
					BACH_DMA_INT_OVERRUN, false);
			//BachDmaMaskInt(dma_data->channel, BACH_DMA_INT_FULL, false);

			prtd->state = DMA_UNDERRUN;
			spin_unlock_irqrestore(&prtd->lock, flags);

			spin_lock_irqsave(&prtd->lock, flags);

		} else if (prtd->state != DMA_FULL) {
			if (InfinityDmaIsFull(dma_data->channel)) {
				//BachDmaMaskInt(dma_data->channel, BACH_DMA_INT_FULL, true);
				InfinityDmaMaskInt(dma_data->channel,
						BACH_DMA_INT_OVERRUN, true);
				InfinityDmaMaskInt(dma_data->channel,
						BACH_DMA_INT_UNDERRUN, false);

				prtd->state = DMA_FULL;

				spin_unlock_irqrestore(&prtd->lock, flags);
				//snd_pcm_stream_lock_irq(substream);
				snd_pcm_stream_lock_irqsave(substream, fgs);
				if (snd_pcm_running(substream))
					snd_pcm_stop(substream,
							SNDRV_PCM_STATE_XRUN);
				snd_pcm_stream_unlock_irqrestore(substream,
						fgs);
				//snd_pcm_stream_unlock_irq(substream);
				//snd_pcm_period_elapsed(substream);

				spin_lock_irqsave(&prtd->lock, flags);
			} else if ((prtd->state != DMA_OVERRUN)
					&& InfinityDmaIsOverrun(
							dma_data->channel)) {
				InfinityDmaMaskInt(dma_data->channel,
						BACH_DMA_INT_OVERRUN, true);
				InfinityDmaMaskInt(dma_data->channel,
						BACH_DMA_INT_UNDERRUN, false);

				prtd->state = DMA_OVERRUN;

				//size = BachDmaGetLevelCnt(dma_data->channel);

				spin_unlock_irqrestore(&prtd->lock, flags);
				snd_pcm_period_elapsed(substream);

				spin_lock_irqsave(&prtd->lock, flags);
			}
		}
	}
	spin_unlock_irqrestore(&prtd->lock, flags);

	// AUD_PRINTF(TRACE_LEVEL, "!!BACH_DMA1_CTRL_0 = 0x%x,  BACH_DMA1_CTRL_8 = 0x%x, level count = %d\n",
	// 		InfinityReadReg(BACH_REG_BANK1, BACH_DMA1_CTRL_0), InfinityReadReg(BACH_REG_BANK1, BACH_DMA1_CTRL_8), InfinityDmaGetLevelCnt(prtd->dma_data->channel));

	// if(prtd->state == DMA_EMPTY)
	//   AUD_PRINTF(PLAYBACK_IRQ_LEVEL, "Out of infinity_pcm_dma_irq\n");

	return IRQ_HANDLED;
}
#endif

static int infinity_pcm_open(struct snd_pcm_substream *substream) {
#if 0
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai_link *dai_link = rtd->dai_link;
	struct snd_pcm *pcm = rtd->pcm;

	struct infinity_pcm_runtime_data *prtd;
	int ret = 0;

	if (SNDRV_PCM_STREAM_PLAYBACK == substream->stream) {
		snd_soc_set_runtime_hwparams(substream,
				&infinity_pcm_playback_hardware);
	} else if (SNDRV_PCM_STREAM_CAPTURE == substream->stream) {
		snd_soc_set_runtime_hwparams(substream,
				&infinity_pcm_capture_hardware);
	} else {
		ret = -EINVAL;
		goto out;
	}
	/* Ensure that buffer size is a multiple of period size */
#if 0
  ret = snd_pcm_hw_constraint_integer(runtime,
                                      SNDRV_PCM_HW_PARAM_PERIODS);
  if (ret < 0)
    goto out;
#endif

	prtd = kzalloc(sizeof(*prtd), GFP_KERNEL);
	if (prtd == NULL) {
		ret = -ENOMEM;
		goto out;
	}

	spin_lock_init(&prtd->lock);
	runtime->private_data = prtd;

	out: return ret;
#endif
	return 0;
}

static int infinity_pcm_close(struct snd_pcm_substream *substream) {
	struct snd_pcm_runtime *runtime = substream->runtime;

	kfree(runtime->private_data); //free infinity_pcm_runtime_data
	runtime->private_data = NULL;

	return 0;
}

static int infinity_pcm_hw_params(struct snd_pcm_substream *substream,
		struct snd_pcm_hw_params *params) {
#if 0
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_codec *codec = rtd->codec;
	struct infinity_pcm_runtime_data *prtd = runtime->private_data;
	struct infinity_pcm_dma_data *dma_data;
	int err = 0;
#ifdef CONFIG_OF
	int num_parents, i;
	struct clk **snd_clks;
#endif

#ifdef CONFIG_OF
	if (!fClk) {
		fClk = true;

		num_parents = of_clk_get_parent_count(
				infinity_dma_device->dev.of_node);
		if (num_parents > 0) {
			snd_clks = kzalloc((sizeof(struct clk*) * num_parents),
					GFP_KERNEL);
			for (i = 0; i < num_parents; i++) {
				snd_clks[i] =
						of_clk_get(
								infinity_dma_device->dev.of_node,
								i);
				if (IS_ERR(snd_clks[i])) {

					kfree(snd_clks);
					return -EINVAL;
				} else {
					clk_prepare_enable(snd_clks[i]);
				}
			}
			kfree(snd_clks);
		}
	}
#endif
	dma_data = snd_soc_dai_get_dma_data(rtd->codec_dai, substream);

	if (!dma_data)
		return 0;

#ifdef DIGMIC_EN
  if(dma_data->channel==BACH_DMA_WRITER1)
  {
    if(!InfinityDigMicSetRate(InfinityRateFromU32(params_rate(params))))
    {
      AUD_PRINTF(ERROR_LEVEL, "%s: rate = %d digital mic not supported\n",
                            __FUNCTION__, params_rate(params));
      return -EINVAL;
    }
  }
#endif

	if (!InfinityDmaSetRate(dma_data->channel,
			InfinityRateFromU32(params_rate(params)))) {

		return -EINVAL;
	}

	if (snd_soc_read(codec, AUD_PLAYBACK_MUX) == 1
			&& dma_data->channel == BACH_DMA_WRITER1)
		InfinityDmaSetRate(BACH_DMA_READER1,
				InfinityRateFromU32(params_rate(params)));

	snd_pcm_set_runtime_buffer(substream, &substream->dma_buffer);
	runtime->dma_bytes = params_buffer_bytes(params);

	if (prtd->dma_data)
		return 0;

	prtd->dma_data = dma_data;

	/*err = request_irq(INFINITY_IRQ_ID, //INT_MS_AUDIO_1,
	 infinity_pcm_dma_irq,
	 IRQF_SHARED,
	 dma_data->name,
	 (void *)substream);*/
	if (!err) {
		/*
		 * Link channel with itself so DMA doesn't need any
		 * reprogramming while looping the buffer
		 */

		//InfinityDmaReset();
		// Re-set up the underrun and overrun
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
			InfinityDmaInitChannel(dma_data->channel,
					(runtime->dma_addr - MIU0_OFFSET),
					runtime->dma_bytes,
					params_channels(params),
					snd_pcm_format_physical_width(
							params_format(params)),
					params_rate(params),
					(runtime->dma_bytes + 1024),
					(runtime->dma_bytes
							- params_period_bytes(
									params)));

		} else {
			InfinityDmaInitChannel(dma_data->channel,
					(runtime->dma_addr - MIU0_OFFSET),
					runtime->dma_bytes,
					params_channels(params),
					snd_pcm_format_physical_width(
							params_format(params)),
					params_rate(params),
					params_period_bytes(params), 256);

		}

		isDmaWork[substream->stream] = true;

		memset(runtime->dma_area, 0, runtime->dma_bytes);
	}

	return err;
#endif
	return 0;
}

static int infinity_pcm_hw_free(struct snd_pcm_substream *substream) {
#if 0
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct infinity_pcm_runtime_data *prtd = runtime->private_data;
#ifdef CONFIG_OF
	int num_parents, i;
	struct clk **snd_clks;
#endif

	if (prtd->dma_data == NULL)
		return 0;

	isDmaWork[substream->stream] = false;

#ifdef CONFIG_OF
	if (!(InfinityDmaIsWork(BACH_DMA_READER1)
			|| InfinityDmaIsWork(BACH_DMA_WRITER1)
			|| isDmaWork[SNDRV_PCM_STREAM_PLAYBACK]
			|| isDmaWork[SNDRV_PCM_STREAM_CAPTURE]) && fClk) {
		fClk = false;

		num_parents = of_clk_get_parent_count(
				infinity_dma_device->dev.of_node);
		if (num_parents > 0) {
			snd_clks = kzalloc((sizeof(struct clk*) * num_parents),
					GFP_KERNEL);
			for (i = 0; i < num_parents; i++) {
				snd_clks[i] =
						of_clk_get(
								infinity_dma_device->dev.of_node,
								i);
				if (IS_ERR(snd_clks[i])) {

					kfree(snd_clks);
					return -EINVAL;
				} else {
					clk_disable_unprepare(snd_clks[i]);
				}
			}
			kfree(snd_clks);
		}
	}
#endif
	//omap_dma_unlink_lch(prtd->dma_ch, prtd->dma_ch);
	//omap_free_dma(prtd->dma_ch);
	prtd->dma_data = NULL;
	snd_pcm_set_runtime_buffer(substream, NULL);

	return 0;
#endif
	return 0;
}

static int infinity_pcm_prepare(struct snd_pcm_substream *substream) {
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct infinity_pcm_runtime_data *prtd = runtime->private_data;
	struct infinity_pcm_dma_data *dma_data = prtd->dma_data;
	//unsigned long flags;
	int ret = 0;
	//ssize_t runtime_period = frames_to_bytes(runtime, runtime->period_size);
	//ssize_t runtime_buffer = frames_to_bytes(runtime, runtime->buffer_size);

	/* if(substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
	 {
	 //AUD_PRINTF(ERROR_LEVEL, "Prepare Time = %llu\n", Chip_Get_US_Ticks());
	 bPlayFirst = 1;
	 }
	 else
	 bCapFirst = 1;
	 */
	return ret;
}

static int infinity_pcm_trigger(struct snd_pcm_substream *substream, int cmd) {
#if 0
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct infinity_pcm_runtime_data *prtd = runtime->private_data;
	struct infinity_pcm_dma_data *dma_data = prtd->dma_data;
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&prtd->lock, flags);
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		break;

	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		memset(runtime->dma_area, 0, runtime->dma_bytes);
		prtd->dma_level_count = 0;
		prtd->remain_count = 0;
		prtd->state = 0;
		if (cmd == SNDRV_PCM_TRIGGER_STOP
				&& substream->stream
						== SNDRV_PCM_STREAM_PLAYBACK)

			if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
				InfinityDmaMaskInt(dma_data->channel,
						BACH_DMA_INT_OVERRUN, true);
			else if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
				InfinityDmaMaskInt(dma_data->channel,
						BACH_DMA_INT_UNDERRUN, true);
			else {
				ret = -EINVAL;
				break;
			}

		// if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		//   InfinityDmaMaskInt(dma_data->channel, BACH_DMA_INT_EMPTY, true);

		InfinityDmaStopChannel(dma_data->channel);

		break;
	default:
		ret = -EINVAL;
	}
	spin_unlock_irqrestore(&prtd->lock, flags);

	// AUD_PRINTF(TRACE_LEVEL, "!!Out BACH_DMA1_CTRL_0 = 0x%x,  BACH_DMA1_CTRL_8 = 0x%x, level count = %d\n",
	//		InfinityReadReg(BACH_REG_BANK1, BACH_DMA1_CTRL_0), InfinityReadReg(BACH_REG_BANK1, BACH_DMA1_CTRL_8), InfinityDmaGetLevelCnt(prtd->dma_data->channel));

	return ret;
#endif
	return 0;
}

static snd_pcm_uframes_t infinity_pcm_pointer(
		struct snd_pcm_substream *substream) {
#if 0
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct infinity_pcm_runtime_data *prtd = runtime->private_data;
	struct infinity_pcm_dma_data *dma_data = prtd->dma_data;

	snd_pcm_uframes_t offset = 0;
	unsigned long flags;
	size_t last_dma_count_level = 0;

	if (SNDRV_PCM_STREAM_PLAYBACK == substream->stream) {
		//spin_lock(&prtd->lock);
		spin_lock_irqsave(&prtd->lock, flags);

		last_dma_count_level = InfinityDmaGetLevelCnt(
				dma_data->channel);
		//udelay(500);
		if (prtd->dma_level_count > runtime->dma_bytes * 2)
			prtd->dma_level_count -= runtime->dma_bytes;
		offset = bytes_to_frames(runtime,
				((prtd->dma_level_count - last_dma_count_level
						- prtd->remain_count)
						% runtime->dma_bytes));

		//spin_unlock(&prtd->lock);
		spin_unlock_irqrestore(&prtd->lock, flags);
	} else if (SNDRV_PCM_STREAM_CAPTURE == substream->stream) {
		spin_lock_irqsave(&prtd->lock, flags);

		last_dma_count_level = InfinityDmaGetLevelCnt(
				dma_data->channel);
		//udelay(5000);

		/*
		 if (last_dma_count_level <= 64)
		 last_dma_count_level = 0;
		 else
		 last_dma_count_level = last_dma_count_level - 64;
		 */

		if (prtd->dma_level_count > runtime->dma_bytes * 2)
			prtd->dma_level_count -= runtime->dma_bytes;
		offset = bytes_to_frames(runtime,
				((prtd->dma_level_count + last_dma_count_level
						- prtd->remain_count)
						% runtime->dma_bytes));
		spin_unlock_irqrestore(&prtd->lock, flags);
	}

	//AUD_PRINTF(TRACE_LEVEL, "last_dma_count_level = 0x%x, offset(bytes) = 0x%x, dma size byte = 0x%x, buffer_size = 0x%x\n", last_dma_count_level, offset*4, runtime->dma_bytes, runtime->buffer_size);
	return offset;
#endif
	return 0;
}

#if 0
static int infinity_pcm_copy(struct snd_pcm_substream *substream, int channel,
		snd_pcm_uframes_t pos, void __user *dst,
		snd_pcm_uframes_t count) {
	struct snd_pcm_runtime *runtime = substream->runtime;
//	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct infinity_pcm_runtime_data *prtd = runtime->private_data;
	struct infinity_pcm_dma_data *dma_data = prtd->dma_data;
	unsigned long flags;
	char *hwbuf = runtime->dma_area + frames_to_bytes(runtime, pos);

	unsigned long nCnt, nLevelCnt;

	// AUD_PRINTF(TRACE_LEVEL, "BACH_DMA1_CTRL_0 = 0x%x,  BACH_DMA1_CTRL_8 = 0x%x, level count = %d, need bytes = %d\n",
	// 		InfinityReadReg(BACH_REG_BANK1, BACH_DMA1_CTRL_0), InfinityReadReg(BACH_REG_BANK1, BACH_DMA1_CTRL_8), InfinityDmaGetLevelCnt(prtd->dma_data->channel),frames_to_bytes(runtime, count));

	if (SNDRV_PCM_STREAM_PLAYBACK == substream->stream) {
		if (copy_from_user(hwbuf, dst, frames_to_bytes(runtime, count)))
			return -EFAULT;

		//spin_lock(&prtd->lock);
		spin_lock_irqsave(&prtd->lock, flags);
		mb();
		nLevelCnt = InfinityDmaGetLevelCnt(dma_data->channel);
		nCnt = InfinityDmaTrigLevelCnt(dma_data->channel,
				(frames_to_bytes(runtime, count)
						+ prtd->remain_count));
		if (prtd->state == DMA_EMPTY) {
			InfinityDmaClearInt(dma_data->channel);
			prtd->state = DMA_NORMAL;
		}

		/*if(bPlayFirst)
		 {
		 AUD_PRINTF(ERROR_LEVEL, "Play Time = %llu,offset %d,count %d\n",Chip_Get_US_Ticks(),frames_to_bytes(runtime, pos),frames_to_bytes(runtime, count));
		 // g_nPlayStartTime = Chip_Get_US_Ticks();
		 bPlayFirst = 0;
		 }*/

		if (((nLevelCnt + nCnt)
				>= (runtime->dma_bytes
						- frames_to_bytes(runtime,
								runtime->period_size)))
				&& snd_pcm_running(substream)) {
			InfinityDmaMaskInt(dma_data->channel,
					BACH_DMA_INT_UNDERRUN, false);
			prtd->state = DMA_NORMAL;
		}

		prtd->dma_level_count += frames_to_bytes(runtime, count);
		prtd->remain_count = (frames_to_bytes(runtime, count)
				+ prtd->remain_count) - nCnt;

		if (prtd->remain_count)

#if 0
    if (DmaIsEmpty(dma_data->channel))
    {
      DmaMaskInt(dma_data->channel, BACH_DMA_INT_UNDERRUN, false);
      DmaMaskInt(dma_data->channel, BACH_DMA_INT_EMPTY, false);
    }
#endif
			//spin_unlock(&prtd->lock);
			spin_unlock_irqrestore(&prtd->lock, flags);
	} else if (SNDRV_PCM_STREAM_CAPTURE == substream->stream) {
		if (copy_to_user(dst, hwbuf, frames_to_bytes(runtime, count)))
			return -EFAULT;

		spin_lock_irqsave(&prtd->lock, flags);
		nCnt = InfinityDmaTrigLevelCnt(dma_data->channel,
				frames_to_bytes(runtime, count)
						+ prtd->remain_count);
		if (prtd->state == DMA_FULL) {
			InfinityDmaClearInt(dma_data->channel);
			prtd->state = DMA_NORMAL;
		}

		/*if(bCapFirst)
		 {
		 AUD_PRINTF(ERROR_LEVEL, "Cap Time = %llu\n",Chip_Get_US_Ticks());
		 //g_nCapStartTime = Chip_Get_US_Ticks();
		 bCapFirst = 0;
		 }*/

		/*
		 if ((prtd->dma_level_count%runtime->dma_bytes) != frames_to_bytes(runtime, pos))
		 AUD_PRINTF(PCM_LEVEL, "%s: dma_level_count = %d, pos_bytes = %d\n",
		 __FUNCTION__, prtd->dma_level_count, frames_to_bytes(runtime, pos));
		 */

		prtd->dma_level_count += frames_to_bytes(runtime, count);
		prtd->remain_count = (frames_to_bytes(runtime, count)
				+ prtd->remain_count) - nCnt;

		// if (prtd->remain_count)
		//   AUD_PRINTF(PCM_LEVEL, "%s: stream id = %d, channel id = %d, cnt = 0x%x, frame_bytes = 0x%x\n",
		//              __FUNCTION__, substream->stream, dma_data->channel, (unsigned int)nCnt, (unsigned int)frames_to_bytes(runtime, count));

		spin_unlock_irqrestore(&prtd->lock, flags);
	}

	// AUD_PRINTF(TRACE_LEVEL, "Out BACH_DMA1_CTRL_0 = 0x%x,  BACH_DMA1_CTRL_8 = 0x%x, level count = %d\n",
	//		InfinityReadReg(BACH_REG_BANK1, BACH_DMA1_CTRL_0), InfinityReadReg(BACH_REG_BANK1, BACH_DMA1_CTRL_8), InfinityDmaGetLevelCnt(prtd->dma_data->channel));

	return 0;
}
#endif

static struct snd_pcm_ops infinity_pcm_ops = { .open = infinity_pcm_open,
		.close = infinity_pcm_close, .ioctl = snd_pcm_lib_ioctl,
		.hw_params = infinity_pcm_hw_params, .hw_free =
				infinity_pcm_hw_free, .prepare =
				infinity_pcm_prepare, .trigger =
				infinity_pcm_trigger, .pointer =
				infinity_pcm_pointer,
//.copy = infinity_pcm_copy,
		};

static u64 infinity_pcm_dmamask = DMA_BIT_MASK(64);

static int infinity_pcm_preallocate_dma_buffer(struct snd_pcm *pcm, int stream) {
#if 0
	char name[16];

	struct snd_pcm_substream *substream = pcm->streams[stream].substream;
	struct snd_dma_buffer *buf = &substream->dma_buffer;
	size_t size = 0;

	if (SNDRV_PCM_STREAM_PLAYBACK == stream) {
		size = infinity_pcm_playback_hardware.buffer_bytes_max;
		snprintf(name, 16, "pcmC%dD%dp", pcm->card->number,
				pcm->device);
	} else if (SNDRV_PCM_STREAM_CAPTURE == stream)  // CAPTURE device
			{
		size = infinity_pcm_capture_hardware.buffer_bytes_max;
		snprintf(name, 16, "pcmC%dD%dc", pcm->card->number,
				pcm->device);
	} else
		return -EINVAL;

	buf->dev.type = SNDRV_DMA_TYPE_DEV;
	buf->dev.dev = pcm->card->dev;
	buf->private_data = NULL;
	// buf->area = dma_alloc_coherent(pcm->card->dev, PAGE_ALIGN(size),
	//                                &buf->addr, GFP_KERNEL);

	buf->area = alloc_dmem(name, PAGE_ALIGN(size), &buf->addr);

	if (!buf->area)
		return -ENOMEM;
	buf->bytes = PAGE_ALIGN(size);

	return 0;
#endif
	return 0;
}

static void infinity_pcm_free_dma_buffers(struct snd_pcm *pcm) {
#if 0
	struct snd_pcm_substream *substream;
	struct snd_dma_buffer *buf;
	int stream;
	char name[16];

	for (stream = 0; stream < 2; stream++) {
		substream = pcm->streams[stream].substream;
		if (!substream)
			continue;

		buf = &substream->dma_buffer;
		if (!buf->area)
			continue;

		if (SNDRV_PCM_STREAM_PLAYBACK == stream) {
			snprintf(name, 16, "pcmC%dD%dp", pcm->card->number,
					pcm->device);
		} else if (SNDRV_PCM_STREAM_CAPTURE == stream) // CAPTURE device
				{
			snprintf(name, 16, "pcmC%dD%dc", pcm->card->number,
					pcm->device);
		} else
			return;

		//dma_free_coherent(pcm->card->dev, buf->bytes,
		//
		free_dmem(name, buf->bytes, buf->area, buf->addr);
		buf->area = NULL;
	}
#endif
}

static int infinity_pcm_new(struct snd_soc_pcm_runtime *rtd) {
#if 0
	struct snd_card *card = rtd->card->snd_card;
	struct snd_pcm *pcm = rtd->pcm;
	//struct snd_pcm_substream *substream = NULL;
	//struct snd_dma_buffer *buf = NULL;
	int ret = 0;

	if (!card->dev->dma_mask)
		card->dev->dma_mask = &infinity_pcm_dmamask;
	if (!card->dev->coherent_dma_mask)
		card->dev->coherent_dma_mask = DMA_BIT_MASK(64);

	if (pcm->streams[SNDRV_PCM_STREAM_PLAYBACK].substream) {
		ret = infinity_pcm_preallocate_dma_buffer(pcm,
				SNDRV_PCM_STREAM_PLAYBACK);
		if (ret)
			goto out;
	}

	if (pcm->streams[SNDRV_PCM_STREAM_CAPTURE].substream) {
		ret = infinity_pcm_preallocate_dma_buffer(pcm,
				SNDRV_PCM_STREAM_CAPTURE);
		if (ret)
			goto out;
	}

	out:
	/* free preallocated buffers in case of error */
	if (ret)
		infinity_pcm_free_dma_buffers(pcm);

	return ret;
#endif
	return 0;
}

struct infinity_pcm_data {
	struct regmap *bach;
};

static void infinity_pcm_dma_reset(struct infinity_pcm_data *data) {
	//reset DMA1 registers
	regmap_write(data->bach, MSTAR_BACH_REG_DMA1_CTRL_4 , 0);//reset DMA 1 read size
	regmap_write(data->bach, MSTAR_BACH_REG_DMA1_CTRL_12, 0);//reset DMA 1 write size

	/*InfinityWriteReg(BACH_REG_BANK1, BACH_DMA1_CTRL_0, REG_SW_RST_DMA,
	REG_SW_RST_DMA);		//DMA 1 software reset
	InfinityWriteReg(BACH_REG_BANK1, BACH_DMA1_CTRL_0, REG_SW_RST_DMA, 0);

	InfinityWriteReg(BACH_REG_BANK1, BACH_DMA1_CTRL_0,
			(REG_PRIORITY_KEEP_HIGH | REG_RD_LEVEL_CNT_LIVE_MASK),
			(REG_PRIORITY_KEEP_HIGH | REG_RD_LEVEL_CNT_LIVE_MASK));

	//enable DMA interrupt
	InfinityWriteReg(BACH_REG_BANK2, BACH_INT_EN, REG_DMA_INT_EN,
	REG_DMA_INT_EN);*/

}

#if 0
/**
 * \brief clear DMA2 interrupt
 */
void InfinityDmaClearInt(BachDmaChannel_e eDmaChannel)
{
    switch(eDmaChannel)
    {

    case BACH_DMA_WRITER1:
        //DMA writer full flag clear / DMA writer local buffer full flag clear
        InfinityWriteReg(BACH_REG_BANK1, BACH_DMA1_CTRL_0, REG_WR_FULL_FLAG_CLR, REG_WR_FULL_FLAG_CLR);
        InfinityWriteReg(BACH_REG_BANK1, BACH_DMA1_CTRL_0, REG_WR_FULL_FLAG_CLR, 0);
        break;

    case BACH_DMA_READER1:
        //DMA reader empty flag clear / DMA reader local buffer empty flag clear
        InfinityWriteReg(BACH_REG_BANK1, BACH_DMA1_CTRL_0, REG_RD_EMPTY_FLAG_CLR, REG_RD_EMPTY_FLAG_CLR);
        InfinityWriteReg(BACH_REG_BANK1, BACH_DMA1_CTRL_0, REG_RD_EMPTY_FLAG_CLR, 0);

        break;

    default:
        ERRMSG("InfinityDmaClearInt - ERROR bank default case!\n");
        break;
    }

    return;
}

U32 InfinityDmaGetLevelCnt(BachDmaChannel_e eDmaChannel)
{
    U16 nConfigValue = 0;
    U32 nByteSize = 0;

    switch(eDmaChannel)
    {
    case BACH_DMA_WRITER1:
    {

        InfinityWriteReg(BACH_REG_BANK1, BACH_DMA1_CTRL_9, REG_WR_LEVEL_CNT_MASK, REG_WR_LEVEL_CNT_MASK);
        nConfigValue = InfinityReadReg(BACH_REG_BANK1, BACH_DMA1_CTRL_15);
        InfinityWriteReg(BACH_REG_BANK1, BACH_DMA1_CTRL_9, REG_WR_LEVEL_CNT_MASK, 0);
        nConfigValue = ((nConfigValue>8)? (nConfigValue-8):0); //level count contains the local buffer data size
        break;
    }

    case BACH_DMA_READER1:
    {

        InfinityWriteReg(BACH_REG_BANK1, BACH_DMA1_CTRL_1, REG_RD_LEVEL_CNT_MASK, REG_RD_LEVEL_CNT_MASK);
        nConfigValue = InfinityReadReg(BACH_REG_BANK1, BACH_DMA1_CTRL_7);
        InfinityWriteReg(BACH_REG_BANK1, BACH_DMA1_CTRL_1, REG_RD_LEVEL_CNT_MASK, 0);


        break;
    }

    default:
        ERRMSG("InfinityDmaGetLevelCnt - ERROR bank default case!\n");
        return 0;

    }

    nByteSize = nConfigValue * MIU_WORD_BYTE_SIZE;

    return nByteSize;
}

void InfinityDmaSetThreshold(BachDmaChannel_e eDmaChannel, U32 nOverrunTh, U32 nUnderrunTh)
{
    U16 nMiuOverrunTh, nMiuUnderrunTh;

    switch(eDmaChannel)
    {
    case BACH_DMA_WRITER1:
        nMiuOverrunTh = (U16)((nOverrunTh / MIU_WORD_BYTE_SIZE) & REG_WR_OVERRUN_TH_MSK);
        nMiuUnderrunTh = (U16)((nUnderrunTh / MIU_WORD_BYTE_SIZE) & REG_WR_UNDERRUN_TH_MSK);
        InfinityWriteReg(BACH_REG_BANK1, BACH_DMA1_CTRL_13, 0xFFFF, nMiuOverrunTh);
        InfinityWriteReg(BACH_REG_BANK1, BACH_DMA1_CTRL_14, 0xFFFF, nMiuUnderrunTh);
        break;

    case BACH_DMA_READER1:

        nMiuOverrunTh = (U16)((nOverrunTh / MIU_WORD_BYTE_SIZE) & REG_RD_OVERRUN_TH_MSK);
        nMiuUnderrunTh = (U16)((nUnderrunTh / MIU_WORD_BYTE_SIZE) & REG_RD_UNDERRUN_TH_MSK);
        InfinityWriteReg(BACH_REG_BANK1, BACH_DMA1_CTRL_5, 0xFFFF, nMiuOverrunTh);
        InfinityWriteReg(BACH_REG_BANK1, BACH_DMA1_CTRL_6, 0xFFFF, nMiuUnderrunTh);
        break;

    default:
        ERRMSG("InfinityDmaSetThreshold - ERROR bank default case!\n");
        break;
    }

}

/**
 * \brief DMA set MIU address
 */
void InfinityDmaSetPhyAddr(BachDmaChannel_e eDmaChannel, U32 nBufAddrOffset, U32 nBufSize)
{
    U16 nMiuAddrLo, nMiuAddrHi, nMiuSize;
    //U32 nOffset = nBufAddr & ~0xf0000000;		//transfer to miu bus address

    ///nOffset = HalUtilPHY2MIUAddr(nBufAddr);
    //nOffset = getSysPhyAddr(nBufAddr);
    //nOffset = nBufAddr - 0x40000000;

    //printf("HalBachDma2SetWrMiuAddr nBufAddr:%x, nBufSize:%x, PHYaddress:%x\n",nBufAddr,nBufSize,nOffset);
    ///nMiuAddrLo = (nBufAddr) & REG_WR_BASE_ADDR_LO_MSK;
    ///nMiuAddrHi = ((nBufAddr) >> REG_WR_BASE_ADDR_HI_OFFSET) & REG_WR_BASE_ADDR_HI_MSK;
    ///nMiuSize = (nBufSize / MIU_WORD_BYTE_SIZE) & REG_WR_BUFF_SIZE_MSK;

    //TRACE2("BachDmaSetPhyAddr() MIU addr = 0x%x, size = 0x%x", nOffset, nBufSize);

    switch(eDmaChannel)
    {
    case BACH_DMA_WRITER1:
        nMiuAddrLo = (U16)((nBufAddrOffset / MIU_WORD_BYTE_SIZE) & REG_WR_BASE_ADDR_LO_MSK);
        nMiuAddrHi = (U16)(((nBufAddrOffset / MIU_WORD_BYTE_SIZE) >> REG_WR_BASE_ADDR_HI_OFFSET) & REG_WR_BASE_ADDR_HI_MSK);
        nMiuSize = (U16)((nBufSize / MIU_WORD_BYTE_SIZE) & REG_WR_BUFF_SIZE_MSK);


        InfinityWriteReg(BACH_REG_BANK1, BACH_DMA1_CTRL_9, REG_WR_BASE_ADDR_LO_MSK, nMiuAddrLo);
        InfinityWriteReg(BACH_REG_BANK1, BACH_DMA1_CTRL_10, REG_WR_BASE_ADDR_HI_MSK, nMiuAddrHi);
        InfinityWriteReg(BACH_REG_BANK1, BACH_DMA1_CTRL_11, 0xFFFF, nMiuSize);

        break;

    case BACH_DMA_READER1:

        nMiuAddrLo = (U16)((nBufAddrOffset / MIU_WORD_BYTE_SIZE) & REG_RD_BASE_ADDR_LO_MSK);
        nMiuAddrHi = (U16)(((nBufAddrOffset / MIU_WORD_BYTE_SIZE) >> REG_RD_BASE_ADDR_HI_OFFSET) & REG_RD_BASE_ADDR_HI_MSK);
        nMiuSize = (U16)((nBufSize / MIU_WORD_BYTE_SIZE) & REG_RD_BUFF_SIZE_MSK);

        InfinityWriteReg(BACH_REG_BANK1, BACH_DMA1_CTRL_1, REG_RD_BASE_ADDR_LO_MSK, nMiuAddrLo);
        InfinityWriteReg(BACH_REG_BANK1, BACH_DMA1_CTRL_2, REG_RD_BASE_ADDR_HI_MSK, nMiuAddrHi);
        InfinityWriteReg(BACH_REG_BANK1, BACH_DMA1_CTRL_3, 0xFFFF, nMiuSize);

        break;

    default:
        ERRMSG("InfinityDmaSetPhyAddr - ERROR bank default case!\n");
        break;

    }

}

bool InfinityDmaMaskInt(BachDmaChannel_e eDmaChan, BachDmaInterrupt_e eDmaInt, bool bMask)
{
    switch(eDmaChan)
    {
    case BACH_DMA_READER1:
        InfinityWriteReg(BACH_REG_BANK1, BACH_DMA1_CTRL_0,
                         g_nInfinityDmaIntReg[BACH_DMA_READER1][eDmaInt], (bMask ? 0 : g_nInfinityDmaIntReg[BACH_DMA_READER1][eDmaInt]));
        break;

    case BACH_DMA_WRITER1:
        InfinityWriteReg(BACH_REG_BANK1, BACH_DMA1_CTRL_0,
                         g_nInfinityDmaIntReg[BACH_DMA_WRITER1][eDmaInt], (bMask ? 0 : g_nInfinityDmaIntReg[BACH_DMA_WRITER1][eDmaInt]));
        break;

    default:
        return false;
    }

    return true;
}

bool InfinityDmaIsFull(BachDmaChannel_e eDmaChannel)
{
    U16 nConfigValue;

    switch(eDmaChannel)
    {
    case BACH_DMA_WRITER1:
        nConfigValue = InfinityReadReg(BACH_REG_BANK1, BACH_DMA1_CTRL_8);
        return (nConfigValue & REG_WR_FULL_FLAG) ? true : false;

    default:
        return false;
    }
}

bool InfinityDmaIsEmpty(BachDmaChannel_e eDmaChannel)
{
    U16 nConfigValue;

    switch(eDmaChannel)
    {
    case BACH_DMA_READER1:
        nConfigValue = InfinityReadReg(BACH_REG_BANK1, BACH_DMA1_CTRL_8);
        return (nConfigValue & REG_RD_EMPTY_FLAG) ? true : false;

    default:
        return false;
    }
}

bool InfinityDmaIsLocalEmpty(BachDmaChannel_e eDmaChannel)
{
    U16 nConfigValue;

    switch(eDmaChannel)
    {
    case BACH_DMA_READER1:
        nConfigValue = InfinityReadReg(BACH_REG_BANK1, BACH_DMA1_CTRL_8);
        return (nConfigValue & REG_RD_LOCALBUF_EMPTY) ? true : false;

    default:
        return false;
    }
}


bool InfinityDmaIsUnderrun(BachDmaChannel_e eDmaChannel)
{
    U16 nConfigValue;

    switch(eDmaChannel)
    {
    case BACH_DMA_WRITER1:
        nConfigValue = InfinityReadReg(BACH_REG_BANK1, BACH_DMA1_CTRL_8);
        return (nConfigValue & REG_WR_UNDERRUN_FLAG) ? true : false;

    case BACH_DMA_READER1:
        nConfigValue = InfinityReadReg(BACH_REG_BANK1, BACH_DMA1_CTRL_8);
        return (nConfigValue & REG_RD_UNDERRUN_FLAG) ? true : false;

    default:
        ERRMSG("InfinityDmaIsUnderrun - ERROR default case!\n");
        return false;
    }

    return false;
}

bool InfinityDmaIsOverrun(BachDmaChannel_e eDmaChannel)
{
    U16 nConfigValue;

    switch(eDmaChannel)
    {
    case BACH_DMA_WRITER1:
        nConfigValue = InfinityReadReg(BACH_REG_BANK1, BACH_DMA1_CTRL_8);
        return (nConfigValue & REG_WR_OVERRUN_FLAG) ? true : false;

    case BACH_DMA_READER1:
        nConfigValue = InfinityReadReg(BACH_REG_BANK1, BACH_DMA1_CTRL_8);
        return (nConfigValue & REG_RD_OVERRUN_FLAG) ? true : false;

    default:
        return false;
    }

    return false;
}

U32 InfinityDmaTrigLevelCnt(BachDmaChannel_e eDmaChannel, U32 nDataSize)
{
    U16 nConfigValue = 0;

    nConfigValue = (U16)((nDataSize / MIU_WORD_BYTE_SIZE) & REG_WR_SIZE_MSK);
    nDataSize = nConfigValue * MIU_WORD_BYTE_SIZE;

    if (nConfigValue > 0)
    {
        switch(eDmaChannel)
        {
        case BACH_DMA_WRITER1:
            InfinityWriteReg(BACH_REG_BANK1, BACH_DMA1_CTRL_12, 0xFFFF, nConfigValue);
            nConfigValue = InfinityReadReg(BACH_REG_BANK1, BACH_DMA1_CTRL_9);
            if(nConfigValue & REG_WR_TRIG)
            {
                InfinityWriteReg(BACH_REG_BANK1, BACH_DMA1_CTRL_9, REG_WR_TRIG, 0);
            }
            else
            {
                InfinityWriteReg(BACH_REG_BANK1, BACH_DMA1_CTRL_9, REG_WR_TRIG, REG_WR_TRIG);
            }

            break;

        case BACH_DMA_READER1:
            InfinityWriteReg(BACH_REG_BANK1, BACH_DMA1_CTRL_4, 0xFFFF, nConfigValue);
            nConfigValue = InfinityReadReg(BACH_REG_BANK1, BACH_DMA1_CTRL_1);
            if(nConfigValue & REG_RD_TRIG)
            {
                InfinityWriteReg(BACH_REG_BANK1, BACH_DMA1_CTRL_1, REG_RD_TRIG, 0);
            }
            else
            {
                InfinityWriteReg(BACH_REG_BANK1, BACH_DMA1_CTRL_1, REG_RD_TRIG, REG_RD_TRIG);
            }
            break;

        default:
            ERRMSG("InfinityDmaTrigLevelCnt - ERROR bank default case!\n");
            return 0;
        }

        return nDataSize;
    }

    return 0;

}

//------------------------------------------------------------------------------
//
//  Function:   BachC3::MapHardware
//
//  Description
//      Maps port address, assigns SysIntr.
//
//  Parameters
//      u32RegAddr:     [in] Physical address of audio hardware register.
//      u32RegLength:   [in] Length of the address space of audio hardware register.
//
//  Return Value
//      Returns true if device was mapped properly
//      Return false if device could not be mapped
//------------------------------------------------------------------------------
void InfinityDmaInitChannel( U32 nChannelIndex,
                             U32 nPhysDMAAddr,
                             U32 nBufferSize,
                             U32 nChannels,
                             U32 nSampleSize,
                             U32 nSampleRate,
                             U32 nOverrunTh,
                             U32 nUnderrunTh
                           )
{
    //U16 nConfigValue;

    // save off the info for power managment
    m_infinitydmachannel[nChannelIndex].nPhysDMAAddr = nPhysDMAAddr;
    m_infinitydmachannel[nChannelIndex].nBufferSize  = nBufferSize;
    m_infinitydmachannel[nChannelIndex].nChannels   = nChannels;
    m_infinitydmachannel[nChannelIndex].nSampleSize = nSampleSize;
    m_infinitydmachannel[nChannelIndex].nSampleRate = nSampleRate;


    // Set up the physical DMA buffer address
    InfinityDmaSetPhyAddr((BachDmaChannel_e)nChannelIndex, nPhysDMAAddr, nBufferSize);


    // Set up the underrun and overrun
    //DmaSetThreshold((BachDmaChannel_e)nChannelIndex, nBufferSize+MIU_WORD_BYTE_SIZE, (nBufferSize/4)*3);
    InfinityDmaSetThreshold((BachDmaChannel_e)nChannelIndex, nOverrunTh, nUnderrunTh);

    // Set up channel mode
    InfinityDmaSetChMode((BachDmaChannel_e)nChannelIndex, (nChannels==1 ? true:false));

    return ;
}



void InfinityDmaReInit(BachDmaChannel_e eDmaChannel)
{
    switch ( eDmaChannel )
    {
    case BACH_DMA_READER1:
        InfinityWriteReg(BACH_REG_BANK1, BACH_DMA1_CTRL_1, REG_RD_TRIG, 0); // prevent from triggering levelcount at toggling init step
        InfinityWriteReg(BACH_REG_BANK1, BACH_DMA1_CTRL_1, REG_RD_INIT, REG_RD_INIT);
        InfinityWriteReg(BACH_REG_BANK1, BACH_DMA1_CTRL_1, REG_RD_INIT, 0);
        break;

    case BACH_DMA_WRITER1:
        InfinityWriteReg(BACH_REG_BANK1, BACH_DMA1_CTRL_9, REG_WR_TRIG, 0);
        InfinityWriteReg(BACH_REG_BANK1, BACH_DMA1_CTRL_9, REG_WR_INIT, REG_WR_INIT);
        InfinityWriteReg(BACH_REG_BANK1, BACH_DMA1_CTRL_9, REG_WR_INIT, 0);
        break;

    default:
        ERRMSG("InfinityDmaReInit - ERROR bank default case!\n");
        break;
    }

}

void InfinityDmaEnable(BachDmaChannel_e eDmaChannel, bool bEnable)
{
    switch ( eDmaChannel )
    {
    case BACH_DMA_READER1:

        InfinityWriteReg(BACH_REG_BANK1, BACH_DMA1_CTRL_0,
                         (REG_RD_EMPTY_INT_EN | REG_RD_UNDERRUN_INT_EN),
                         (bEnable ? (REG_RD_EMPTY_INT_EN | REG_RD_UNDERRUN_INT_EN) : 0));

        if(bEnable)
        {
            InfinityWriteReg(BACH_REG_BANK1, BACH_DMA1_CTRL_0, REG_ENABLE, REG_ENABLE); //reader prefetch enable, it should be enabled before reader enable
            udelay(10);
            InfinityWriteReg(BACH_REG_BANK1, BACH_DMA1_CTRL_1, REG_RD_ENABLE, REG_RD_ENABLE);
        }
        else
        {
            InfinityWriteReg(BACH_REG_BANK1, BACH_DMA1_CTRL_1, REG_RD_ENABLE, 0);
            InfinityWriteReg(BACH_REG_BANK1, BACH_DMA1_CTRL_0, REG_ENABLE, 0); //reader prefetch enable, it has to be disabled before dma init
        }

        break;

    case BACH_DMA_WRITER1:

        InfinityWriteReg(BACH_REG_BANK1, BACH_DMA1_CTRL_0,
                         (REG_WR_FULL_INT_EN | REG_WR_OVERRUN_INT_EN),
                         (bEnable ? (REG_WR_FULL_INT_EN | REG_WR_OVERRUN_INT_EN) : 0));

        InfinityWriteReg(BACH_REG_BANK1, BACH_DMA1_CTRL_9, REG_WR_ENABLE, (bEnable ? REG_WR_ENABLE : 0));

        break;

    default:
        ERRMSG("InfinityDmaEnable - ERROR bank default case!\n");
        break;

    }

}

void InfinityDmaStartChannel(BachDmaChannel_e eDmaChannel)
{
//TRACE1("BachDmaStartChannel %d",eDmaChannel);
    //DmaReInit(eDmaChannel);
    InfinityDmaClearInt(eDmaChannel);
    InfinityDmaEnable(eDmaChannel, true);
    m_infinitydmachannel[eDmaChannel].nDMAChannelState = DMA_RUNNING;  // save the state
    return;
}

void InfinityDmaStopChannel(BachDmaChannel_e eDmaChannel)
{
    InfinityDmaEnable(eDmaChannel, false);
    InfinityDmaReInit(eDmaChannel);

    //Sleep(100);
    // save the state
    m_infinitydmachannel[eDmaChannel].nDMAChannelState = DMA_STOPPED;
    return;
}

#endif

static int infinity_pcm_probe(struct snd_soc_component *platform) {
	struct infinity_pcm_data *data;

	data = devm_kzalloc(platform->dev, sizeof(*data), GFP_KERNEL);
	if (IS_ERR(data))
		return PTR_ERR(data);

	data->bach = syscon_regmap_lookup_by_phandle(platform->dev->of_node,
			"mstar,bach");
	if (IS_ERR(data->bach))
		return PTR_ERR(data->bach);

	snd_soc_component_set_drvdata(platform, data);

	infinity_pcm_dma_reset(data);
	return 0;
}

static void infinity_pcm_remove(struct snd_soc_component *platform) {
}

static int infinity_pcm_suspend(struct snd_soc_component *dai) {
	return 0;
}

static int infinity_pcm_resume(struct snd_soc_component *dai) {
#if 0
	struct snd_pcm_substream *substream;
	struct snd_pcm_runtime *runtime;
	struct infinity_pcm_runtime_data *prtd;
	struct infinity_pcm_dma_data *dma_data;
	struct snd_soc_pcm_runtime *rtd;
	//struct snd_soc_codec *codec;

	int stream, i = 0;

	InfinityDmaReset();

	for (i = 0; i < dai->card->num_rtd; i++) {
		if (dai->card->rtd[i].dai_link->ignore_suspend)
			continue;

		for (stream = 0; stream < 2; stream++) {
			for (substream =
					dai->card->rtd[i].pcm->streams[stream].substream;
					substream; substream =
							substream->next) {
				/* FIXME: the open/close code should lock this as well */
				if (substream->runtime == NULL)
					continue;

				runtime = substream->runtime;
				prtd = runtime->private_data;
				rtd = substream->private_data;
				//codec = rtd->codec;

				if (prtd->dma_data) {
					dma_data = prtd->dma_data;
					/*
					 * Link channel with itself so DMA doesn't need any
					 * reprogramming while looping the buffer
					 */

					InfinityDmaSetRate(dma_data->channel,
							InfinityRateFromU32(
									runtime->rate));

					//SysInit();
					//DmaReset();
					//DmaReInit(dma_data->channel);
					InfinityDmaInitChannel(
							dma_data->channel,
							(runtime->dma_addr
									- MIU0_OFFSET),
							runtime->dma_bytes,
							runtime->channels,
							snd_pcm_format_physical_width(
									runtime->format),
							runtime->rate,
							runtime->dma_bytes * 3
									/ 4,
							runtime->dma_bytes * 1
									/ 4);

					// Re-set up the underrun and overrun
					if (substream->stream
							== SNDRV_PCM_STREAM_PLAYBACK) {
						InfinityDmaSetThreshold(
								(BachDmaChannel_e) dma_data->channel,
								(runtime->dma_bytes
										+ 1024),
								(runtime->dma_bytes
										- frames_to_bytes(
												runtime,
												runtime->period_size)));

					} else {
						InfinityDmaSetThreshold(
								(BachDmaChannel_e) dma_data->channel,
								frames_to_bytes(
										runtime,
										runtime->period_size),
								256);
					}

					memset(runtime->dma_area, 0,
							runtime->dma_bytes);
					prtd->dma_level_count = 0;
					prtd->remain_count = 0;
					prtd->state = 0; //EMPTY
				}

			}
		}

	}

	return 0;
#endif
	return 0;
}

static const struct snd_soc_component_driver infinity_pcm = {
		.probe = infinity_pcm_probe,
		.remove = infinity_pcm_remove,
		.suspend = infinity_pcm_suspend,
		.resume = infinity_pcm_resume,
		/*.ops = &infinity_pcm_ops,
		.pcm_new = infinity_pcm_new,
		.pcm_free = infinity_pcm_free_dma_buffers,*/
};

static int infinity_pcm_drv_probe(struct platform_device *pdev) {
	return snd_soc_register_component(&pdev->dev, &infinity_pcm,
	NULL, 0);
}

static int infinity_pcm_drv_remove(struct platform_device *pdev) {
	snd_soc_unregister_component(&pdev->dev);
	return 0;
}

static const struct of_device_id infinity_pcm_of_match[] = {
	{
		.compatible = "mstar,snd-infinity-pcm",
	},
	{ },
};
MODULE_DEVICE_TABLE(of, infinity_pcm_of_match);

static struct platform_driver infinity_pcm_driver = {
	.driver = {
			.name = "infinity-pcm",
			.owner = THIS_MODULE,
			.of_match_table = infinity_pcm_of_match,
	},
	.probe = infinity_pcm_drv_probe,
	.remove = infinity_pcm_drv_remove,
};

module_platform_driver(infinity_pcm_driver);

MODULE_AUTHOR("Roger Lai, roger.lai@mstarsemi.com");
MODULE_DESCRIPTION("iNfinity Bach PCM DMA module");
MODULE_LICENSE("GPL v2");
