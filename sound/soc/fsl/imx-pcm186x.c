/*
 * Copyright 2018 NXP
 *
 * The code contained herein is licensed under the GNU General Public
 * License. You may obtain a copy of the GNU General Public License
 * Version 2 or later at the following locations:
 *
 * http://www.opensource.org/licenses/gpl-license.html
 * http://www.gnu.org/copyleft/gpl.html
 */

#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/i2c.h>
#include <linux/of_gpio.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/clk.h>
#include <sound/soc.h>
#include <sound/jack.h>
#include <sound/control.h>
#include <sound/pcm_params.h>
#include <sound/soc-dapm.h>
#include <linux/pinctrl/consumer.h>
#include "fsl_sai.h"

#define RX 0
#define TX 1

// Ref 9.3.16.4, in DSP mode, 256 BCK per frame.
// Slot number can be caculated based on sample bits.
#define BCK_RATIO 256

/**
 * CPU private data
 *
 * @sysclk_id[2]: SYSCLK ids for set_sysclk()
 * @slots: number of slots supported by DAI
 *
 * Note: [0] for rx and [1] for tx
 */
struct cpu_priv {
	u32 sysclk_id[2];
	u32 slots;
};

struct imx_pcm186x_data {
	struct snd_soc_dai_link dai[3];
	struct snd_soc_card card;
	struct cpu_priv cpu_priv;
	bool  is_stream_opened[2];
	u32 tx_cb_slave;
	u32 tx_cf_slave;
	u32 rx_cb_slave;
	u32 rx_cf_slave;
	struct clk *codec_clk;
};

static unsigned int supported_rates[] = {
	16000, 48000,
};

static struct snd_pcm_hw_constraint_list constraint_rates = {
	.count = ARRAY_SIZE(supported_rates),
	.list  = supported_rates,
	.mask = 0,
};

static unsigned int supported_channels[] = {
	1, 2, 4,
};

static struct snd_pcm_hw_constraint_list constraint_channels = {
	.count = ARRAY_SIZE(supported_channels),
	.list = supported_channels,
	.mask = 0,
};

static int imx_pcm186x_startup(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	int ret;

	ret = snd_pcm_hw_constraint_list(runtime, 0, SNDRV_PCM_HW_PARAM_RATE,
						&constraint_rates);
	if (ret)
		return ret;

	ret = snd_pcm_hw_constraint_list(runtime, 0, SNDRV_PCM_HW_PARAM_CHANNELS,
						&constraint_channels);
	if (ret)
		return ret;

	return 0;
}

static int imx_pcm186x_hw_params(struct snd_pcm_substream *substream,
				     struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct imx_pcm186x_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct cpu_priv *cpu_priv = &data->cpu_priv;
	struct device *dev = rtd->card->dev;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	unsigned int fmt = SND_SOC_DAIFMT_DSP_B | SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_CBS_CFS;
	int ret;
	unsigned int mask;
	unsigned int codec_mask;

	dev_info(dev, "%s(), imx_pcm186x_hw_params, chns %d, rate %d, format 0x%x, width %d\n",
		__func__, params_channels(params), params_rate(params),
		params_format(params), params_width(params));

	cpu_priv->slots = BCK_RATIO/params_width(params);

	/* set codec slot, for pcm186x codec sai, 1 means enable enable the slot */
	codec_mask = ((0x1 << params_channels(params)) - 1);
	/* set cpu slot, for cpu sai, 0 means enable enable the slot */
	mask = ~codec_mask;

	ret = snd_soc_dai_set_tdm_slot(rtd->cpu_dai, mask, mask, cpu_priv->slots,
					params_width(params));
	if (ret) {
		dev_err(dev, "failed to set cpu dai tdm slot: %d\n", ret);
		return ret;
	}

	ret = snd_soc_dai_set_tdm_slot(codec_dai, codec_mask, codec_mask, cpu_priv->slots,
	        params_width(params));
	if (ret) {
	  dev_err(dev, "failed to set codec dai tdm slot: %d\n", ret);
	  return ret;
	}

	/* set cpu DAI format */
	ret = snd_soc_dai_set_fmt(rtd->cpu_dai, fmt);
	if (ret) {
		dev_err(dev, "failed to set cpu dai fmt: %d\n", ret);
		return ret;
	}

	/* set codec DAI format */
	ret = snd_soc_dai_set_fmt(codec_dai, fmt);
	if (ret) {
		dev_err(dev, "failed to set codec dai fmt: %d\n", ret);
		return ret;
	}

	/* set bit clock, in DAP mode, the pcm186x BCK need to be 256 * LRCK */
	ret = snd_soc_dai_set_sysclk(rtd->cpu_dai, FSL_SAI_CLK_BIT,
					params_rate(params) * 256, SND_SOC_CLOCK_OUT);
	if (ret) {
		dev_err(dev, "failed to set cpu sysclk FSL_SAI_CLK_BIT: %d\n", ret);
		return ret;
	}

	return 0;
}

static int imx_pcm186x_hw_free(struct snd_pcm_substream *substream)
{
	return 0;
}

static void imx_pcm186x_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;
	struct imx_pcm186x_data *data = snd_soc_card_get_drvdata(card);
	bool tx = substream->stream == SNDRV_PCM_STREAM_PLAYBACK;

	data->is_stream_opened[tx] = false;
}

static const struct snd_soc_dapm_route audio_map[] = {
	{"CPU-Capture",  NULL, "Capture"},
};

static struct snd_soc_ops imx_pcm186x_ops = {
	.startup = imx_pcm186x_startup,
	.shutdown  = imx_pcm186x_shutdown,
	.hw_params = imx_pcm186x_hw_params,
	.hw_free = imx_pcm186x_hw_free,
};

static int imx_pcm186x_probe(struct platform_device *pdev)
{
	struct device_node *cpu_np = NULL;
	struct device_node *codec_np = NULL;
	struct platform_device *cpu_pdev;
	struct imx_pcm186x_data *data;
	int ret = 0;
	int err;

	cpu_np = of_parse_phandle(pdev->dev.of_node, "cpu-dai", 0);
	if (!cpu_np) {
		dev_err(&pdev->dev, "cpu dai phandle missing or invalid\n");
		ret = -EINVAL;
		goto fail;
	}

	codec_np = of_parse_phandle(pdev->dev.of_node, "audio-codec", 0);
	if(!codec_np) {
		dev_err(&pdev->dev, "codec dai phandle missing or invalid\n");
		goto fail;
	}

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data) {
		ret = -ENOMEM;
		goto fail;
	}

	cpu_pdev = of_find_device_by_node(cpu_np);
	if (!cpu_pdev) {
		dev_err(&pdev->dev, "failed to find SAI platform device\n");
		ret = -EINVAL;
		goto fail;
	}

	data->codec_clk = devm_clk_get(&pdev->dev, "mclk");
	if (IS_ERR(data->codec_clk)) {
		data->codec_clk = NULL;
	} else {
		dev_info(&pdev->dev, "get mclk\n");
		ret = clk_prepare_enable(data->codec_clk);
		if (ret) {
			dev_err(&pdev->dev, "Failed to enable MCLK: %d\n", ret);
			goto fail;
		}
	}

	data->cpu_priv.sysclk_id[TX] = FSL_SAI_CLK_MAST1;
	data->cpu_priv.sysclk_id[RX] = FSL_SAI_CLK_MAST1;

	/* pcm186x defalut bsp mode, 32 bits, 8 slots */
	err = of_property_read_u32(pdev->dev.of_node, "slots", &data->cpu_priv.slots);
	if(err)	data->cpu_priv.slots = 8;
	dev_info(&pdev->dev, "slots %d\n", data->cpu_priv.slots);

	data->dai[0].name = "hifi";
	data->dai[0].stream_name = "hifi";
	data->dai[0].codec_dai_name = "pcm1865-aif";
	data->dai[0].cpu_dai_name = dev_name(&cpu_pdev->dev);
	data->dai[0].platform_of_node = cpu_np;
	data->dai[0].codec_of_node = codec_np;
	data->dai[0].ops = &imx_pcm186x_ops;
	data->dai[0].playback_only = false;
	data->dai[0].capture_only = true;
	data->card.num_links = 1;
	data->card.dai_link = data->dai;


	data->card.dapm_routes = audio_map,
	data->card.num_dapm_routes = ARRAY_SIZE(audio_map),
	data->card.dev = &pdev->dev;
	data->card.owner = THIS_MODULE;
	ret = snd_soc_of_parse_card_name(&data->card, "model");
	if (ret)
		goto fail;

	ret = snd_soc_of_parse_audio_routing(&data->card, "audio-routing");
	if (ret)
		goto fail;

	platform_set_drvdata(pdev, &data->card);
	snd_soc_card_set_drvdata(&data->card, data);
	ret = devm_snd_soc_register_card(&pdev->dev, &data->card);
	if (ret) {
		dev_err(&pdev->dev, "snd_soc_register_card failed (%d)\n", ret);
		goto fail;
	}

fail:
	if (cpu_np)
		of_node_put(cpu_np);
	if (codec_np)
		of_node_put(codec_np);

	return ret;
}

static const struct of_device_id imx_pcm186x_dt_ids[] = {
	{ .compatible = "fsl,imx-audio-pcm186x", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, imx_pcm186x_dt_ids);

static struct platform_driver imx_pcm186x_driver = {
	.driver = {
		.name = "imx-pcm186x",
		.owner = THIS_MODULE,
		.pm = &snd_soc_pm_ops,
		.of_match_table = imx_pcm186x_dt_ids,
},
	.probe = imx_pcm186x_probe,
};
module_platform_driver(imx_pcm186x_driver);

MODULE_AUTHOR("hui.fang@nxp.com");
MODULE_DESCRIPTION("NXP i.MX pcm186x ASoC machine driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:imx-pcm186x");
