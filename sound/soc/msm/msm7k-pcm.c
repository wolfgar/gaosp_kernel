/* linux/sound/soc/msm/msm7k-pcm.c
 *
 * Copyright (c) 2008-2009, Code Aurora Forum. All rights reserved.
 *
 * All source code in this file is licensed under the following license except
 * where indicated.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you can find it at http://www.fsf.org.
 */



#include <linux/init.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/time.h>
#include <linux/wait.h>
#include <linux/platform_device.h>
#include <sound/core.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/pcm.h>
#include <sound/initval.h>
#include <sound/control.h>
#include <asm/dma.h>
#include <linux/dma-mapping.h>

#include "msm-pcm.h"


#define SND_DRIVER        "snd_msm"
#define MAX_PCM_DEVICES	SNDRV_CARDS
#define MAX_PCM_SUBSTREAMS 1

struct snd_msm {
	struct snd_card *card;
	struct snd_pcm *pcm;
};

int copy_count;
int intcnt;

struct audio_locks the_locks;
struct msm_volume msm_vol_ctl;


static unsigned convert_dsp_samp_index(unsigned index)
{
	switch (index) {
	case 48000:
		return AUDREC_CMD_SAMP_RATE_INDX_48000;
	case 44100:
		return AUDREC_CMD_SAMP_RATE_INDX_44100;
	case 32000:
		return AUDREC_CMD_SAMP_RATE_INDX_32000;
	case 24000:
		return AUDREC_CMD_SAMP_RATE_INDX_24000;
	case 22050:
		return AUDREC_CMD_SAMP_RATE_INDX_22050;
	case 16000:
		return AUDREC_CMD_SAMP_RATE_INDX_16000;
	case 12000:
		return AUDREC_CMD_SAMP_RATE_INDX_12000;
	case 11025:
		return AUDREC_CMD_SAMP_RATE_INDX_11025;
	case 8000:
		return AUDREC_CMD_SAMP_RATE_INDX_8000;
	default:
		return AUDREC_CMD_SAMP_RATE_INDX_44100;
	}
}

static unsigned convert_samp_rate(unsigned hz)
{
	switch (hz) {
	case 48000:
		return RPC_AUD_DEF_SAMPLE_RATE_48000;
	case 44100:
		return RPC_AUD_DEF_SAMPLE_RATE_44100;
	case 32000:
		return RPC_AUD_DEF_SAMPLE_RATE_32000;
	case 24000:
		return RPC_AUD_DEF_SAMPLE_RATE_24000;
	case 22050:
		return RPC_AUD_DEF_SAMPLE_RATE_22050;
	case 16000:
		return RPC_AUD_DEF_SAMPLE_RATE_16000;
	case 12000:
		return RPC_AUD_DEF_SAMPLE_RATE_12000;
	case 11025:
		return RPC_AUD_DEF_SAMPLE_RATE_11025;
	case 8000:
		return RPC_AUD_DEF_SAMPLE_RATE_8000;
	default:
		return RPC_AUD_DEF_SAMPLE_RATE_44100;
	}
}

static struct snd_pcm_hardware msm_pcm_playback_hardware = {
	.info =                 SNDRV_PCM_INFO_INTERLEAVED,
	.formats =              USE_FORMATS,
	.rates =                USE_RATE,
	.rate_min =             USE_RATE_MIN,
	.rate_max =             USE_RATE_MAX,
	.channels_min =         USE_CHANNELS_MIN,
	.channels_max =         USE_CHANNELS_MAX,
	.buffer_bytes_max =     MAX_BUFFER_PLAYBACK_SIZE,
	.period_bytes_min =     64,
	.period_bytes_max =     MAX_PERIOD_SIZE,
	.periods_min =          USE_PERIODS_MIN,
	.periods_max =          USE_PERIODS_MAX,
	.fifo_size =            0,
};

static struct snd_pcm_hardware msm_pcm_capture_hardware = {
	.info =                 SNDRV_PCM_INFO_INTERLEAVED,
	.formats =		USE_FORMATS,
	.rates =		USE_RATE,
	.rate_min =		USE_RATE_MIN,
	.rate_max =		USE_RATE_MAX,
	.channels_min =		USE_CHANNELS_MIN,
	.channels_max =		USE_CHANNELS_MAX,
	.buffer_bytes_max =	MAX_BUFFER_CAPTURE_SIZE,
	.period_bytes_min =	CAPTURE_SIZE,
	.period_bytes_max =	CAPTURE_SIZE,
	.periods_min =		USE_PERIODS_MIN,
	.periods_max =		USE_PERIODS_MAX,
	.fifo_size =		0,
};

/* Conventional and unconventional sample rate supported */
static unsigned int supported_sample_rates[] = {
	8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000
};

static struct snd_pcm_hw_constraint_list constraints_sample_rates = {
	.count = ARRAY_SIZE(supported_sample_rates),
	.list = supported_sample_rates,
	.mask = 0,
};

static void playback_event_handler(void *data)
{
	struct msm_audio *prtd = data;
	snd_pcm_period_elapsed(prtd->playback_substream);
}

static void capture_event_handler(void *data)
{
	struct msm_audio *prtd = data;
	snd_pcm_period_elapsed(prtd->capture_substream);
}

static int msm_pcm_playback_prepare(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct msm_audio *prtd = runtime->private_data;

	prtd->pcm_size = snd_pcm_lib_buffer_bytes(substream);
	prtd->pcm_count = snd_pcm_lib_period_bytes(substream);
	prtd->pcm_irq_pos = 0;
	prtd->pcm_buf_pos = 0;

	/* rate and channels are sent to audio driver */
	prtd->out_sample_rate = runtime->rate;
	prtd->out_channel_mode = runtime->channels;

	return 0;
}

static int msm_pcm_capture_prepare(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct msm_audio *prtd = runtime->private_data;
	struct audmgr_config cfg;
	int rc;

	prtd->pcm_size = snd_pcm_lib_buffer_bytes(substream);
	prtd->pcm_count = snd_pcm_lib_period_bytes(substream);
	prtd->pcm_irq_pos = 0;
	prtd->pcm_buf_pos = 0;

	/* rate and channels are sent to audio driver */
	prtd->samp_rate = convert_samp_rate(runtime->rate);
	prtd->samp_rate_index = convert_dsp_samp_index(runtime->rate);
	prtd->channel_mode = (runtime->channels - 1);
	prtd->buffer_size = prtd->channel_mode ? STEREO_DATA_SIZE : \
							MONO_DATA_SIZE;

	if (prtd->enabled == 1)
		return 0;

	prtd->type = AUDREC_CMD_TYPE_0_INDEX_WAV;

	cfg.tx_rate = convert_samp_rate(runtime->rate);
	cfg.rx_rate = RPC_AUD_DEF_SAMPLE_RATE_NONE;
	cfg.def_method = RPC_AUD_DEF_METHOD_RECORD;
	cfg.codec = RPC_AUD_DEF_CODEC_PCM;
	cfg.snd_method = RPC_SND_METHOD_MIDI;

	rc = audmgr_enable(&prtd->audmgr, &cfg);
	if (rc < 0)
		return rc;

	if (msm_adsp_enable(prtd->audpre)) {
		audmgr_disable(&prtd->audmgr);
		return -ENODEV;
	}
	if (msm_adsp_enable(prtd->audrec)) {
		msm_adsp_disable(prtd->audpre);
		audmgr_disable(&prtd->audmgr);
		return -ENODEV;
	}
	prtd->enabled = 1;
	audrec_dsp_enable(prtd, 1);

	return 0;
}

static int msm_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	int ret = 0;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

static snd_pcm_uframes_t
msm_pcm_playback_pointer(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct msm_audio *prtd = runtime->private_data;

	if (prtd->pcm_irq_pos == prtd->pcm_size)
		prtd->pcm_irq_pos = 0;
	return bytes_to_frames(runtime, (prtd->pcm_irq_pos));
}

static int msm_pcm_capture_copy(struct snd_pcm_substream *substream,
		 int channel, snd_pcm_uframes_t hwoff, void __user *buf,
						 snd_pcm_uframes_t frames)
{
	int rc = 0, rc1 = 0, rc2 = 0;
	int fbytes = 0;
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct msm_audio *prtd = substream->runtime->private_data;

	int monofbytes = 0;
	char *bufferp = NULL;

	fbytes = frames_to_bytes(runtime, frames);
	monofbytes = fbytes / 2;
	if (runtime->channels == 2) {
		rc = audio_buffer_read(prtd, buf, fbytes, NULL);
	} else {
		bufferp = buf;
		rc1 = audio_buffer_read(prtd, bufferp, monofbytes, NULL);
		bufferp = buf + monofbytes ;
		rc2 = audio_buffer_read(prtd, bufferp, monofbytes, NULL);
		rc = rc1 + rc2;
	}
	prtd->pcm_buf_pos += fbytes;
	return rc;
}

static snd_pcm_uframes_t
msm_pcm_capture_pointer(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct msm_audio *prtd = runtime->private_data;

	return bytes_to_frames(runtime, (prtd->pcm_irq_pos));
}

static int msm_pcm_capture_close(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct msm_audio *prtd = runtime->private_data;

	audrec_disable(prtd);
	audmgr_close(&prtd->audmgr);
	msm_adsp_put(prtd->audrec);
	msm_adsp_put(prtd->audpre);
	kfree(prtd);

	return 0;
}

struct  msm_audio_event_callbacks snd_msm_audio_ops = {
	.playback = playback_event_handler,
	.capture = capture_event_handler,
};

static int msm_pcm_open(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct msm_audio *prtd;
	int ret = 0;

	prtd = kzalloc(sizeof(struct msm_audio), GFP_KERNEL);
	if (prtd == NULL) {
		ret = -ENOMEM;
		return ret;
	}
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		msm_vol_ctl.update = 1; /* Update Volume, with Cached value */
		runtime->hw = msm_pcm_playback_hardware;
		prtd->dir = SNDRV_PCM_STREAM_PLAYBACK;
		prtd->playback_substream = substream;
	} else if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
		runtime->hw = msm_pcm_capture_hardware;
		prtd->dir = SNDRV_PCM_STREAM_CAPTURE;
		prtd->capture_substream = substream;
	}
	ret = snd_pcm_hw_constraint_list(runtime, 0,
						SNDRV_PCM_HW_PARAM_RATE,
						&constraints_sample_rates);
	if (ret < 0)
		goto out;
	prtd->ops = &snd_msm_audio_ops;
	runtime->private_data = prtd;

	ret = audio_adsp_configure(prtd);
	if (ret)
		goto out;
	intcnt = 0;
	copy_count = 0;
	return 0;

 out:
	kfree(prtd);
	return ret;
}

static int msm_pcm_playback_copy(struct snd_pcm_substream *substream, int a,
	snd_pcm_uframes_t hwoff, void __user *buf, snd_pcm_uframes_t frames)
{
	int rc = 1;
	int fbytes = 0;

	struct snd_pcm_runtime *runtime = substream->runtime;
	struct msm_audio *prtd = runtime->private_data;

	fbytes = frames_to_bytes(runtime, frames);
	rc = audio_send_buffer(prtd, buf, fbytes, NULL);
	++copy_count;
	prtd->pcm_buf_pos += fbytes;
	if (copy_count == 1) {
		mutex_lock(&the_locks.lock);
		audio_configure(prtd);
		mutex_unlock(&the_locks.lock);
	}
	if ((prtd->running) && (msm_vol_ctl.update)) {
		rc = msm_audio_volume_update(PCMPLAYBACK_DECODERID,
				msm_vol_ctl.volume, msm_vol_ctl.pan);
		msm_vol_ctl.update = 0;
	}

	return  rc;
}

static int msm_pcm_playback_close(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct msm_audio *prtd = runtime->private_data;

	audio_disable(prtd);
	audmgr_close(&prtd->audmgr);
	kfree(prtd);

	return 0;
}


static int msm_pcm_copy(struct snd_pcm_substream *substream, int a,
	 snd_pcm_uframes_t hwoff, void __user *buf, snd_pcm_uframes_t frames)
{
	int ret = 0;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		ret = msm_pcm_playback_copy(substream, a, hwoff, buf, frames);
	else if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		ret = msm_pcm_capture_copy(substream, a, hwoff, buf, frames);
	return ret;
}

static int msm_pcm_close(struct snd_pcm_substream *substream)
{
	int ret = 0;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		ret = msm_pcm_playback_close(substream);
	else if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		ret = msm_pcm_capture_close(substream);
	return ret;
}
static int msm_pcm_prepare(struct snd_pcm_substream *substream)
{
	int ret = 0;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		ret = msm_pcm_playback_prepare(substream);
	else if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		ret = msm_pcm_capture_prepare(substream);
	return ret;
}

static snd_pcm_uframes_t msm_pcm_pointer(struct snd_pcm_substream *substream)
{
	snd_pcm_uframes_t ret = 0;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		ret = msm_pcm_playback_pointer(substream);
	else if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		ret = msm_pcm_capture_pointer(substream);
	return ret;
}

int msm_pcm_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params)
{
	struct snd_pcm_runtime *runtime = substream->runtime;

	if (substream->pcm->device & 1) {
		runtime->hw.info &= ~SNDRV_PCM_INFO_INTERLEAVED;
		runtime->hw.info |= SNDRV_PCM_INFO_NONINTERLEAVED;
	}
	return 0;

}

struct snd_pcm_ops msm_pcm_ops = {
	.open           = msm_pcm_open,
	.copy		= msm_pcm_copy,
	.hw_params	= msm_pcm_hw_params,
	.close          = msm_pcm_close,
	.ioctl          = snd_pcm_lib_ioctl,
	.prepare        = msm_pcm_prepare,
	.trigger        = msm_pcm_trigger,
	.pointer        = msm_pcm_pointer,
};
EXPORT_SYMBOL_GPL(msm_pcm_ops);

struct snd_soc_dai msm7201_dai = {
	.name = "ASOC",
	.playback = {
		.stream_name = "Playback",
		.channels_min = USE_CHANNELS_MIN,
		.channels_max = USE_CHANNELS_MAX,
		.rates = USE_RATE,
		.rate_min = USE_RATE_MIN,
		.rate_max = USE_RATE_MAX,
		.formats = USE_FORMATS,
	},
	.capture = {
		.stream_name = "Capture",
		.channels_min = USE_CHANNELS_MIN,
		.channels_max = USE_CHANNELS_MAX,
		.rate_min = USE_RATE_MIN,
		.rates = USE_RATE,
		.formats = USE_FORMATS,
	},
};
EXPORT_SYMBOL_GPL(msm7201_dai);

struct snd_soc_dai msm_cpudai = {
	.name = "MSM_7200A",
	.id = 0,
	.playback = {
		.channels_min = USE_CHANNELS_MIN,
		.channels_max = USE_CHANNELS_MAX,
		.rates = USE_RATE,
		.rate_min = USE_RATE_MIN,
		.rate_max = USE_RATE_MAX,
		.formats = USE_FORMATS,
	},
	.capture = {
		.channels_min = USE_CHANNELS_MIN,
		.channels_max = USE_CHANNELS_MAX,
		.rate_min = USE_RATE_MIN,
		.rates = USE_RATE,
		.formats = USE_FORMATS,
	},
};
EXPORT_SYMBOL_GPL(msm_cpudai);

static int __init msm_dai_init(void)
{
	int ret;

	ret = snd_soc_register_dai(&msm7201_dai);
	ret = snd_soc_register_dai(&msm_cpudai);

	return ret;
}
module_init(msm_dai_init);

static void __exit msm_dai_exit(void)
{
	snd_soc_unregister_dai(&msm7201_dai);
	snd_soc_unregister_dai(&msm_cpudai);
}
module_exit(msm_dai_exit);


MODULE_DESCRIPTION("PCM module platform driver");
MODULE_LICENSE("GPL v2");
