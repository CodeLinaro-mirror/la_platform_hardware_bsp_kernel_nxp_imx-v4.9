/*
 * LTC1695 fan controller driver
 *
 * Copyright (C) 2018 Google, Inc.
 *
 * Keun young Park <keunyoung@google.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/i2c.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/sysfs.h>
#include <linux/thermal.h>


struct ltc1695_data {
	struct i2c_client *client;

	struct device *hwmon_dev;
	struct thermal_cooling_device *cdev;

	u8 pwm_value;
};

#define PWM_VALUE_MAX 0x3f
#define PWM_BOOST_START_TIMER_FLAG 0x40


static int ltc1695_set_pwm_reg(struct i2c_client *client, u8 value)
{
	u8 reg_value = value | PWM_BOOST_START_TIMER_FLAG;
	return i2c_smbus_write_byte(client, reg_value);
}


static ssize_t get_pwm(struct device *dev, struct device_attribute *da,
		       char *buf)
{
	struct ltc1695_data *data = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", data->pwm_value);
}

static ssize_t set_pwm(struct device *dev, struct device_attribute *da,
		       const char *buf, size_t count)
{
	struct ltc1695_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	unsigned long val;

	if (kstrtoul(buf, 10, &val))
		return -EINVAL;
	data->pwm_value = clamp_val(val, 0, PWM_VALUE_MAX);
	pr_info("ltc1695 set_pwm %d\n", data->pwm_value);
	ltc1695_set_pwm_reg(client, data->pwm_value);

	return count;
}

static DEVICE_ATTR(pwm1, S_IWUSR | S_IRUGO, get_pwm, set_pwm);

static struct attribute *ltc1695_attrs[] = {
	&dev_attr_pwm1.attr,
	NULL
};

ATTRIBUTE_GROUPS(ltc1695);

/* thermal cooling device callbacks */
static int ltc1695_get_max_state(struct thermal_cooling_device *cdev,
				 unsigned long *state)
{
	struct ltc1695_data *data = cdev->devdata;

	if (!data)
		return -EINVAL;

	// map each pwm value to state
	*state = PWM_VALUE_MAX;

	return 0;
}

static int ltc1695_get_cur_state(struct thermal_cooling_device *cdev,
				 unsigned long *state)
{
	struct ltc1695_data *data = cdev->devdata;

	if (!data)
		return -EINVAL;

	*state = data->pwm_value;

	return 0;
}

static int ltc1695_set_cur_state(struct thermal_cooling_device *cdev, unsigned long state)
{
	struct ltc1695_data *data = cdev->devdata;

	if (!data)
		return -EINVAL;

	if (state == data->pwm_value) {
		return 0;
	}

	data->pwm_value = clamp_val(state, 0, PWM_VALUE_MAX);
	pr_info("ltc1695 set_pwm %d\n", data->pwm_value);
	ltc1695_set_pwm_reg(data->client, data->pwm_value);

	return 0;
}

static const struct thermal_cooling_device_ops ltc1695_cooling_ops = {
	.get_max_state = ltc1695_get_max_state,
	.get_cur_state = ltc1695_get_cur_state,
	.set_cur_state = ltc1695_set_cur_state,
};


static int ltc1695_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct ltc1695_data *data;
	struct device *hwmon_dev;
	struct thermal_cooling_device *cdev;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE_DATA)) {
		pr_err("Cannot find ltc1695\n");
		return -EIO;
	}

	data = devm_kzalloc(dev, sizeof(struct ltc1695_data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->client = client;

	data->pwm_value = PWM_VALUE_MAX;
	ltc1695_set_pwm_reg(client, data->pwm_value);
	pr_info("Found ltc1695, id 0x%x, current pwm:%d\n", id->driver_data,
		data->pwm_value);

	hwmon_dev = devm_hwmon_device_register_with_groups(dev, "ltc1695",
							   data,
							   ltc1695_groups);
	if (IS_ERR(hwmon_dev)) {
		dev_err(dev, "Failed to register ltc1695 device\n");
		return PTR_ERR(hwmon_dev);
	}
	data->hwmon_dev = hwmon_dev;

	if (IS_ENABLED(CONFIG_THERMAL)) {
		cdev = thermal_of_cooling_device_register(dev->of_node,
							  "ltc1695", data,
							  &ltc1695_cooling_ops);
		if (IS_ERR(cdev)) {
			dev_err(dev,
				"Failed to register pwm-fan as cooling device");
			return PTR_ERR(cdev);
		}
		data->cdev = cdev;
	}

	return 0;
}

static const struct i2c_device_id ltc1695_id[] = {
	{ "ltc1695", 0x74 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ltc1695_id);

static struct i2c_driver ltc1695_driver = {
	.driver = {
		.name	= "ltc1695",
	},
	.probe	  = ltc1695_probe,
	.id_table = ltc1695_id,
};

module_i2c_driver(ltc1695_driver);

MODULE_AUTHOR("Keun young Park <keunyoung@google.com>");
MODULE_DESCRIPTION("LTC1695 driver");
MODULE_LICENSE("GPL");
