#include <linux/export.h>
#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/kernel.h>
#include <linux/spi/spi.h>
#include <linux/slab.h>

#include <linux/iio/iio.h>
#include <linux/iio/buffer.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/triggered_buffer.h>

#include  "adis.h"

#define ADIS_MAX_OUTPUTS 12

static int adis_read_buffer_data(struct adis *adis, struct iio_dev *indio_dev)
{
	int n_outputs = indio_dev->num_channels;
	struct spi_transfer xfers[ADIS_MAX_OUTPUTS + 1];
	struct spi_message msg;
	int ret;
	int i;

	mutex_lock(&adis->txrx_lock);

	spi_message_init(&msg);

	memset(xfers, 0, sizeof(xfers));
	for (i = 0; i <= n_outputs; i++) {
		xfers[i].bits_per_word = 8;
		xfers[i].cs_change = 1;
		xfers[i].len = 2;
		xfers[i].delay_usecs = adis->data->read_delay;
		if (i < n_outputs) {
			xfers[i].tx_buf = adis->tx + 2 * i;
			adis->tx[2 * i] = indio_dev->channels[i].address;
			adis->tx[2 * i + 1] = 0;
		}
		if (i >= 1)
			xfers[i].rx_buf = adis->rx + 2 * (i - 1);
		spi_message_add_tail(&xfers[i], &msg);
	}

	ret = spi_sync(adis->spi, &msg);
	if (ret)
		dev_err(&adis->spi->dev, "Failed to read data: %d", ret);

	mutex_unlock(&adis->txrx_lock);

	return ret;
}

static irqreturn_t adis_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct adis *adis = iio_device_get_drvdata(indio_dev);
	u16 *data;
	int i = 0;

	data = kmalloc(indio_dev->scan_bytes, GFP_KERNEL);
	if (data == NULL) {
		dev_err(&adis->spi->dev, "Failed to allocate memory.");
		return -ENOMEM;
	}

	if (!bitmap_empty(indio_dev->active_scan_mask, indio_dev->masklength)
	    && adis_read_buffer_data(adis, indio_dev) >= 0)
		for (; i < bitmap_weight(indio_dev->active_scan_mask,
					 indio_dev->masklength); i++)
			data[i] = be16_to_cpup((__be16 *)&(adis->rx[i*2]));

	/* Guaranteed to be aligned with 8 byte boundary */
	if (indio_dev->scan_timestamp)
		*((s64 *)(PTR_ALIGN(data, sizeof(s64)))) = pf->timestamp;

	iio_push_to_buffers(indio_dev, (u8 *)data);

	iio_trigger_notify_done(indio_dev->trig);
	kfree(data);

	return IRQ_HANDLED;
}

/**
 * adis_setup_buffer_and_trigger() - Sets up buffer and trigger for the adis device
 * @adis: The adis device.
 * @indio_dev: The IIO device.
 * @trigger_handler: Optional trigger handler, may be NULL.
 *
 * Returns 0 on success, a negative error code otherwise.
 *
 * This function sets up the buffer and trigger for a adis devices.  If
 * 'trigger_handler' is NULL the default trigger handler will be used. The
 * default trigger handler will simply read the registers assigned to the
 * currently active channels.
 *
 * adis_cleanup_buffer_and_trigger() should be called to free the resources
 * allocated by this function.
 */
int adis_setup_buffer_and_trigger(struct adis *adis, struct iio_dev *indio_dev,
	irqreturn_t (*trigger_handler)(int, void *))
{
	int ret;

	if (!trigger_handler)
		trigger_handler = adis_trigger_handler;

	ret = iio_triggered_buffer_setup(indio_dev, &iio_pollfunc_store_time,
		trigger_handler, NULL);
	if (ret)
		return ret;

	if (adis->spi->irq) {
		ret = adis_probe_trigger(adis, indio_dev);
		if (ret)
			goto error_buffer_cleanup;
	}
	return 0;

error_buffer_cleanup:
	iio_triggered_buffer_cleanup(indio_dev);
	return ret;
}
EXPORT_SYMBOL_GPL(adis_setup_buffer_and_trigger);

/**
 * adis_cleanup_buffer_and_trigger() - Free buffer and trigger resources
 * @adis: The adis device.
 * @indio_dev: The IIO device.
 *
 * Frees resources allocated by adis_setup_buffer_and_trigger()
 */
void adis_cleanup_buffer_and_trigger(struct adis *adis,
	struct iio_dev *indio_dev)
{
	if (adis->spi->irq)
		adis_remove_trigger(adis);
	iio_triggered_buffer_cleanup(indio_dev);
}
EXPORT_SYMBOL_GPL(adis_cleanup_buffer_and_trigger);
