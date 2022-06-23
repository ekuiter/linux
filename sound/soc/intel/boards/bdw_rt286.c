// SPDX-License-Identifier: GPL-2.0-only
/*
 * Intel Broadwell Wildcatpoint SST Audio
 *
 * Copyright (C) 2013, Intel Corporation. All rights reserved.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/soc.h>
#include <sound/jack.h>
#include <sound/pcm_params.h>
#include <sound/soc-acpi.h>

#include "../../codecs/rt286.h"

static struct snd_soc_jack card_headset;
/* Headset jack detection DAPM pins */
static struct snd_soc_jack_pin card_headset_pins[] = {
	{
		.pin = "Mic Jack",
		.mask = SND_JACK_MICROPHONE,
	},
	{
		.pin = "Headphone Jack",
		.mask = SND_JACK_HEADPHONE,
	},
};

static const struct snd_kcontrol_new card_controls[] = {
	SOC_DAPM_PIN_SWITCH("Speaker"),
	SOC_DAPM_PIN_SWITCH("Headphone Jack"),
};

static const struct snd_soc_dapm_widget card_widgets[] = {
	SND_SOC_DAPM_HP("Headphone Jack", NULL),
	SND_SOC_DAPM_SPK("Speaker", NULL),
	SND_SOC_DAPM_MIC("Mic Jack", NULL),
	SND_SOC_DAPM_MIC("DMIC1", NULL),
	SND_SOC_DAPM_MIC("DMIC2", NULL),
	SND_SOC_DAPM_LINE("Line Jack", NULL),
};

static const struct snd_soc_dapm_route card_routes[] = {

	/* speaker */
	{"Speaker", NULL, "SPOR"},
	{"Speaker", NULL, "SPOL"},

	/* HP jack connectors - unknown if we have jack deteck */
	{"Headphone Jack", NULL, "HPO Pin"},

	/* other jacks */
	{"MIC1", NULL, "Mic Jack"},
	{"LINE1", NULL, "Line Jack"},

	/* digital mics */
	{"DMIC1 Pin", NULL, "DMIC1"},
	{"DMIC2 Pin", NULL, "DMIC2"},

	/* CODEC BE connections */
	{"SSP0 CODEC IN", NULL, "AIF1 Capture"},
	{"AIF1 Playback", NULL, "SSP0 CODEC OUT"},
};

static int codec_link_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_component *component = asoc_rtd_to_codec(rtd, 0)->component;
	int ret = 0;
	ret = snd_soc_card_jack_new_pins(rtd->card, "Headset",
		SND_JACK_HEADSET | SND_JACK_BTN_0, &card_headset,
		card_headset_pins, ARRAY_SIZE(card_headset_pins));
	if (ret)
		return ret;

	snd_soc_component_set_jack(component, &card_headset, NULL);
	return 0;
}


static int codec_link_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
			struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
						      SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *chan = hw_param_interval(params,
						      SNDRV_PCM_HW_PARAM_CHANNELS);

	/* The ADSP will covert the FE rate to 48k, stereo */
	rate->min = rate->max = 48000;
	chan->min = chan->max = 2;

	/* set SSP0 to 16 bit */
	params_set_format(params, SNDRV_PCM_FORMAT_S16_LE);
	return 0;
}

static int codec_link_hw_params(struct snd_pcm_substream *substream,
	struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = asoc_substream_to_rtd(substream);
	struct snd_soc_dai *codec_dai = asoc_rtd_to_codec(rtd, 0);
	int ret;

	ret = snd_soc_dai_set_sysclk(codec_dai, RT286_SCLK_S_PLL, 24000000,
		SND_SOC_CLOCK_IN);

	if (ret < 0) {
		dev_err(rtd->dev, "can't set codec sysclk configuration\n");
		return ret;
	}

	return ret;
}

static const struct snd_soc_ops codec_link_ops = {
	.hw_params = codec_link_hw_params,
};

static const unsigned int channels[] = {
	2,
};

static const struct snd_pcm_hw_constraint_list constraints_channels = {
	.count = ARRAY_SIZE(channels),
	.list = channels,
	.mask = 0,
};

static int bdw_rt286_fe_startup(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;

	/* Board supports stereo configuration only */
	runtime->hw.channels_max = 2;
	return snd_pcm_hw_constraint_list(runtime, 0,
					  SNDRV_PCM_HW_PARAM_CHANNELS,
					  &constraints_channels);
}

static const struct snd_soc_ops bdw_rt286_fe_ops = {
	.startup = bdw_rt286_fe_startup,
};

SND_SOC_DAILINK_DEF(system,
	DAILINK_COMP_ARRAY(COMP_CPU("System Pin")));

SND_SOC_DAILINK_DEF(offload0,
	DAILINK_COMP_ARRAY(COMP_CPU("Offload0 Pin")));

SND_SOC_DAILINK_DEF(offload1,
	DAILINK_COMP_ARRAY(COMP_CPU("Offload1 Pin")));

SND_SOC_DAILINK_DEF(loopback,
	DAILINK_COMP_ARRAY(COMP_CPU("Loopback Pin")));

SND_SOC_DAILINK_DEF(dummy,
	DAILINK_COMP_ARRAY(COMP_DUMMY()));

SND_SOC_DAILINK_DEF(platform,
	DAILINK_COMP_ARRAY(COMP_PLATFORM("haswell-pcm-audio")));

SND_SOC_DAILINK_DEF(codec,
	DAILINK_COMP_ARRAY(COMP_CODEC("i2c-INT343A:00", "rt286-aif1")));

SND_SOC_DAILINK_DEF(ssp0_port,
	    DAILINK_COMP_ARRAY(COMP_CPU("ssp0-port")));

/* broadwell digital audio interface glue - connects codec <--> CPU */
static struct snd_soc_dai_link card_dai_links[] = {
	/* Front End DAI links */
	{
		.name = "System PCM",
		.stream_name = "System Playback/Capture",
		.nonatomic = 1,
		.dynamic = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST, SND_SOC_DPCM_TRIGGER_POST},
		.ops = &bdw_rt286_fe_ops,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		SND_SOC_DAILINK_REG(system, dummy, platform),
	},
	{
		.name = "Offload0",
		.stream_name = "Offload0 Playback",
		.nonatomic = 1,
		.dynamic = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST, SND_SOC_DPCM_TRIGGER_POST},
		.dpcm_playback = 1,
		SND_SOC_DAILINK_REG(offload0, dummy, platform),
	},
	{
		.name = "Offload1",
		.stream_name = "Offload1 Playback",
		.nonatomic = 1,
		.dynamic = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST, SND_SOC_DPCM_TRIGGER_POST},
		.dpcm_playback = 1,
		SND_SOC_DAILINK_REG(offload1, dummy, platform),
	},
	{
		.name = "Loopback PCM",
		.stream_name = "Loopback",
		.nonatomic = 1,
		.dynamic = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST, SND_SOC_DPCM_TRIGGER_POST},
		.dpcm_capture = 1,
		SND_SOC_DAILINK_REG(loopback, dummy, platform),
	},
	/* Back End DAI links */
	{
		/* SSP0 - Codec */
		.name = "Codec",
		.id = 0,
		.no_pcm = 1,
		.init = codec_link_init,
		.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
			SND_SOC_DAIFMT_CBC_CFC,
		.ignore_pmdown_time = 1,
		.be_hw_params_fixup = codec_link_hw_params_fixup,
		.ops = &codec_link_ops,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		SND_SOC_DAILINK_REG(ssp0_port, codec, platform),
	},
};

static void bdw_rt286_disable_jack(struct snd_soc_card *card)
{
	struct snd_soc_component *component;

	for_each_card_components(card, component) {
		if (!strcmp(component->name, "i2c-INT343A:00")) {

			dev_dbg(component->dev, "disabling jack detect before going to suspend.\n");
			snd_soc_component_set_jack(component, NULL, NULL);
			break;
		}
	}
}

static int bdw_rt286_suspend(struct snd_soc_card *card)
{
	bdw_rt286_disable_jack(card);

	return 0;
}

static int bdw_rt286_resume(struct snd_soc_card *card){
	struct snd_soc_component *component;

	for_each_card_components(card, component) {
		if (!strcmp(component->name, "i2c-INT343A:00")) {

			dev_dbg(component->dev, "enabling jack detect for resume.\n");
			snd_soc_component_set_jack(component, &card_headset, NULL);
			break;
		}
	}
	return 0;
}

/* use space before codec name to simplify card ID, and simplify driver name */
#define SOF_CARD_NAME "bdw rt286" /* card name will be 'sof-bdw rt286' */
#define SOF_DRIVER_NAME "SOF"

#define CARD_NAME "broadwell-rt286"
#define DRIVER_NAME NULL /* card name will be used for driver name */

/* broadwell audio machine driver for WPT + RT286S */
static struct snd_soc_card bdw_rt286_card = {
	.owner = THIS_MODULE,
	.dai_link = card_dai_links,
	.num_links = ARRAY_SIZE(card_dai_links),
	.controls = card_controls,
	.num_controls = ARRAY_SIZE(card_controls),
	.dapm_widgets = card_widgets,
	.num_dapm_widgets = ARRAY_SIZE(card_widgets),
	.dapm_routes = card_routes,
	.num_dapm_routes = ARRAY_SIZE(card_routes),
	.fully_routed = true,
	.suspend_pre = bdw_rt286_suspend,
	.resume_post = bdw_rt286_resume,
};

static int bdw_rt286_probe(struct platform_device *pdev)
{
	struct snd_soc_acpi_mach *mach;
	int ret;

	bdw_rt286_card.dev = &pdev->dev;

	/* override platform name, if required */
	mach = pdev->dev.platform_data;
	ret = snd_soc_fixup_dai_links_platform_name(&bdw_rt286_card,
						    mach->mach_params.platform);
	if (ret)
		return ret;

	/* set card and driver name */
	if (snd_soc_acpi_sof_parent(&pdev->dev)) {
		bdw_rt286_card.name = SOF_CARD_NAME;
		bdw_rt286_card.driver_name = SOF_DRIVER_NAME;
	} else {
		bdw_rt286_card.name = CARD_NAME;
		bdw_rt286_card.driver_name = DRIVER_NAME;
	}

	return devm_snd_soc_register_card(&pdev->dev, &bdw_rt286_card);
}

static int bdw_rt286_remove(struct platform_device *pdev)
{
	struct snd_soc_card *card = platform_get_drvdata(pdev);

	bdw_rt286_disable_jack(card);

	return 0;
}

static struct platform_driver bdw_rt286_driver = {
	.probe = bdw_rt286_probe,
	.remove = bdw_rt286_remove,
	.driver = {
		.name = "broadwell-audio",
		.pm = &snd_soc_pm_ops
	},
};

module_platform_driver(bdw_rt286_driver)

/* Module information */
MODULE_AUTHOR("Liam Girdwood, Xingchao Wang");
MODULE_DESCRIPTION("Intel SST Audio for WPT/Broadwell");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:broadwell-audio");
