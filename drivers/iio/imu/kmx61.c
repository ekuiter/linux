/*
 * KMX61 - Kionix 6-axis Accelerometer/Magnetometer
 *
 * Copyright (c) 2014, Intel Corporation.
 *
 * This file is subject to the terms and conditions of version 2 of
 * the GNU General Public License.  See the file COPYING in the main
 * directory of this archive for more details.
 *
 * IIO driver for KMX61 (7-bit I2C slave address 0x0E or 0x0F).
 *
 * TODO: buffer, interrupt, thresholds, acpi, temperature sensor
 *
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>

#define KMX61_DRV_NAME "kmx61"

#define KMX61_REG_WHO_AM_I	0x00

/*
 * three 16-bit accelerometer output registers for X/Y/Z axis
 * we use only XOUT_L as a base register, all other addresses
 * can be obtained by applying an offset and are provided here
 * only for clarity.
 */
#define KMX61_ACC_XOUT_L	0x0A
#define KMX61_ACC_XOUT_H	0x0B
#define KMX61_ACC_YOUT_L	0x0C
#define KMX61_ACC_YOUT_H	0x0D
#define KMX61_ACC_ZOUT_L	0x0E
#define KMX61_ACC_ZOUT_H	0x0F

/*
 * one 16-bit temperature output register
 */
#define KMX61_TEMP_L		0x10
#define KMX61_TEMP_H		0x11

/*
 * three 16-bit magnetometer output registers for X/Y/Z axis
 */
#define KMX61_MAG_XOUT_L	0x12
#define KMX61_MAG_XOUT_H	0x13
#define KMX61_MAG_YOUT_L	0x14
#define KMX61_MAG_YOUT_H	0x15
#define KMX61_MAG_ZOUT_L	0x16
#define KMX61_MAG_ZOUT_H	0x17

#define KMX61_REG_ODCNTL	0x2C
#define KMX61_REG_STBY		0x29
#define KMX61_REG_CTRL1		0x2A

#define KMX61_ACC_STBY_BIT	BIT(0)
#define KMX61_MAG_STBY_BIT	BIT(1)
#define KMX61_ACT_STBY_BIT	BIT(7)

#define KMX61_ALL_STBY		(KMX61_ACC_STBY_BIT | KMX61_MAG_STBY_BIT)

#define KMX61_REG_CTRL1_GSEL0_SHIFT	0
#define KMX61_REG_CTRL1_GSEL1_SHIFT	1
#define KMX61_REG_CTRL1_GSEL0_MASK	0x01
#define KMX61_REG_CTRL1_GSEL1_MASK	0x02

#define KMX61_REG_CTRL1_BIT_RES		BIT(4)

#define KMX61_ACC_ODR_SHIFT	0
#define KMX61_MAG_ODR_SHIFT	4
#define KMX61_ACC_ODR_MASK	0x0F
#define KMX61_MAG_ODR_MASK	0xF0

#define KMX61_SLEEP_DELAY_MS	2000

#define KMX61_CHIP_ID		0x12

struct kmx61_data {
	struct i2c_client *client;

	/* serialize access to non-atomic ops, e.g set_mode */
	struct mutex lock;
	u8 range;
	u8 odr_bits;

	/* standby state */
	u8 acc_stby;
	u8 mag_stby;

	/* power state */
	bool acc_ps;
	bool mag_ps;
};

enum kmx61_range {
	KMX61_RANGE_2G,
	KMX61_RANGE_4G,
	KMX61_RANGE_8G,
};

enum kmx61_scan {
	KMX61_SCAN_ACC_X,
	KMX61_SCAN_ACC_Y,
	KMX61_SCAN_ACC_Z,
	KMX61_SCAN_TEMP,
	KMX61_SCAN_MAG_X,
	KMX61_SCAN_MAG_Y,
	KMX61_SCAN_MAG_Z,
};

static const struct {
	u16 uscale;
	u8 gsel0;
	u8 gsel1;
} kmx61_scale_table[] = {
	{9582, 0, 0},
	{19163, 1, 0},
	{38326, 0, 1},
};

/* KMX61 devices */
#define KMX61_ACC	0x01
#define KMX61_MAG	0x02

static const struct {
	int val;
	int val2;
	u8 odr_bits;
} kmx61_samp_freq_table[] = { {12, 500000, 0x00},
			{25, 0, 0x01},
			{50, 0, 0x02},
			{100, 0, 0x03},
			{200, 0, 0x04},
			{400, 0, 0x05},
			{800, 0, 0x06},
			{1600, 0, 0x07},
			{0, 781000, 0x08},
			{1, 563000, 0x09},
			{3, 125000, 0x0A},
			{6, 250000, 0x0B} };

static IIO_CONST_ATTR(accel_scale_available, "0.009582 0.019163 0.038326");
static IIO_CONST_ATTR(magn_scale_available, "0.001465");
static IIO_CONST_ATTR_SAMP_FREQ_AVAIL(
	"0.781000 1.563000 3.125000 6.250000 12.500000 25 50 100 200 400 800");

static struct attribute *kmx61_attributes[] = {
	&iio_const_attr_accel_scale_available.dev_attr.attr,
	&iio_const_attr_magn_scale_available.dev_attr.attr,
	&iio_const_attr_sampling_frequency_available.dev_attr.attr,
	NULL,
};

static const struct attribute_group kmx61_attribute_group = {
	.attrs = kmx61_attributes,
};

#define KMX61_ACC_CHAN(_axis, _index) { \
	.type = IIO_ACCEL, \
	.modified = 1, \
	.channel2 = IIO_MOD_ ## _axis, \
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW), \
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE) | \
				BIT(IIO_CHAN_INFO_SAMP_FREQ), \
	.address = KMX61_ACC, \
	.scan_index = _index, \
	.scan_type = { \
		.sign = 's', \
		.realbits = 12, \
		.storagebits = 16, \
		.shift = 4, \
		.endianness = IIO_LE, \
	}, \
}

#define KMX61_MAG_CHAN(_axis, _index) { \
	.type = IIO_MAGN, \
	.modified = 1, \
	.channel2 = IIO_MOD_ ## _axis, \
	.address = KMX61_MAG, \
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW), \
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE) | \
				BIT(IIO_CHAN_INFO_SAMP_FREQ), \
	.scan_index = _index, \
	.scan_type = { \
		.sign = 's', \
		.realbits = 14, \
		.storagebits = 16, \
		.shift = 2, \
		.endianness = IIO_LE, \
	}, \
}

static const struct iio_chan_spec kmx61_channels[] = {
	KMX61_ACC_CHAN(X, KMX61_SCAN_ACC_X),
	KMX61_ACC_CHAN(Y, KMX61_SCAN_ACC_Y),
	KMX61_ACC_CHAN(Z, KMX61_SCAN_ACC_Z),
	KMX61_MAG_CHAN(X, KMX61_SCAN_MAG_X),
	KMX61_MAG_CHAN(Y, KMX61_SCAN_MAG_Y),
	KMX61_MAG_CHAN(Z, KMX61_SCAN_MAG_Z),
};

static int kmx61_convert_freq_to_bit(int val, int val2)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(kmx61_samp_freq_table); i++)
		if (val == kmx61_samp_freq_table[i].val &&
		    val2 == kmx61_samp_freq_table[i].val2)
			return kmx61_samp_freq_table[i].odr_bits;
	return -EINVAL;
}
/**
 * kmx61_set_mode() - set KMX61 device operating mode
 * @data - kmx61 device private data pointer
 * @mode - bitmask, indicating operating mode for @device
 * @device - bitmask, indicating device for which @mode needs to be set
 * @update - update stby bits stored in device's private  @data
 *
 * For each sensor (accelerometer/magnetometer) there are two operating modes
 * STANDBY and OPERATION. Neither accel nor magn can be disabled independently
 * if they are both enabled. Internal sensors state is saved in acc_stby and
 * mag_stby members of driver's private @data.
 */
static int kmx61_set_mode(struct kmx61_data *data, u8 mode, u8 device,
			  bool update)
{
	int ret;
	int acc_stby = -1, mag_stby = -1;

	ret = i2c_smbus_read_byte_data(data->client, KMX61_REG_STBY);
	if (ret < 0) {
		dev_err(&data->client->dev, "Error reading reg_stby\n");
		return ret;
	}
	if (device & KMX61_ACC) {
		if (mode & KMX61_ACC_STBY_BIT) {
			ret |= KMX61_ACC_STBY_BIT;
			acc_stby = 1;
		} else {
			ret &= ~KMX61_ACC_STBY_BIT;
			acc_stby = 0;
		}
	}

	if (device & KMX61_MAG) {
		if (mode & KMX61_MAG_STBY_BIT) {
			ret |= KMX61_MAG_STBY_BIT;
			mag_stby = 1;
		} else {
			ret &= ~KMX61_MAG_STBY_BIT;
			mag_stby = 0;
		}
	}

	ret = i2c_smbus_write_byte_data(data->client, KMX61_REG_STBY, ret);
	if (ret < 0) {
		dev_err(&data->client->dev, "Error writing reg_stby\n");
		return ret;
	}

	if (acc_stby != -1 && update)
		data->acc_stby = !!acc_stby;
	if (mag_stby != -1 && update)
		data->mag_stby = !!mag_stby;

	return ret;
}

static int kmx61_get_mode(struct kmx61_data *data, u8 *mode, u8 device)
{
	int ret;

	ret = i2c_smbus_read_byte_data(data->client, KMX61_REG_STBY);
	if (ret < 0) {
		dev_err(&data->client->dev, "Error reading reg_stby\n");
		return ret;
	}
	*mode = 0;

	if (device & KMX61_ACC) {
		if (ret & KMX61_ACC_STBY_BIT)
			*mode |= KMX61_ACC_STBY_BIT;
		else
			*mode &= ~KMX61_ACC_STBY_BIT;
	}

	if (device & KMX61_MAG) {
		if (ret & KMX61_MAG_STBY_BIT)
			*mode |= KMX61_MAG_STBY_BIT;
		else
			*mode &= ~KMX61_MAG_STBY_BIT;
	}

	return 0;
}

static int kmx61_set_odr(struct kmx61_data *data, int val, int val2, u8 device)
{
	int ret;
	u8 mode;
	int lodr_bits, odr_bits;

	ret = kmx61_get_mode(data, &mode, KMX61_ACC | KMX61_MAG);
	if (ret < 0)
		return ret;

	lodr_bits = kmx61_convert_freq_to_bit(val, val2);
	if (lodr_bits < 0)
		return lodr_bits;

	/* To change ODR, accel and magn must be in STDBY */
	ret = kmx61_set_mode(data, KMX61_ALL_STBY, KMX61_ACC | KMX61_MAG,
			     true);
	if (ret < 0)
		return ret;

	odr_bits = 0;
	if (device & KMX61_ACC)
		odr_bits |= lodr_bits;
	if (device & KMX61_MAG)
		odr_bits |= (lodr_bits << KMX61_MAG_ODR_SHIFT);

	ret = i2c_smbus_write_byte_data(data->client, KMX61_REG_ODCNTL,
					odr_bits);
	if (ret < 0)
		return ret;

	ret = kmx61_set_mode(data, mode, KMX61_ACC | KMX61_MAG, true);
	if (ret < 0)
		return ret;

	data->odr_bits = lodr_bits;

	return 0;
}

static
int kmx61_get_odr(struct kmx61_data *data, int *val, int *val2, u8 device)
{	int i;
	u8 lodr_bits;

	if (device & KMX61_ACC)
		lodr_bits = (data->odr_bits >> KMX61_ACC_ODR_SHIFT) &
			     KMX61_ACC_ODR_MASK;
	else if (device & KMX61_MAG)
		lodr_bits = (data->odr_bits >> KMX61_MAG_ODR_SHIFT) &
			     KMX61_MAG_ODR_MASK;
	else
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(kmx61_samp_freq_table); i++)
		if (lodr_bits == kmx61_samp_freq_table[i].odr_bits) {
			*val = kmx61_samp_freq_table[i].val;
			*val2 = kmx61_samp_freq_table[i].val2;
			return 0;
		}
	return -EINVAL;
}

static int kmx61_set_range(struct kmx61_data *data, int range)
{
	int ret;

	ret = i2c_smbus_read_byte_data(data->client, KMX61_REG_CTRL1);
	if (ret < 0) {
		dev_err(&data->client->dev, "Error reading reg_ctrl1\n");
		return ret;
	}

	ret &= ~(KMX61_REG_CTRL1_GSEL0_MASK | KMX61_REG_CTRL1_GSEL1_MASK);
	ret |= kmx61_scale_table[range].gsel0 << KMX61_REG_CTRL1_GSEL0_SHIFT;
	ret |= kmx61_scale_table[range].gsel1 << KMX61_REG_CTRL1_GSEL1_SHIFT;

	ret = i2c_smbus_write_byte_data(data->client, KMX61_REG_CTRL1, ret);
	if (ret < 0) {
		dev_err(&data->client->dev, "Error writing reg_ctrl1\n");
		return ret;
	}

	data->range = range;

	return 0;
}

static int kmx61_set_scale(struct kmx61_data *data, int uscale)
{
	int ret, i;
	u8  mode;

	for (i = 0; i < ARRAY_SIZE(kmx61_scale_table); i++) {
		if (kmx61_scale_table[i].uscale == uscale) {
			ret = kmx61_get_mode(data, &mode,
					     KMX61_ACC | KMX61_MAG);
			if (ret < 0)
				return ret;

			ret = kmx61_set_mode(data, KMX61_ALL_STBY,
					     KMX61_ACC | KMX61_MAG, true);
			if (ret < 0)
				return ret;

			ret = kmx61_set_range(data, i);
			if (ret < 0)
				return ret;

			return  kmx61_set_mode(data, mode,
					       KMX61_ACC | KMX61_MAG, true);
		}
	}
	return -EINVAL;
}

static int kmx61_chip_init(struct kmx61_data *data)
{
	int ret;

	ret = i2c_smbus_read_byte_data(data->client, KMX61_REG_WHO_AM_I);
	if (ret < 0) {
		dev_err(&data->client->dev, "Error reading who_am_i\n");
		return ret;
	}

	if (ret != KMX61_CHIP_ID) {
		dev_err(&data->client->dev,
			"Wrong chip id, got %x expected %x\n",
			 ret, KMX61_CHIP_ID);
		return -EINVAL;
	}

	/* set accel 12bit, 4g range */
	ret = kmx61_set_range(data, KMX61_RANGE_4G);
	if (ret < 0)
		return ret;

	/* set acc/magn to OPERATION mode */
	ret = kmx61_set_mode(data, 0, KMX61_ACC | KMX61_MAG, true);
	if (ret < 0)
		return ret;

	return 0;
}
/**
 * kmx61_set_power_state() - set power state for kmx61 @device
 * @data - kmx61 device private pointer
 * @on - power state to be set for @device
 * @device - bitmask indicating device for which @on state needs to be set
 *
 * Notice that when ACC power state needs to be set to ON and MAG is in
 * OPERATION then we know that kmx61_runtime_resume was already called
 * so we must set ACC OPERATION mode here. The same happens when MAG power
 * state needs to be set to ON and ACC is in OPERATION.
 */
static int kmx61_set_power_state(struct kmx61_data *data, bool on, u8 device)
{
#ifdef CONFIG_PM_RUNTIME
	int ret;

	if (device & KMX61_ACC) {
		if (on && !data->acc_ps && !data->mag_stby)
			kmx61_set_mode(data, 0, KMX61_ACC, true);
		data->acc_ps = on;
	}
	if (device & KMX61_MAG) {
		if (on && !data->mag_ps && !data->acc_stby)
			kmx61_set_mode(data, 0, KMX61_MAG, true);
		data->mag_ps = on;
	}

	if (on) {
		ret = pm_runtime_get_sync(&data->client->dev);
	} else {
		pm_runtime_mark_last_busy(&data->client->dev);
		ret = pm_runtime_put_autosuspend(&data->client->dev);
	}
	if (ret < 0) {
		dev_err(&data->client->dev,
			"Failed: kmx61_set_power_state for %d, ret %d\n",
			on, ret);
		return ret;
	}
#endif
	return 0;
}

static int kmx61_read_measurement(struct kmx61_data *data, int base, int offset)
{
	int ret;
	u8 reg = base + offset * 2;

	ret = i2c_smbus_read_word_data(data->client, reg);
	if (ret < 0) {
		dev_err(&data->client->dev, "failed to read reg at %x\n", reg);
		return ret;
	}

	return ret;
}

static int kmx61_read_raw(struct iio_dev *indio_dev,
			      struct iio_chan_spec const *chan, int *val,
			      int *val2, long mask)
{
	struct kmx61_data *data = iio_priv(indio_dev);
	int ret;
	u8 base_reg;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		switch (chan->type) {
		case IIO_ACCEL:
		case IIO_MAGN:
			base_reg = KMX61_ACC_XOUT_L;
			break;
		default:
			return -EINVAL;
		}
		mutex_lock(&data->lock);

		kmx61_set_power_state(data, true, chan->address);
		ret = kmx61_read_measurement(data, base_reg, chan->scan_index);
		if (ret < 0) {
			kmx61_set_power_state(data, false, chan->address);
			mutex_unlock(&data->lock);
			return ret;
		}
		*val = sign_extend32(ret >> chan->scan_type.shift,
				     chan->scan_type.realbits - 1);
		kmx61_set_power_state(data, false, chan->address);

		mutex_unlock(&data->lock);
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		switch (chan->type) {
		case IIO_ACCEL:
			*val = 0;
			*val2 = kmx61_scale_table[data->range].uscale;
			return IIO_VAL_INT_PLUS_MICRO;
		case IIO_MAGN:
			/* 14 bits res, 1465 microGauss per magn count */
			*val = 0;
			*val2 = 1465;
			return IIO_VAL_INT_PLUS_MICRO;
		default:
			return -EINVAL;
		}
	case IIO_CHAN_INFO_SAMP_FREQ:
		if (chan->type != IIO_ACCEL && chan->type != IIO_MAGN)
			return -EINVAL;

		mutex_lock(&data->lock);
		ret = kmx61_get_odr(data, val, val2, chan->address);
		mutex_unlock(&data->lock);
		if (ret)
			return -EINVAL;
		return IIO_VAL_INT_PLUS_MICRO;
	}
	return -EINVAL;
}

static int kmx61_write_raw(struct iio_dev *indio_dev,
			       struct iio_chan_spec const *chan, int val,
			       int val2, long mask)
{
	struct kmx61_data *data = iio_priv(indio_dev);
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_SAMP_FREQ:
		if (chan->type != IIO_ACCEL && chan->type != IIO_MAGN)
			return -EINVAL;

		mutex_lock(&data->lock);
		ret = kmx61_set_odr(data, val, val2, chan->address);
		mutex_unlock(&data->lock);
		return ret;
	case IIO_CHAN_INFO_SCALE:
		switch (chan->type) {
		case IIO_ACCEL:
			if (val != 0)
				return -EINVAL;
			mutex_lock(&data->lock);
			ret = kmx61_set_scale(data, val2);
			mutex_unlock(&data->lock);
			return ret;
		default:
			return -EINVAL;
		}
		return ret;
	default:
		return -EINVAL;
	}
	return ret;
}

static const struct iio_info kmx61_info = {
	.driver_module		= THIS_MODULE,
	.read_raw		= kmx61_read_raw,
	.write_raw		= kmx61_write_raw,
	.attrs			= &kmx61_attribute_group,
};

static int kmx61_probe(struct i2c_client *client,
		       const struct i2c_device_id *id)
{
	struct kmx61_data *data;
	struct iio_dev *indio_dev;
	int ret;
	const char *name = NULL;

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	i2c_set_clientdata(client, indio_dev);
	data->client = client;

	if (id)
		name = id->name;

	indio_dev->dev.parent = &client->dev;
	indio_dev->channels = kmx61_channels;
	indio_dev->num_channels = ARRAY_SIZE(kmx61_channels);
	indio_dev->name = name;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->info = &kmx61_info;

	mutex_init(&data->lock);

	ret = kmx61_chip_init(data);
	if (ret < 0)
		return ret;

	ret = iio_device_register(indio_dev);
	if (ret < 0) {
		dev_err(&client->dev, "Failed to register iio device\n");
		goto err_iio_device_register;
	}

	ret = pm_runtime_set_active(&client->dev);
	if (ret < 0)
		goto err_pm_runtime_set_active;

	pm_runtime_enable(&client->dev);
	pm_runtime_set_autosuspend_delay(&client->dev, KMX61_SLEEP_DELAY_MS);
	pm_runtime_use_autosuspend(&client->dev);

	return 0;

err_pm_runtime_set_active:
	iio_device_unregister(indio_dev);
err_iio_device_register:
	kmx61_set_mode(data, KMX61_ALL_STBY, KMX61_ACC | KMX61_MAG, true);
	return ret;
}

static int kmx61_remove(struct i2c_client *client)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(client);
	struct kmx61_data *data = iio_priv(indio_dev);
	int ret;

	pm_runtime_disable(&client->dev);
	pm_runtime_set_suspended(&client->dev);
	pm_runtime_put_noidle(&client->dev);

	iio_device_unregister(indio_dev);

	mutex_lock(&data->lock);
	ret = kmx61_set_mode(data, KMX61_ALL_STBY, KMX61_ACC | KMX61_MAG, true);
	mutex_unlock(&data->lock);

	return ret;
}

#ifdef CONFIG_PM_SLEEP
static int kmx61_suspend(struct device *dev)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(to_i2c_client(dev));
	struct kmx61_data *data = iio_priv(indio_dev);
	int ret;

	mutex_lock(&data->lock);
	ret = kmx61_set_mode(data, KMX61_ALL_STBY, KMX61_ACC | KMX61_MAG,
			     false);
	mutex_unlock(&data->lock);

	return ret;
}

static int kmx61_resume(struct device *dev)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(to_i2c_client(dev));
	struct kmx61_data *data = iio_priv(indio_dev);
	u8 stby = 0;

	if (data->acc_stby)
		stby |= KMX61_ACC_STBY_BIT;
	if (data->mag_stby)
		stby |= KMX61_MAG_STBY_BIT;

	return kmx61_set_mode(data, stby, KMX61_ACC | KMX61_MAG, true);
}
#endif

#ifdef CONFIG_PM_RUNTIME
static int kmx61_runtime_suspend(struct device *dev)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(to_i2c_client(dev));
	struct kmx61_data *data = iio_priv(indio_dev);
	int ret;

	mutex_lock(&data->lock);
	ret = kmx61_set_mode(data, KMX61_ALL_STBY, KMX61_ACC | KMX61_MAG, true);
	mutex_unlock(&data->lock);

	return ret;
}

static int kmx61_runtime_resume(struct device *dev)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(to_i2c_client(dev));
	struct kmx61_data *data = iio_priv(indio_dev);
	u8 stby = 0;

	if (!data->acc_ps)
		stby |= KMX61_ACC_STBY_BIT;
	if (!data->mag_ps)
		stby |= KMX61_MAG_STBY_BIT;

	return kmx61_set_mode(data, stby, KMX61_ACC | KMX61_MAG, true);
}
#endif

static const struct dev_pm_ops kmx61_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(kmx61_suspend, kmx61_resume)
	SET_RUNTIME_PM_OPS(kmx61_runtime_suspend, kmx61_runtime_resume, NULL)
};

static const struct i2c_device_id kmx61_id[] = {
	{"kmx611021", 0},
	{}
};

MODULE_DEVICE_TABLE(i2c, kmx61_id);

static struct i2c_driver kmx61_driver = {
	.driver = {
		.name = KMX61_DRV_NAME,
		.pm = &kmx61_pm_ops,
	},
	.probe		= kmx61_probe,
	.remove		= kmx61_remove,
	.id_table	= kmx61_id,
};

module_i2c_driver(kmx61_driver);

MODULE_AUTHOR("Daniel Baluta <daniel.baluta@intel.com>");
MODULE_DESCRIPTION("KMX61 accelerometer/magnetometer driver");
MODULE_LICENSE("GPL v2");
