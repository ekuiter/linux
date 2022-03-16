// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
//
// This file is provided under a dual BSD/GPLv2 license.  When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2021 Intel Corporation. All rights reserved.
//
//

#include <uapi/sound/sof/tokens.h>
#include <sound/pcm_params.h>
#include "sof-priv.h"
#include "sof-audio.h"
#include "ops.h"

/* PCM */
static const struct sof_topology_token pcm_tokens[] = {
	{SOF_TKN_PCM_DMAC_CONFIG, SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc_comp_host, dmac_config)},
};

/* Generic components */
static const struct sof_topology_token comp_tokens[] = {
	{SOF_TKN_COMP_PERIOD_SINK_COUNT, SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc_comp_config, periods_sink)},
	{SOF_TKN_COMP_PERIOD_SOURCE_COUNT, SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc_comp_config, periods_source)},
	{SOF_TKN_COMP_FORMAT,
		SND_SOC_TPLG_TUPLE_TYPE_STRING, get_token_comp_format,
		offsetof(struct sof_ipc_comp_config, frame_fmt)},
};

/* Core tokens */
static const struct sof_topology_token core_tokens[] = {
	{SOF_TKN_COMP_CORE_ID, SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc_comp, core)},
};

/* Component extended tokens */
static const struct sof_topology_token comp_ext_tokens[] = {
	{SOF_TKN_COMP_UUID, SND_SOC_TPLG_TUPLE_TYPE_UUID, get_token_uuid,
		offsetof(struct snd_sof_widget, uuid)},
};

static const struct sof_token_info ipc3_token_list[SOF_TOKEN_COUNT] = {
	[SOF_PCM_TOKENS] = {"PCM tokens", pcm_tokens, ARRAY_SIZE(pcm_tokens)},
	[SOF_COMP_TOKENS] = {"Comp tokens", comp_tokens, ARRAY_SIZE(comp_tokens)},
	[SOF_CORE_TOKENS] = {"Core tokens", core_tokens, ARRAY_SIZE(core_tokens)},
	[SOF_COMP_EXT_TOKENS] = {"AFE tokens", comp_ext_tokens, ARRAY_SIZE(comp_ext_tokens)},
};

/**
 * sof_comp_alloc - allocate and initialize buffer for a new component
 * @swidget: pointer to struct snd_sof_widget containing extended data
 * @ipc_size: IPC payload size that will be updated depending on valid
 *  extended data.
 * @index: ID of the pipeline the component belongs to
 *
 * Return: The pointer to the new allocated component, NULL if failed.
 */
static void *sof_comp_alloc(struct snd_sof_widget *swidget, size_t *ipc_size,
			    int index)
{
	struct sof_ipc_comp *comp;
	size_t total_size = *ipc_size;
	size_t ext_size = sizeof(swidget->uuid);

	/* only non-zero UUID is valid */
	if (!guid_is_null(&swidget->uuid))
		total_size += ext_size;

	comp = kzalloc(total_size, GFP_KERNEL);
	if (!comp)
		return NULL;

	/* configure comp new IPC message */
	comp->hdr.size = total_size;
	comp->hdr.cmd = SOF_IPC_GLB_TPLG_MSG | SOF_IPC_TPLG_COMP_NEW;
	comp->id = swidget->comp_id;
	comp->pipeline_id = index;
	comp->core = swidget->core;

	/* handle the extended data if needed */
	if (total_size > *ipc_size) {
		/* append extended data to the end of the component */
		memcpy((u8 *)comp + *ipc_size, &swidget->uuid, ext_size);
		comp->ext_data_length = ext_size;
	}

	/* update ipc_size and return */
	*ipc_size = total_size;
	return comp;
}

static void sof_dbg_comp_config(struct snd_soc_component *scomp, struct sof_ipc_comp_config *config)
{
	dev_dbg(scomp->dev, " config: periods snk %d src %d fmt %d\n",
		config->periods_sink, config->periods_source,
		config->frame_fmt);
}

static int sof_ipc3_widget_setup_comp_host(struct snd_sof_widget *swidget)
{
	struct snd_soc_component *scomp = swidget->scomp;
	struct sof_ipc_comp_host *host;
	size_t ipc_size = sizeof(*host);
	int ret;

	host = sof_comp_alloc(swidget, &ipc_size, swidget->pipeline_id);
	if (!host)
		return -ENOMEM;
	swidget->private = host;

	/* configure host comp IPC message */
	host->comp.type = SOF_COMP_HOST;
	host->config.hdr.size = sizeof(host->config);

	if (swidget->id == snd_soc_dapm_aif_out)
		host->direction = SOF_IPC_STREAM_CAPTURE;
	else
		host->direction = SOF_IPC_STREAM_PLAYBACK;

	/* parse one set of pcm_tokens */
	ret = sof_update_ipc_object(scomp, host, SOF_PCM_TOKENS, swidget->tuples,
				    swidget->num_tuples, sizeof(*host), 1);
	if (ret < 0)
		goto err;

	/* parse one set of comp_tokens */
	ret = sof_update_ipc_object(scomp, &host->config, SOF_COMP_TOKENS, swidget->tuples,
				    swidget->num_tuples, sizeof(host->config), 1);
	if (ret < 0)
		goto err;

	dev_dbg(scomp->dev, "loaded host %s\n", swidget->widget->name);
	sof_dbg_comp_config(scomp, &host->config);

	return 0;
err:
	kfree(swidget->private);
	swidget->private = NULL;

	return ret;
}

static void sof_ipc3_widget_free_comp(struct snd_sof_widget *swidget)
{
	kfree(swidget->private);
}

/* token list for each topology object */
static enum sof_tokens host_token_list[] = {
	SOF_CORE_TOKENS,
	SOF_COMP_EXT_TOKENS,
	SOF_PCM_TOKENS,
	SOF_COMP_TOKENS,
};

static const struct sof_ipc_tplg_widget_ops tplg_ipc3_widget_ops[SND_SOC_DAPM_TYPE_COUNT] = {
	[snd_soc_dapm_aif_in] =  {sof_ipc3_widget_setup_comp_host, sof_ipc3_widget_free_comp,
				  host_token_list, ARRAY_SIZE(host_token_list), NULL},
	[snd_soc_dapm_aif_out] = {sof_ipc3_widget_setup_comp_host, sof_ipc3_widget_free_comp,
				  host_token_list, ARRAY_SIZE(host_token_list), NULL},
};

static const struct sof_ipc_tplg_ops ipc3_tplg_ops = {
	.widget = tplg_ipc3_widget_ops,
	.token_list = ipc3_token_list,
};

const struct sof_ipc_ops ipc3_ops = {
	.tplg = &ipc3_tplg_ops,
};
