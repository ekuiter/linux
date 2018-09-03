/*
 * HDMI Connector driver
 *
 * Copyright (C) 2013 Texas Instruments Incorporated - http://www.ti.com/
 * Author: Tomi Valkeinen <tomi.valkeinen@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 */

#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include <drm/drm_edid.h>

#include "../dss/omapdss.h"

static const struct videomode hdmic_default_vm = {
	.hactive	= 640,
	.vactive	= 480,
	.pixelclock	= 25175000,
	.hsync_len	= 96,
	.hfront_porch	= 16,
	.hback_porch	= 48,
	.vsync_len	= 2,
	.vfront_porch	= 11,
	.vback_porch	= 31,

	.flags		= DISPLAY_FLAGS_HSYNC_LOW | DISPLAY_FLAGS_VSYNC_LOW,
};

struct panel_drv_data {
	struct omap_dss_device dssdev;
	void (*hpd_cb)(void *cb_data, enum drm_connector_status status);
	void *hpd_cb_data;
	bool hpd_enabled;
	struct mutex hpd_lock;

	struct device *dev;

	struct videomode vm;

	struct gpio_desc *hpd_gpio;
};

#define to_panel_data(x) container_of(x, struct panel_drv_data, dssdev)

static int hdmic_connect(struct omap_dss_device *src,
			 struct omap_dss_device *dst)
{
	return 0;
}

static void hdmic_disconnect(struct omap_dss_device *src,
			     struct omap_dss_device *dst)
{
}

static int hdmic_enable(struct omap_dss_device *dssdev)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *src = dssdev->src;
	int r;

	dev_dbg(ddata->dev, "enable\n");

	if (!omapdss_device_is_connected(dssdev))
		return -ENODEV;

	if (omapdss_device_is_enabled(dssdev))
		return 0;

	src->ops->set_timings(src, &ddata->vm);

	r = src->ops->enable(src);
	if (r)
		return r;

	dssdev->state = OMAP_DSS_DISPLAY_ACTIVE;

	return r;
}

static void hdmic_disable(struct omap_dss_device *dssdev)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *src = dssdev->src;

	dev_dbg(ddata->dev, "disable\n");

	if (!omapdss_device_is_enabled(dssdev))
		return;

	src->ops->disable(src);

	dssdev->state = OMAP_DSS_DISPLAY_DISABLED;
}

static void hdmic_set_timings(struct omap_dss_device *dssdev,
			      struct videomode *vm)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *src = dssdev->src;

	ddata->vm = *vm;

	src->ops->set_timings(src, vm);
}

static void hdmic_get_timings(struct omap_dss_device *dssdev,
			      struct videomode *vm)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);

	*vm = ddata->vm;
}

static int hdmic_check_timings(struct omap_dss_device *dssdev,
			       struct videomode *vm)
{
	struct omap_dss_device *src = dssdev->src;

	return src->ops->check_timings(src, vm);
}

static int hdmic_read_edid(struct omap_dss_device *dssdev,
		u8 *edid, int len)
{
	struct omap_dss_device *src = dssdev->src;

	return src->ops->read_edid(src, edid, len);
}

static bool hdmic_detect(struct omap_dss_device *dssdev)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *src = dssdev->src;
	bool connected;

	if (ddata->hpd_gpio)
		connected = gpiod_get_value_cansleep(ddata->hpd_gpio);
	else
		connected = src->ops->detect(src);
	if (!connected && src->ops->hdmi.lost_hotplug)
		src->ops->hdmi.lost_hotplug(src);
	return connected;
}

static int hdmic_register_hpd_cb(struct omap_dss_device *dssdev,
				 void (*cb)(void *cb_data,
					    enum drm_connector_status status),
				 void *cb_data)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *src = dssdev->src;

	if (ddata->hpd_gpio) {
		mutex_lock(&ddata->hpd_lock);
		ddata->hpd_cb = cb;
		ddata->hpd_cb_data = cb_data;
		mutex_unlock(&ddata->hpd_lock);
		return 0;
	} else if (src->ops->register_hpd_cb) {
		return src->ops->register_hpd_cb(src, cb, cb_data);
	}

	return -ENOTSUPP;
}

static void hdmic_unregister_hpd_cb(struct omap_dss_device *dssdev)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *src = dssdev->src;

	if (ddata->hpd_gpio) {
		mutex_lock(&ddata->hpd_lock);
		ddata->hpd_cb = NULL;
		ddata->hpd_cb_data = NULL;
		mutex_unlock(&ddata->hpd_lock);
	} else if (src->ops->unregister_hpd_cb) {
		src->ops->unregister_hpd_cb(src);
	}
}

static void hdmic_enable_hpd(struct omap_dss_device *dssdev)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *src = dssdev->src;

	if (ddata->hpd_gpio) {
		mutex_lock(&ddata->hpd_lock);
		ddata->hpd_enabled = true;
		mutex_unlock(&ddata->hpd_lock);
	} else if (src->ops->enable_hpd) {
		src->ops->enable_hpd(src);
	}
}

static void hdmic_disable_hpd(struct omap_dss_device *dssdev)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *src = dssdev->src;

	if (ddata->hpd_gpio) {
		mutex_lock(&ddata->hpd_lock);
		ddata->hpd_enabled = false;
		mutex_unlock(&ddata->hpd_lock);
	} else if (src->ops->disable_hpd) {
		src->ops->disable_hpd(src);
	}
}

static int hdmic_set_hdmi_mode(struct omap_dss_device *dssdev, bool hdmi_mode)
{
	struct omap_dss_device *src = dssdev->src;

	return src->ops->hdmi.set_hdmi_mode(src, hdmi_mode);
}

static int hdmic_set_infoframe(struct omap_dss_device *dssdev,
		const struct hdmi_avi_infoframe *avi)
{
	struct omap_dss_device *src = dssdev->src;

	return src->ops->hdmi.set_infoframe(src, avi);
}

static const struct omap_dss_device_ops hdmic_ops = {
	.connect		= hdmic_connect,
	.disconnect		= hdmic_disconnect,

	.enable			= hdmic_enable,
	.disable		= hdmic_disable,

	.set_timings		= hdmic_set_timings,
	.get_timings		= hdmic_get_timings,
	.check_timings		= hdmic_check_timings,

	.read_edid		= hdmic_read_edid,
	.detect			= hdmic_detect,
	.register_hpd_cb	= hdmic_register_hpd_cb,
	.unregister_hpd_cb	= hdmic_unregister_hpd_cb,
	.enable_hpd		= hdmic_enable_hpd,
	.disable_hpd		= hdmic_disable_hpd,

	.hdmi = {
		.set_hdmi_mode	= hdmic_set_hdmi_mode,
		.set_infoframe	= hdmic_set_infoframe,
	},
};

static irqreturn_t hdmic_hpd_isr(int irq, void *data)
{
	struct panel_drv_data *ddata = data;

	mutex_lock(&ddata->hpd_lock);
	if (ddata->hpd_enabled && ddata->hpd_cb) {
		enum drm_connector_status status;

		if (hdmic_detect(&ddata->dssdev))
			status = connector_status_connected;
		else
			status = connector_status_disconnected;

		ddata->hpd_cb(ddata->hpd_cb_data, status);
	}
	mutex_unlock(&ddata->hpd_lock);

	return IRQ_HANDLED;
}

static int hdmic_probe(struct platform_device *pdev)
{
	struct panel_drv_data *ddata;
	struct omap_dss_device *dssdev;
	struct gpio_desc *gpio;
	int r;

	ddata = devm_kzalloc(&pdev->dev, sizeof(*ddata), GFP_KERNEL);
	if (!ddata)
		return -ENOMEM;

	platform_set_drvdata(pdev, ddata);
	ddata->dev = &pdev->dev;

	mutex_init(&ddata->hpd_lock);

	/* HPD GPIO */
	gpio = devm_gpiod_get_optional(&pdev->dev, "hpd", GPIOD_IN);
	if (IS_ERR(gpio)) {
		dev_err(&pdev->dev, "failed to parse HPD gpio\n");
		return PTR_ERR(gpio);
	}

	ddata->hpd_gpio = gpio;

	if (ddata->hpd_gpio) {
		r = devm_request_threaded_irq(&pdev->dev,
				gpiod_to_irq(ddata->hpd_gpio),
				NULL, hdmic_hpd_isr,
				IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING |
				IRQF_ONESHOT,
				"hdmic hpd", ddata);
		if (r)
			return r;
	}

	ddata->vm = hdmic_default_vm;

	dssdev = &ddata->dssdev;
	dssdev->ops = &hdmic_ops;
	dssdev->dev = &pdev->dev;
	dssdev->type = OMAP_DISPLAY_TYPE_HDMI;
	dssdev->owner = THIS_MODULE;
	dssdev->of_ports = BIT(0);

	omapdss_display_init(dssdev);
	omapdss_device_register(dssdev);

	return 0;
}

static int __exit hdmic_remove(struct platform_device *pdev)
{
	struct panel_drv_data *ddata = platform_get_drvdata(pdev);
	struct omap_dss_device *dssdev = &ddata->dssdev;

	omapdss_device_unregister(&ddata->dssdev);

	hdmic_disable(dssdev);

	return 0;
}

static const struct of_device_id hdmic_of_match[] = {
	{ .compatible = "omapdss,hdmi-connector", },
	{},
};

MODULE_DEVICE_TABLE(of, hdmic_of_match);

static struct platform_driver hdmi_connector_driver = {
	.probe	= hdmic_probe,
	.remove	= __exit_p(hdmic_remove),
	.driver	= {
		.name	= "connector-hdmi",
		.of_match_table = hdmic_of_match,
		.suppress_bind_attrs = true,
	},
};

module_platform_driver(hdmi_connector_driver);

MODULE_AUTHOR("Tomi Valkeinen <tomi.valkeinen@ti.com>");
MODULE_DESCRIPTION("HDMI Connector driver");
MODULE_LICENSE("GPL");
