
/* drivers/leds/leds-vibrator-gpio.c
 *
 * Copyright (C) 2008 Google, Inc.
 * Copyright (C) 2018 XiaoMi, Inc.
 * Author: Mike Lockwood <lockwood@android.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/leds.h>
#include <linux/of_device.h>
#include <linux/hrtimer.h>
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>

struct vibrator_gpio_data {
	struct	platform_device *dev;
	struct	hrtimer vib_timer;
	struct	led_classdev led_dev;
	const	char *name;
	unsigned	int gpio;
	int	play_time_ms;
	int	timeout;
	u8	active_low;
	spinlock_t	lock;
};

static enum hrtimer_restart vibrator_gpio_timer_func(struct hrtimer *timer)
{
	struct vibrator_gpio_data *pdata =
		container_of(timer, struct vibrator_gpio_data, vib_timer);
	pr_debug("%s: vibrator_gpio_timer_func pdata->gpio=%d\n", __func__, pdata->gpio);

	gpio_direction_output(pdata->gpio, pdata->active_low ? 1 : 0);

	return HRTIMER_NORESTART;
}

#ifdef CONFIG_PM
static int vibrator_gpio_suspend(struct device *dev) {
	struct vibrator_gpio_data *pdata = dev_get_drvdata(dev);

	hrtimer_cancel(&pdata->vib_timer);
	gpio_direction_output(pdata->gpio, 0);

	return 0;
}
#else
#define vibrator_gpio_suspend NULL
#endif

static SIMPLE_DEV_PM_OPS(vibrator_gpio_pm_ops, vibrator_gpio_suspend, NULL);

static ssize_t vibrator_gpio_show_state(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", 0);
}

static ssize_t vibrator_gpio_store_state(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	return count;
}

static ssize_t vibrator_gpio_show_duration(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_dev = dev_get_drvdata(dev);
	struct vibrator_gpio_data *pdata = container_of(led_dev,
		struct vibrator_gpio_data, led_dev);
	ktime_t time_rem;
	s64 time_us = 0;

	if (hrtimer_active(&pdata->vib_timer)) {
		time_rem = hrtimer_get_remaining(&pdata->vib_timer);
		time_us = ktime_to_us(time_rem);
		return snprintf(buf, PAGE_SIZE, "%lld\n", time_us / 1000);
	} else
		return 0;
}

static ssize_t vibrator_gpio_store_duration(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct led_classdev *led_dev = dev_get_drvdata(dev);
	struct vibrator_gpio_data *pdata = container_of(led_dev,
		struct vibrator_gpio_data, led_dev);
	u32 val;
	int rc;
	unsigned long flags;

	rc = kstrtouint(buf, 0, &val);
	if (rc < 0)
		return rc;

	/* values greater than 0 are valid */
	if (val <= 0)
		return count;

	spin_lock_irqsave(&pdata->lock, flags);
	pdata->play_time_ms = val;
	pr_debug("%s: timer value=%d\n", __func__,
		pdata->play_time_ms);
	spin_unlock_irqrestore(&pdata->lock, flags);

	return count;
}

static ssize_t vibrator_gpio_show_activate(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	/* Nothing to show for now */
	return snprintf(buf, PAGE_SIZE, "%d\n", 0);
}

static ssize_t vibrator_gpio_store_activate(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct led_classdev *led_dev = dev_get_drvdata(dev);
	struct vibrator_gpio_data *pdata = container_of(led_dev,
		struct vibrator_gpio_data, led_dev);
	u32 val;
	int rc, timer = pdata->play_time_ms;
	unsigned long flags;

	rc = kstrtouint(buf, 0, &val);
	if (rc < 0)
		return rc;

	if (val != 0 && val != 1)
		return count;

	spin_lock_irqsave(&pdata->lock, flags);

	hrtimer_cancel(&pdata->vib_timer);
	gpio_direction_output(pdata->gpio, pdata->active_low ? !val : !!val);
	pr_debug("%s: gpio_enable data->gpio=%d\n", __func__, pdata->gpio);

	if (val > 0 || timer > 0) {
		if (val > pdata->timeout)
			val = pdata->timeout;

		hrtimer_start(&pdata->vib_timer,
			ktime_set(timer / 1000,
			(timer % 1000) * 1000000),
			HRTIMER_MODE_REL);
	}

	spin_unlock_irqrestore(&pdata->lock, flags);

	return count;
}

static struct device_attribute vibrator_gpio_attrs[] = {
	__ATTR(state, S_IRUGO | S_IWUSR | S_IWGRP,
		vibrator_gpio_show_state,
		vibrator_gpio_store_state),
	__ATTR(duration, S_IRUGO | S_IWUSR | S_IWGRP,
		vibrator_gpio_show_duration,
		vibrator_gpio_store_duration),
	__ATTR(activate, S_IRUGO | S_IWUSR | S_IWGRP,
		vibrator_gpio_show_activate,
		vibrator_gpio_store_activate),
};

/* Dummy functions for led class setting */
static enum led_brightness vibrator_gpio_brightness_get(struct led_classdev *cdev)
{
	return 0;
}

static void vibrator_gpio_brightness_set(struct led_classdev *cdev,
					enum led_brightness level)
{
}

static int vibrator_gpio_parse_dt(struct device *dev,struct vibrator_gpio_data *pdata)
{
	struct device_node *node = dev->of_node;
	int rc;

	u32 temp_val;

	pdata->gpio = of_get_named_gpio_flags(node,"qcom,vib-gpio_one",0, &temp_val);
	pr_debug("%s: gpio=%d flag=%d\n", __func__, pdata->gpio, temp_val);
	if (!gpio_is_valid(pdata->gpio)) {
		pr_err("%s: Failed to read vib-gpio_one\n", __func__);
		return -EINVAL;
	}

	if (NULL == (pdata->name = of_get_property(node, "label", NULL))) {
		pr_debug("%s: get label error\n", __func__);
		return 1;
	}
	pr_debug("%s: pdata-name=%s\n", __func__, pdata->name);
	rc = of_property_read_u32(node,"qcom,vibrator-timeout-ms", &pdata->timeout);

	if (rc) {
		pdata->timeout = 10000;
	}

	return 0;
}

static int vibrator_gpio_probe(struct platform_device *pdev)
{
	struct vibrator_gpio_data *pdata;

	int  ret, i;
	pr_info("%s: vibrator gpio probe enter\n", __func__);

	pdata =devm_kzalloc(&pdev->dev, sizeof(struct vibrator_gpio_data), GFP_KERNEL);
	if (!pdata)
		return -ENOMEM;


	ret= vibrator_gpio_parse_dt(&pdev->dev,pdata);
	if (ret) {
		pr_err("%s: DT parsing failed, ret=%d\n",__func__, ret);
		return ret;
	}

	hrtimer_init(&pdata->vib_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);

	spin_lock_init(&pdata->lock);
	pdata->vib_timer.function = vibrator_gpio_timer_func;

	pdata->led_dev.name = "vibrator";
	pdata->led_dev.brightness_get = vibrator_gpio_brightness_get;
	pdata->led_dev.brightness_set = vibrator_gpio_brightness_set;
	pdata->led_dev.max_brightness = LED_FULL;

	ret = gpio_request(pdata->gpio, pdata->name);
	if (ret < 0) {
		gpio_free(pdata->gpio);
		pr_err("%s: request gpio failed, ret=%d\n", __func__, ret);
		goto err_out;
	}

	ret = devm_led_classdev_register(&pdev->dev, &pdata->led_dev);
	if (ret < 0) {
		pr_err("%s: register vibrator led class device failed, ret=%d\n", __func__, ret);
		gpio_free(pdata->gpio);
		goto err_out;

	}

	pdata->active_low = 0;
	gpio_direction_output(pdata->gpio, 0);

	platform_set_drvdata(pdev, pdata);

	for (i = 0; i < ARRAY_SIZE(vibrator_gpio_attrs); i++) {
		ret = sysfs_create_file(&pdata->led_dev.dev->kobj,
				&vibrator_gpio_attrs[i].attr);
		if (ret < 0) {
			pr_err("%s: error in creating sysfs file, ret=%d\n", __func__, ret);
			goto sysfs_fail;
		}
	}

	pr_info("%s: vibrator gpio probe success\n", __func__);
	return 0;

sysfs_fail:
	for (--i; i >= 0; i--)
		sysfs_remove_file(&pdata->led_dev.dev->kobj,
				&vibrator_gpio_attrs[i].attr);
err_out:
	pr_err("%s: vibrator gpio probe failed\n", __func__);
	hrtimer_cancel(&pdata->vib_timer);
	kfree(pdata);
	return ret;
}

static int vibrator_gpio_remove(struct platform_device *pdev)
{
	struct vibrator_gpio_data *pdata = platform_get_drvdata(pdev);
	gpio_free(pdata->gpio);

	return 0;
}

static  struct of_device_id vibrator_gpio_machine_of_match[]  = {
	{ .compatible = "qcom,vibrator-gpio", },
	{},
};
MODULE_DEVICE_TABLE(of, vibrator_gpio_machine_of_match);

static struct platform_driver vibrator_gpio_driver = {
	.probe = vibrator_gpio_probe,
	.remove = vibrator_gpio_remove,
	.driver = {
		.name = "vibrator_gpio",
		.of_match_table = vibrator_gpio_machine_of_match,
		.pm	= &vibrator_gpio_pm_ops,
	},
};
module_platform_driver(vibrator_gpio_driver);

MODULE_AUTHOR("Jason LCT");
MODULE_DESCRIPTION("Vibrator gpio driver");
MODULE_LICENSE("GPL");
