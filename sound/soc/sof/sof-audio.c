// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
//
// This file is provided under a dual BSD/GPLv2 license.  When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2019 Intel Corporation. All rights reserved.
//
// Author: Ranjani Sridharan <ranjani.sridharan@linux.intel.com>
//

#include "sof-audio.h"
#include "ops.h"

static int sof_kcontrol_setup(struct snd_sof_dev *sdev, struct snd_sof_control *scontrol)
{
	int ipc_cmd, ctrl_type;
	int ret;

	/* reset readback offset for scontrol */
	scontrol->readback_offset = 0;

	/* notify DSP of kcontrol values */
	switch (scontrol->cmd) {
	case SOF_CTRL_CMD_VOLUME:
	case SOF_CTRL_CMD_ENUM:
	case SOF_CTRL_CMD_SWITCH:
		ipc_cmd = SOF_IPC_COMP_SET_VALUE;
		ctrl_type = SOF_CTRL_TYPE_VALUE_CHAN_SET;
		break;
	case SOF_CTRL_CMD_BINARY:
		ipc_cmd = SOF_IPC_COMP_SET_DATA;
		ctrl_type = SOF_CTRL_TYPE_DATA_SET;
		break;
	default:
		return 0;
	}

	ret = snd_sof_ipc_set_get_comp_data(scontrol, ipc_cmd, ctrl_type, scontrol->cmd, true);
	if (ret < 0)
		dev_err(sdev->dev, "error: failed kcontrol value set for widget: %d\n",
			scontrol->comp_id);

	return ret;
}

static int sof_dai_config_setup(struct snd_sof_dev *sdev, struct snd_sof_dai *dai)
{
	struct sof_ipc_dai_config *config;
	struct sof_ipc_reply reply;
	int ret;

	config = &dai->dai_config[dai->current_config];
	if (!config) {
		dev_err(sdev->dev, "error: no config for DAI %s\n", dai->name);
		return -EINVAL;
	}

	ret = sof_ipc_tx_message(sdev->ipc, config->hdr.cmd, config, config->hdr.size,
				 &reply, sizeof(reply));

	if (ret < 0)
		dev_err(sdev->dev, "error: failed to set dai config for %s\n", dai->name);

	return ret;
}

static int sof_widget_kcontrol_setup(struct snd_sof_dev *sdev, struct snd_sof_widget *swidget)
{
	struct snd_sof_control *scontrol;
	int ret;

	/* set up all controls for the widget */
	list_for_each_entry(scontrol, &sdev->kcontrol_list, list)
		if (scontrol->comp_id == swidget->comp_id) {
			ret = sof_kcontrol_setup(sdev, scontrol);
			if (ret < 0) {
				dev_err(sdev->dev, "error: fail to set up kcontrols for widget %s\n",
					swidget->widget->name);
				return ret;
			}
		}

	return 0;
}

int sof_widget_free(struct snd_sof_dev *sdev, struct snd_sof_widget *swidget)
{
	struct sof_ipc_free ipc_free = {
		.hdr = {
			.size = sizeof(ipc_free),
			.cmd = SOF_IPC_GLB_TPLG_MSG,
		},
		.id = swidget->comp_id,
	};
	struct sof_ipc_reply reply;
	int ret;

	if (!swidget->private)
		return 0;

	/* only free when use_count is 0 */
	if (--swidget->use_count)
		return 0;

	switch (swidget->id) {
	case snd_soc_dapm_scheduler:
		ipc_free.hdr.cmd |= SOF_IPC_TPLG_PIPE_FREE;
		break;
	case snd_soc_dapm_buffer:
		ipc_free.hdr.cmd |= SOF_IPC_TPLG_BUFFER_FREE;
		break;
	default:
		ipc_free.hdr.cmd |= SOF_IPC_TPLG_COMP_FREE;
		break;
	}

	ret = sof_ipc_tx_message(sdev->ipc, ipc_free.hdr.cmd, &ipc_free, sizeof(ipc_free),
				 &reply, sizeof(reply));
	if (ret < 0) {
		dev_err(sdev->dev, "error: failed to free widget %s\n", swidget->widget->name);
		swidget->use_count++;
		return ret;
	}

	swidget->complete = 0;
	dev_dbg(sdev->dev, "widget %s freed\n", swidget->widget->name);

	return 0;
}
EXPORT_SYMBOL(sof_widget_free);

int sof_widget_setup(struct snd_sof_dev *sdev, struct snd_sof_widget *swidget)
{
	struct sof_ipc_pipe_new *pipeline;
	struct sof_ipc_comp_reply r;
	struct sof_ipc_cmd_hdr *hdr;
	struct sof_ipc_comp *comp;
	struct snd_sof_dai *dai;
	size_t ipc_size;
	int ret;

	/* skip if there is no private data */
	if (!swidget->private)
		return 0;

	/* widget already set up */
	if (++swidget->use_count > 1)
		return 0;

	ret = sof_pipeline_core_enable(sdev, swidget);
	if (ret < 0) {
		dev_err(sdev->dev, "error: failed to enable target core: %d for widget %s\n",
			ret, swidget->widget->name);
		goto use_count_dec;
	}

	switch (swidget->id) {
	case snd_soc_dapm_dai_in:
	case snd_soc_dapm_dai_out:
		ipc_size = sizeof(struct sof_ipc_comp_dai) + sizeof(struct sof_ipc_comp_ext);
		comp = kzalloc(ipc_size, GFP_KERNEL);
		if (!comp)
			return -ENOMEM;

		dai = swidget->private;
		memcpy(comp, &dai->comp_dai, sizeof(struct sof_ipc_comp_dai));

		/* append extended data to the end of the component */
		memcpy((u8 *)comp + sizeof(struct sof_ipc_comp_dai), &swidget->comp_ext,
		       sizeof(swidget->comp_ext));

		ret = sof_ipc_tx_message(sdev->ipc, comp->hdr.cmd, comp, ipc_size, &r, sizeof(r));
		kfree(comp);
		break;
	case snd_soc_dapm_scheduler:
		pipeline = swidget->private;
		ret = sof_load_pipeline_ipc(sdev->dev, pipeline, &r);
		break;
	default:
		hdr = swidget->private;
		ret = sof_ipc_tx_message(sdev->ipc, hdr->cmd, swidget->private, hdr->size,
					 &r, sizeof(r));
		break;
	}
	if (ret < 0) {
		dev_err(sdev->dev, "error: failed to load widget %s\n", swidget->widget->name);
		goto use_count_dec;
	}

	/* restore kcontrols for widget */
	ret = sof_widget_kcontrol_setup(sdev, swidget);
	if (ret < 0) {
		dev_err(sdev->dev, "error: failed to restore kcontrols for widget %s\n",
			swidget->widget->name);
		return ret;
	}

	dev_dbg(sdev->dev, "widget %s setup complete\n", swidget->widget->name);

	return 0;

use_count_dec:
	swidget->use_count--;
	return ret;
}
EXPORT_SYMBOL(sof_widget_setup);

/*
 * helper to determine if there are only D0i3 compatible
 * streams active
 */
bool snd_sof_dsp_only_d0i3_compatible_stream_active(struct snd_sof_dev *sdev)
{
	struct snd_pcm_substream *substream;
	struct snd_sof_pcm *spcm;
	bool d0i3_compatible_active = false;
	int dir;

	list_for_each_entry(spcm, &sdev->pcm_list, list) {
		for_each_pcm_streams(dir) {
			substream = spcm->stream[dir].substream;
			if (!substream || !substream->runtime)
				continue;

			/*
			 * substream->runtime being not NULL indicates
			 * that the stream is open. No need to check the
			 * stream state.
			 */
			if (!spcm->stream[dir].d0i3_compatible)
				return false;

			d0i3_compatible_active = true;
		}
	}

	return d0i3_compatible_active;
}
EXPORT_SYMBOL(snd_sof_dsp_only_d0i3_compatible_stream_active);

bool snd_sof_stream_suspend_ignored(struct snd_sof_dev *sdev)
{
	struct snd_sof_pcm *spcm;

	list_for_each_entry(spcm, &sdev->pcm_list, list) {
		if (spcm->stream[SNDRV_PCM_STREAM_PLAYBACK].suspend_ignored ||
		    spcm->stream[SNDRV_PCM_STREAM_CAPTURE].suspend_ignored)
			return true;
	}

	return false;
}

int sof_set_hw_params_upon_resume(struct device *dev)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(dev);
	struct snd_pcm_substream *substream;
	struct snd_sof_pcm *spcm;
	snd_pcm_state_t state;
	int dir;

	/*
	 * SOF requires hw_params to be set-up internally upon resume.
	 * So, set the flag to indicate this for those streams that
	 * have been suspended.
	 */
	list_for_each_entry(spcm, &sdev->pcm_list, list) {
		for_each_pcm_streams(dir) {
			/*
			 * do not reset hw_params upon resume for streams that
			 * were kept running during suspend
			 */
			if (spcm->stream[dir].suspend_ignored)
				continue;

			substream = spcm->stream[dir].substream;
			if (!substream || !substream->runtime)
				continue;

			state = substream->runtime->status->state;
			if (state == SNDRV_PCM_STATE_SUSPENDED)
				spcm->prepared[dir] = false;
		}
	}

	/* set internal flag for BE */
	return snd_sof_dsp_hw_params_upon_resume(sdev);
}

const struct sof_ipc_pipe_new *snd_sof_pipeline_find(struct snd_sof_dev *sdev,
						     int pipeline_id)
{
	const struct snd_sof_widget *swidget;

	list_for_each_entry(swidget, &sdev->widget_list, list)
		if (swidget->id == snd_soc_dapm_scheduler) {
			const struct sof_ipc_pipe_new *pipeline =
				swidget->private;
			if (pipeline->pipeline_id == pipeline_id)
				return pipeline;
		}

	return NULL;
}

int sof_set_up_pipelines(struct device *dev)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(dev);
	struct snd_sof_widget *swidget;
	struct snd_sof_route *sroute;
	struct snd_sof_dai *dai;
	int ret;

	/* restore pipeline components */
	list_for_each_entry_reverse(swidget, &sdev->widget_list, list) {
		/* reset widget use_count after resuming */
		swidget->use_count = 0;

		ret = sof_widget_setup(sdev, swidget);
		if (ret < 0)
			return ret;
	}

	/* restore pipeline connections */
	list_for_each_entry_reverse(sroute, &sdev->route_list, list) {
		struct sof_ipc_pipe_comp_connect *connect;
		struct sof_ipc_reply reply;

		/* skip if there's no private data */
		if (!sroute->private)
			continue;

		connect = sroute->private;

		/* send ipc */
		ret = sof_ipc_tx_message(sdev->ipc,
					 connect->hdr.cmd,
					 connect, sizeof(*connect),
					 &reply, sizeof(reply));
		if (ret < 0) {
			dev_err(dev,
				"error: failed to load route sink %s control %s source %s\n",
				sroute->route->sink,
				sroute->route->control ? sroute->route->control
					: "none",
				sroute->route->source);

			return ret;
		}
		sroute->setup = true;
	}

	/* restore dai links */
	list_for_each_entry_reverse(dai, &sdev->dai_list, list) {
		struct sof_ipc_dai_config *config = &dai->dai_config[dai->current_config];

		/*
		 * The link DMA channel would be invalidated for running
		 * streams but not for streams that were in the PAUSED
		 * state during suspend. So invalidate it here before setting
		 * the dai config in the DSP.
		 */
		if (config->type == SOF_DAI_INTEL_HDA)
			config->hda.link_dma_ch = DMA_CHAN_INVALID;

		ret = sof_dai_config_setup(sdev, dai);
		if (ret < 0)
			return ret;
	}

	/* complete pipeline */
	list_for_each_entry(swidget, &sdev->widget_list, list) {
		switch (swidget->id) {
		case snd_soc_dapm_scheduler:
			swidget->complete =
				snd_sof_complete_pipeline(dev, swidget);
			break;
		default:
			break;
		}
	}

	return 0;
}

/*
 * This function doesn't free widgets. It only resets the set up status for all routes and
 * use_count for all widgets.
 */
void sof_tear_down_pipelines(struct device *dev)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(dev);
	struct snd_sof_widget *swidget;
	struct snd_sof_route *sroute;

	/*
	 * No need to protect swidget->use_count and sroute->setup as this function is called only
	 * during the suspend callback and all streams should be suspended by then
	 */
	list_for_each_entry(swidget, &sdev->widget_list, list)
		swidget->use_count = 0;

	list_for_each_entry(sroute, &sdev->route_list, list)
		sroute->setup = false;
}

/*
 * Generic object lookup APIs.
 */

struct snd_sof_pcm *snd_sof_find_spcm_name(struct snd_soc_component *scomp,
					   const char *name)
{
	struct snd_sof_dev *sdev = snd_soc_component_get_drvdata(scomp);
	struct snd_sof_pcm *spcm;

	list_for_each_entry(spcm, &sdev->pcm_list, list) {
		/* match with PCM dai name */
		if (strcmp(spcm->pcm.dai_name, name) == 0)
			return spcm;

		/* match with playback caps name if set */
		if (*spcm->pcm.caps[0].name &&
		    !strcmp(spcm->pcm.caps[0].name, name))
			return spcm;

		/* match with capture caps name if set */
		if (*spcm->pcm.caps[1].name &&
		    !strcmp(spcm->pcm.caps[1].name, name))
			return spcm;
	}

	return NULL;
}

struct snd_sof_pcm *snd_sof_find_spcm_comp(struct snd_soc_component *scomp,
					   unsigned int comp_id,
					   int *direction)
{
	struct snd_sof_dev *sdev = snd_soc_component_get_drvdata(scomp);
	struct snd_sof_pcm *spcm;
	int dir;

	list_for_each_entry(spcm, &sdev->pcm_list, list) {
		for_each_pcm_streams(dir) {
			if (spcm->stream[dir].comp_id == comp_id) {
				*direction = dir;
				return spcm;
			}
		}
	}

	return NULL;
}

struct snd_sof_pcm *snd_sof_find_spcm_pcm_id(struct snd_soc_component *scomp,
					     unsigned int pcm_id)
{
	struct snd_sof_dev *sdev = snd_soc_component_get_drvdata(scomp);
	struct snd_sof_pcm *spcm;

	list_for_each_entry(spcm, &sdev->pcm_list, list) {
		if (le32_to_cpu(spcm->pcm.pcm_id) == pcm_id)
			return spcm;
	}

	return NULL;
}

struct snd_sof_widget *snd_sof_find_swidget(struct snd_soc_component *scomp,
					    const char *name)
{
	struct snd_sof_dev *sdev = snd_soc_component_get_drvdata(scomp);
	struct snd_sof_widget *swidget;

	list_for_each_entry(swidget, &sdev->widget_list, list) {
		if (strcmp(name, swidget->widget->name) == 0)
			return swidget;
	}

	return NULL;
}

/* find widget by stream name and direction */
struct snd_sof_widget *
snd_sof_find_swidget_sname(struct snd_soc_component *scomp,
			   const char *pcm_name, int dir)
{
	struct snd_sof_dev *sdev = snd_soc_component_get_drvdata(scomp);
	struct snd_sof_widget *swidget;
	enum snd_soc_dapm_type type;

	if (dir == SNDRV_PCM_STREAM_PLAYBACK)
		type = snd_soc_dapm_aif_in;
	else
		type = snd_soc_dapm_aif_out;

	list_for_each_entry(swidget, &sdev->widget_list, list) {
		if (!strcmp(pcm_name, swidget->widget->sname) &&
		    swidget->id == type)
			return swidget;
	}

	return NULL;
}

struct snd_sof_dai *snd_sof_find_dai(struct snd_soc_component *scomp,
				     const char *name)
{
	struct snd_sof_dev *sdev = snd_soc_component_get_drvdata(scomp);
	struct snd_sof_dai *dai;

	list_for_each_entry(dai, &sdev->dai_list, list) {
		if (dai->name && (strcmp(name, dai->name) == 0))
			return dai;
	}

	return NULL;
}

#define SOF_DAI_CLK_INTEL_SSP_MCLK	0
#define SOF_DAI_CLK_INTEL_SSP_BCLK	1

static int sof_dai_get_clk(struct snd_soc_pcm_runtime *rtd, int clk_type)
{
	struct snd_soc_component *component =
		snd_soc_rtdcom_lookup(rtd, SOF_AUDIO_PCM_DRV_NAME);
	struct snd_sof_dai *dai =
		snd_sof_find_dai(component, (char *)rtd->dai_link->name);

	/* use the tplg configured mclk if existed */
	if (!dai || !dai->dai_config)
		return 0;

	switch (dai->dai_config->type) {
	case SOF_DAI_INTEL_SSP:
		switch (clk_type) {
		case SOF_DAI_CLK_INTEL_SSP_MCLK:
			return dai->dai_config->ssp.mclk_rate;
		case SOF_DAI_CLK_INTEL_SSP_BCLK:
			return dai->dai_config->ssp.bclk_rate;
		default:
			dev_err(rtd->dev, "fail to get SSP clk %d rate\n",
				clk_type);
			return -EINVAL;
		}
		break;
	default:
		/* not yet implemented for platforms other than the above */
		dev_err(rtd->dev, "DAI type %d not supported yet!\n",
			dai->dai_config->type);
		return -EINVAL;
	}
}

/*
 * Helper to get SSP MCLK from a pcm_runtime.
 * Return 0 if not exist.
 */
int sof_dai_get_mclk(struct snd_soc_pcm_runtime *rtd)
{
	return sof_dai_get_clk(rtd, SOF_DAI_CLK_INTEL_SSP_MCLK);
}
EXPORT_SYMBOL(sof_dai_get_mclk);

/*
 * Helper to get SSP BCLK from a pcm_runtime.
 * Return 0 if not exist.
 */
int sof_dai_get_bclk(struct snd_soc_pcm_runtime *rtd)
{
	return sof_dai_get_clk(rtd, SOF_DAI_CLK_INTEL_SSP_BCLK);
}
EXPORT_SYMBOL(sof_dai_get_bclk);

/*
 * SOF Driver enumeration.
 */
int sof_machine_check(struct snd_sof_dev *sdev)
{
	struct snd_sof_pdata *sof_pdata = sdev->pdata;
	const struct sof_dev_desc *desc = sof_pdata->desc;
	struct snd_soc_acpi_mach *mach;

	if (!IS_ENABLED(CONFIG_SND_SOC_SOF_FORCE_NOCODEC_MODE)) {

		/* find machine */
		snd_sof_machine_select(sdev);
		if (sof_pdata->machine) {
			snd_sof_set_mach_params(sof_pdata->machine, sdev);
			return 0;
		}

		if (!IS_ENABLED(CONFIG_SND_SOC_SOF_NOCODEC)) {
			dev_err(sdev->dev, "error: no matching ASoC machine driver found - aborting probe\n");
			return -ENODEV;
		}
	} else {
		dev_warn(sdev->dev, "Force to use nocodec mode\n");
	}

	/* select nocodec mode */
	dev_warn(sdev->dev, "Using nocodec machine driver\n");
	mach = devm_kzalloc(sdev->dev, sizeof(*mach), GFP_KERNEL);
	if (!mach)
		return -ENOMEM;

	mach->drv_name = "sof-nocodec";
	sof_pdata->tplg_filename = desc->nocodec_tplg_filename;

	sof_pdata->machine = mach;
	snd_sof_set_mach_params(sof_pdata->machine, sdev);

	return 0;
}
EXPORT_SYMBOL(sof_machine_check);

int sof_machine_register(struct snd_sof_dev *sdev, void *pdata)
{
	struct snd_sof_pdata *plat_data = pdata;
	const char *drv_name;
	const void *mach;
	int size;

	drv_name = plat_data->machine->drv_name;
	mach = plat_data->machine;
	size = sizeof(*plat_data->machine);

	/* register machine driver, pass machine info as pdata */
	plat_data->pdev_mach =
		platform_device_register_data(sdev->dev, drv_name,
					      PLATFORM_DEVID_NONE, mach, size);
	if (IS_ERR(plat_data->pdev_mach))
		return PTR_ERR(plat_data->pdev_mach);

	dev_dbg(sdev->dev, "created machine %s\n",
		dev_name(&plat_data->pdev_mach->dev));

	return 0;
}
EXPORT_SYMBOL(sof_machine_register);

void sof_machine_unregister(struct snd_sof_dev *sdev, void *pdata)
{
	struct snd_sof_pdata *plat_data = pdata;

	if (!IS_ERR_OR_NULL(plat_data->pdev_mach))
		platform_device_unregister(plat_data->pdev_mach);
}
EXPORT_SYMBOL(sof_machine_unregister);
