// SPDX-License-Identifier: GPL-2.0
/*
 * Support for GalaxyCore GC0310 VGA camera sensor.
 *
 * Copyright (c) 2013 Intel Corporation. All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/kmod.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/moduleparam.h>
#include <media/v4l2-device.h>
#include <linux/io.h>
#include "../include/linux/atomisp_gmin_platform.h"

#include "gc0310.h"

/*
 * gc0310_write_reg_array - Initializes a list of GC0310 registers
 * @client: i2c driver client structure
 * @reglist: list of registers to be written
 * @count: number of register, value pairs in the list
 */
static int gc0310_write_reg_array(struct i2c_client *client,
				  const struct gc0310_reg *reglist, int count)
{
	int i, err;

	for (i = 0; i < count; i++) {
		err = i2c_smbus_write_byte_data(client, reglist[i].reg, reglist[i].val);
		if (err) {
			dev_err(&client->dev, "write error: wrote 0x%x to offset 0x%x error %d",
				reglist[i].val, reglist[i].reg, err);
			return err;
		}
	}

	return 0;
}

static int gc0310_exposure_set(struct gc0310_device *dev, u32 exp)
{
	struct i2c_client *client = v4l2_get_subdevdata(&dev->sd);

	return i2c_smbus_write_word_swapped(client, GC0310_AEC_PK_EXPO_H, exp);
}

static int gc0310_gain_set(struct gc0310_device *dev, u32 gain)
{
	struct i2c_client *client = v4l2_get_subdevdata(&dev->sd);
	u8 again, dgain;
	int ret;

	/* Taken from original driver, this never sets dgain lower then 32? */

	/* Change 0 - 95 to 32 - 127 */
	gain += 32;

	if (gain < 64) {
		again = 0x0; /* sqrt(2) */
		dgain = gain;
	} else {
		again = 0x2; /* 2 * sqrt(2) */
		dgain = gain / 2;
	}

	ret = i2c_smbus_write_byte_data(client, GC0310_AGC_ADJ, again);
	if (ret)
		return ret;

	return i2c_smbus_write_byte_data(client, GC0310_DGC_ADJ, dgain);
}

static int gc0310_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct gc0310_device *dev =
		container_of(ctrl->handler, struct gc0310_device, ctrls.handler);
	int ret;

	if (!dev->power_on)
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		ret = gc0310_exposure_set(dev, ctrl->val);
		break;
	case V4L2_CID_GAIN:
		ret = gc0310_gain_set(dev, ctrl->val);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static const struct v4l2_ctrl_ops ctrl_ops = {
	.s_ctrl = gc0310_s_ctrl,
};

static int power_ctrl(struct v4l2_subdev *sd, bool flag)
{
	int ret = 0;
	struct gc0310_device *dev = to_gc0310_sensor(sd);

	if (!dev || !dev->platform_data)
		return -ENODEV;

	if (flag) {
		/* The upstream module driver (written to Crystal
		 * Cove) had this logic to pulse the rails low first.
		 * This appears to break things on the MRD7 with the
		 * X-Powers PMIC...
		 *
		 *     ret = dev->platform_data->v1p8_ctrl(sd, 0);
		 *     ret |= dev->platform_data->v2p8_ctrl(sd, 0);
		 *     mdelay(50);
		 */
		ret |= dev->platform_data->v1p8_ctrl(sd, 1);
		ret |= dev->platform_data->v2p8_ctrl(sd, 1);
		usleep_range(10000, 15000);
	}

	if (!flag || ret) {
		ret |= dev->platform_data->v1p8_ctrl(sd, 0);
		ret |= dev->platform_data->v2p8_ctrl(sd, 0);
	}
	return ret;
}

static int gpio_ctrl(struct v4l2_subdev *sd, bool flag)
{
	int ret;
	struct gc0310_device *dev = to_gc0310_sensor(sd);

	if (!dev || !dev->platform_data)
		return -ENODEV;

	/* GPIO0 == "reset" (active low), GPIO1 == "power down" */
	if (flag) {
		/* Pulse reset, then release power down */
		ret = dev->platform_data->gpio0_ctrl(sd, 0);
		usleep_range(5000, 10000);
		ret |= dev->platform_data->gpio0_ctrl(sd, 1);
		usleep_range(10000, 15000);
		ret |= dev->platform_data->gpio1_ctrl(sd, 0);
		usleep_range(10000, 15000);
	} else {
		ret = dev->platform_data->gpio1_ctrl(sd, 1);
		ret |= dev->platform_data->gpio0_ctrl(sd, 0);
	}
	return ret;
}

static int power_up(struct v4l2_subdev *sd)
{
	struct gc0310_device *dev = to_gc0310_sensor(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret;

	if (!dev->platform_data) {
		dev_err(&client->dev,
			"no camera_sensor_platform_data");
		return -ENODEV;
	}

	if (dev->power_on)
		return 0; /* Already on */

	/* power control */
	ret = power_ctrl(sd, 1);
	if (ret)
		goto fail_power;

	/* flis clock control */
	ret = dev->platform_data->flisclk_ctrl(sd, 1);
	if (ret)
		goto fail_clk;

	/* gpio ctrl */
	ret = gpio_ctrl(sd, 1);
	if (ret) {
		ret = gpio_ctrl(sd, 1);
		if (ret)
			goto fail_gpio;
	}

	msleep(100);

	dev->power_on = true;
	return 0;

fail_gpio:
	dev->platform_data->flisclk_ctrl(sd, 0);
fail_clk:
	power_ctrl(sd, 0);
fail_power:
	dev_err(&client->dev, "sensor power-up failed\n");

	return ret;
}

static int power_down(struct v4l2_subdev *sd)
{
	struct gc0310_device *dev = to_gc0310_sensor(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret = 0;

	if (!dev->platform_data) {
		dev_err(&client->dev,
			"no camera_sensor_platform_data");
		return -ENODEV;
	}

	if (!dev->power_on)
		return 0; /* Already off */

	/* gpio ctrl */
	ret = gpio_ctrl(sd, 0);
	if (ret) {
		ret = gpio_ctrl(sd, 0);
		if (ret)
			dev_err(&client->dev, "gpio failed 2\n");
	}

	ret = dev->platform_data->flisclk_ctrl(sd, 0);
	if (ret)
		dev_err(&client->dev, "flisclk failed\n");

	/* power control */
	ret = power_ctrl(sd, 0);
	if (ret)
		dev_err(&client->dev, "vprog failed.\n");

	dev->power_on = false;
	return ret;
}

static struct v4l2_mbus_framefmt *
gc0310_get_pad_format(struct gc0310_device *dev,
		      struct v4l2_subdev_state *state,
		      unsigned int pad, enum v4l2_subdev_format_whence which)
{
	if (which == V4L2_SUBDEV_FORMAT_TRY)
		return v4l2_subdev_get_try_format(&dev->sd, state, pad);

	return &dev->mode.fmt;
}

/* The GC0310 currently only supports 1 fixed fmt */
static void gc0310_fill_format(struct v4l2_mbus_framefmt *fmt)
{
	memset(fmt, 0, sizeof(*fmt));
	fmt->width = GC0310_NATIVE_WIDTH;
	fmt->height = GC0310_NATIVE_HEIGHT;
	fmt->field = V4L2_FIELD_NONE;
	fmt->code = MEDIA_BUS_FMT_SGRBG8_1X8;
}

static int gc0310_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *sd_state,
			  struct v4l2_subdev_format *format)
{
	struct gc0310_device *dev = to_gc0310_sensor(sd);
	struct v4l2_mbus_framefmt *fmt;

	fmt = gc0310_get_pad_format(dev, sd_state, format->pad, format->which);
	gc0310_fill_format(fmt);

	format->format = *fmt;
	return 0;
}

static int gc0310_get_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *sd_state,
			  struct v4l2_subdev_format *format)
{
	struct gc0310_device *dev = to_gc0310_sensor(sd);
	struct v4l2_mbus_framefmt *fmt;

	fmt = gc0310_get_pad_format(dev, sd_state, format->pad, format->which);
	format->format = *fmt;
	return 0;
}

static int gc0310_detect(struct i2c_client *client)
{
	struct i2c_adapter *adapter = client->adapter;
	int ret;

	if (!i2c_check_functionality(adapter, I2C_FUNC_I2C))
		return -ENODEV;

	ret = i2c_smbus_read_word_swapped(client, GC0310_SC_CMMN_CHIP_ID_H);
	if (ret < 0) {
		dev_err(&client->dev, "read sensor_id failed: %d\n", ret);
		return -ENODEV;
	}

	dev_dbg(&client->dev, "sensor ID = 0x%x\n", ret);

	if (ret != GC0310_ID) {
		dev_err(&client->dev, "sensor ID error, read id = 0x%x, target id = 0x%x\n",
			ret, GC0310_ID);
		return -ENODEV;
	}

	dev_dbg(&client->dev, "detect gc0310 success\n");

	return 0;
}

static int gc0310_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct gc0310_device *dev = to_gc0310_sensor(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret;

	dev_dbg(&client->dev, "%s S enable=%d\n", __func__, enable);
	mutex_lock(&dev->input_lock);

	if (enable) {
		ret = power_up(sd);
		if (ret)
			goto error_unlock;

		ret = gc0310_write_reg_array(client, gc0310_reset_register,
					     ARRAY_SIZE(gc0310_reset_register));
		if (ret)
			goto error_power_down;

		ret = gc0310_write_reg_array(client, gc0310_VGA_30fps,
					     ARRAY_SIZE(gc0310_VGA_30fps));
		if (ret)
			goto error_power_down;

		/* restore value of all ctrls */
		ret = __v4l2_ctrl_handler_setup(&dev->ctrls.handler);
		if (ret)
			goto error_power_down;

		/* enable per frame MIPI and sensor ctrl reset  */
		ret = i2c_smbus_write_byte_data(client, 0xFE, 0x30);
		if (ret)
			goto error_power_down;
	}

	ret = i2c_smbus_write_byte_data(client, GC0310_RESET_RELATED, GC0310_REGISTER_PAGE_3);
	if (ret)
		goto error_power_down;

	ret = i2c_smbus_write_byte_data(client, GC0310_SW_STREAM,
					enable ? GC0310_START_STREAMING : GC0310_STOP_STREAMING);
	if (ret)
		goto error_power_down;

	ret = i2c_smbus_write_byte_data(client, GC0310_RESET_RELATED, GC0310_REGISTER_PAGE_0);
	if (ret)
		goto error_power_down;

	if (!enable)
		power_down(sd);

	mutex_unlock(&dev->input_lock);
	return 0;

error_power_down:
	power_down(sd);
error_unlock:
	mutex_unlock(&dev->input_lock);
	return ret;
}

static int gc0310_s_config(struct v4l2_subdev *sd,
			   int irq, void *platform_data)
{
	struct gc0310_device *dev = to_gc0310_sensor(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret = 0;

	if (!platform_data)
		return -ENODEV;

	dev->platform_data =
	    (struct camera_sensor_platform_data *)platform_data;

	mutex_lock(&dev->input_lock);
	/* power off the module, then power on it in future
	 * as first power on by board may not fulfill the
	 * power on sequqence needed by the module
	 */
	dev->power_on = true; /* force power_down() to run */
	ret = power_down(sd);
	if (ret) {
		dev_err(&client->dev, "gc0310 power-off err.\n");
		goto fail_power_off;
	}

	ret = power_up(sd);
	if (ret) {
		dev_err(&client->dev, "gc0310 power-up err.\n");
		goto fail_power_on;
	}

	ret = dev->platform_data->csi_cfg(sd, 1);
	if (ret)
		goto fail_csi_cfg;

	/* config & detect sensor */
	ret = gc0310_detect(client);
	if (ret) {
		dev_err(&client->dev, "gc0310_detect err s_config.\n");
		goto fail_csi_cfg;
	}

	/* turn off sensor, after probed */
	ret = power_down(sd);
	if (ret) {
		dev_err(&client->dev, "gc0310 power-off err.\n");
		goto fail_csi_cfg;
	}
	mutex_unlock(&dev->input_lock);

	return 0;

fail_csi_cfg:
	dev->platform_data->csi_cfg(sd, 0);
fail_power_on:
	power_down(sd);
	dev_err(&client->dev, "sensor power-gating failed\n");
fail_power_off:
	mutex_unlock(&dev->input_lock);
	return ret;
}

static int gc0310_g_frame_interval(struct v4l2_subdev *sd,
				   struct v4l2_subdev_frame_interval *interval)
{
	interval->interval.numerator = 1;
	interval->interval.denominator = GC0310_FPS;

	return 0;
}

static int gc0310_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	/* We support only a single format */
	if (code->index)
		return -EINVAL;

	code->code = MEDIA_BUS_FMT_SGRBG8_1X8;
	return 0;
}

static int gc0310_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	/* We support only a single resolution */
	if (fse->index)
		return -EINVAL;

	fse->min_width = GC0310_NATIVE_WIDTH;
	fse->max_width = GC0310_NATIVE_WIDTH;
	fse->min_height = GC0310_NATIVE_HEIGHT;
	fse->max_height = GC0310_NATIVE_HEIGHT;

	return 0;
}

static int gc0310_g_skip_frames(struct v4l2_subdev *sd, u32 *frames)
{
	*frames = GC0310_SKIP_FRAMES;
	return 0;
}

static const struct v4l2_subdev_sensor_ops gc0310_sensor_ops = {
	.g_skip_frames	= gc0310_g_skip_frames,
};

static const struct v4l2_subdev_video_ops gc0310_video_ops = {
	.s_stream = gc0310_s_stream,
	.g_frame_interval = gc0310_g_frame_interval,
};

static const struct v4l2_subdev_pad_ops gc0310_pad_ops = {
	.enum_mbus_code = gc0310_enum_mbus_code,
	.enum_frame_size = gc0310_enum_frame_size,
	.get_fmt = gc0310_get_fmt,
	.set_fmt = gc0310_set_fmt,
};

static const struct v4l2_subdev_ops gc0310_ops = {
	.video = &gc0310_video_ops,
	.pad = &gc0310_pad_ops,
	.sensor = &gc0310_sensor_ops,
};

static int gc0310_init_controls(struct gc0310_device *dev)
{
	struct v4l2_ctrl_handler *hdl = &dev->ctrls.handler;

	v4l2_ctrl_handler_init(hdl, 2);

	/* Use the same lock for controls as for everything else */
	hdl->lock = &dev->input_lock;
	dev->sd.ctrl_handler = hdl;

	dev->ctrls.exposure =
		v4l2_ctrl_new_std(hdl, &ctrl_ops, V4L2_CID_EXPOSURE, 0, 4095, 1, 1023);

	/* 32 steps at base gain 1 + 64 half steps at base gain 2 */
	dev->ctrls.gain =
		v4l2_ctrl_new_std(hdl, &ctrl_ops, V4L2_CID_GAIN, 0, 95, 1, 31);

	return hdl->error;
}

static void gc0310_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct gc0310_device *dev = to_gc0310_sensor(sd);

	dev_dbg(&client->dev, "gc0310_remove...\n");

	dev->platform_data->csi_cfg(sd, 0);

	v4l2_device_unregister_subdev(sd);
	media_entity_cleanup(&dev->sd.entity);
	v4l2_ctrl_handler_free(&dev->ctrls.handler);
	kfree(dev);
}

static int gc0310_probe(struct i2c_client *client)
{
	struct gc0310_device *dev;
	int ret;
	void *pdata;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	mutex_init(&dev->input_lock);
	v4l2_i2c_subdev_init(&dev->sd, client, &gc0310_ops);
	gc0310_fill_format(&dev->mode.fmt);

	pdata = gmin_camera_platform_data(&dev->sd,
					  ATOMISP_INPUT_FORMAT_RAW_8,
					  atomisp_bayer_order_grbg);
	if (!pdata) {
		ret = -EINVAL;
		goto out_free;
	}

	ret = gc0310_s_config(&dev->sd, client->irq, pdata);
	if (ret)
		goto out_free;

	ret = atomisp_register_i2c_module(&dev->sd, pdata, RAW_CAMERA);
	if (ret)
		goto out_free;

	dev->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	dev->pad.flags = MEDIA_PAD_FL_SOURCE;
	dev->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	ret = gc0310_init_controls(dev);
	if (ret) {
		gc0310_remove(client);
		return ret;
	}

	ret = media_entity_pads_init(&dev->sd.entity, 1, &dev->pad);
	if (ret)
		gc0310_remove(client);

	return ret;
out_free:
	v4l2_device_unregister_subdev(&dev->sd);
	kfree(dev);
	return ret;
}

static const struct acpi_device_id gc0310_acpi_match[] = {
	{"XXGC0310"},
	{"INT0310"},
	{},
};
MODULE_DEVICE_TABLE(acpi, gc0310_acpi_match);

static struct i2c_driver gc0310_driver = {
	.driver = {
		.name = "gc0310",
		.acpi_match_table = gc0310_acpi_match,
	},
	.probe_new = gc0310_probe,
	.remove = gc0310_remove,
};
module_i2c_driver(gc0310_driver);

MODULE_AUTHOR("Lai, Angie <angie.lai@intel.com>");
MODULE_DESCRIPTION("A low-level driver for GalaxyCore GC0310 sensors");
MODULE_LICENSE("GPL");
