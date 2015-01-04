/*
 *  linux/drivers/devfreq/governor_simpleondemand.c
 *
 *  Copyright (C) 2011 Samsung Electronics
 *	MyungJoo Ham <myungjoo.ham@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/errno.h>
#include <linux/module.h>
#include <linux/devfreq.h>
#include <linux/math64.h>
#include "governor.h"

#define DEVFREQ_SIMPLE_ONDEMAND	"simple_ondemand"
#define DFSO_UPTHRESHOLD	(80)
#define DFSO_DOWNDIFFERENTIAL	(20)

unsigned int dfso_upthreshold;
unsigned int dfso_downdifferential;
unsigned int dfso_simple_scaling;

static int devfreq_simple_ondemand_func(struct devfreq *df,
					unsigned long *freq,
					u32 *flag)
{
	struct devfreq_dev_status stat;
	int err;
	unsigned long long a, b;
	unsigned long max = (df->max_freq) ? df->max_freq : UINT_MAX;
	unsigned long min = (df->min_freq) ? df->min_freq : 0;

	stat.private_data = NULL;

	err = df->profile->get_dev_status(df->dev.parent, &stat);
	if (err)
		return err;

	/* Prevent overflow */
	if (stat.busy_time >= (1 << 24) || stat.total_time >= (1 << 24)) {
		stat.busy_time >>= 7;
		stat.total_time >>= 7;
	}

	if (dfso_simple_scaling) {
		if (stat.busy_time * 100 >
		    stat.total_time * dfso_upthreshold)
			*freq = max;
		else if (stat.busy_time * 100 <
		    stat.total_time * dfso_downdifferential)
			*freq = min;
		else
			*freq = df->previous_freq;
		return 0;
	}

	/* Assume MAX if it is going to be divided by zero */
	if (stat.total_time == 0) {
		*freq = max;
		return 0;
	}

	/* Set MAX if it's busy enough */
	if (stat.busy_time * 100 >
	    stat.total_time * dfso_upthreshold) {
		*freq = max;
		return 0;
	}

	/* Set MAX if we do not know the initial frequency */
	if (stat.current_frequency == 0) {
		*freq = max;
		return 0;
	}

	/* Keep the current frequency */
	if (stat.busy_time * 100 >
	    stat.total_time * (dfso_upthreshold - dfso_downdifferential)) {
		*freq = stat.current_frequency;
		return 0;
	}

	/* Set the desired frequency based on the load */
	a = stat.busy_time;
	a *= stat.current_frequency;
	b = div_u64(a, stat.total_time);
	b *= 100;
	b = div_u64(b, (dfso_upthreshold - dfso_downdifferential / 2));
	*freq = (unsigned long) b;

	if (df->min_freq && *freq < df->min_freq)
		*freq = df->min_freq;
	if (df->max_freq && *freq > df->max_freq)
		*freq = df->max_freq;

	return 0;
}

static ssize_t upthreshold_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", dfso_upthreshold);
}

static ssize_t upthreshold_store(struct device *dev,
				 struct device_attribute *attr, const char *buf,
				 size_t count)
{
	unsigned int val;

	sscanf(buf, "%d", &val);
	if (val > 100 || val < dfso_downdifferential)
		return -EINVAL;

	dfso_upthreshold = val;

	return count;
}

static ssize_t downdifferential_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", dfso_downdifferential);
}

static ssize_t downdifferential_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	unsigned int val;

	sscanf(buf, "%d", &val);
	if (val > dfso_upthreshold)
		return -EINVAL;

	dfso_downdifferential = val;

	return count;
}

static ssize_t simple_scaling_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", dfso_simple_scaling);
}

static ssize_t simple_scaling_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	unsigned int val;

	sscanf(buf, "%d", &val);
	if (val < 0 || val > 1)
		return -EINVAL;

	dfso_simple_scaling = val;

	return count;
}

static DEVICE_ATTR(upthreshold, 0644, upthreshold_show, upthreshold_store);
static DEVICE_ATTR(downdifferential, 0644, downdifferential_show,
		   downdifferential_store);
static DEVICE_ATTR(simple_scaling, 0644, simple_scaling_show,
		   simple_scaling_store);

static struct attribute *attrs[] = {
	&dev_attr_upthreshold.attr,
	&dev_attr_downdifferential.attr,
	&dev_attr_simple_scaling.attr,
	NULL,
};

static struct attribute_group attr_group = {
	.attrs = attrs,
	.name = DEVFREQ_SIMPLE_ONDEMAND,
};

static int devfreq_simple_ondemand_start(struct devfreq *devfreq)
{
	dfso_upthreshold = DFSO_UPTHRESHOLD;
	dfso_downdifferential = DFSO_DOWNDIFFERENTIAL;
	devfreq_monitor_start(devfreq);

	return devfreq_policy_add_files(devfreq, attr_group);
}

static int devfreq_simple_ondemand_stop(struct devfreq *devfreq)
{
	devfreq_policy_remove_files(devfreq, attr_group);
	devfreq_monitor_stop(devfreq);

	return 0;
}

static int devfreq_simple_ondemand_handler(struct devfreq *devfreq,
				unsigned int event, void *data)
{
	int ret = 0;

	switch (event) {
	case DEVFREQ_GOV_START:
		ret = devfreq_simple_ondemand_start(devfreq);
		break;

	case DEVFREQ_GOV_STOP:
		devfreq_simple_ondemand_stop(devfreq);
		break;

	case DEVFREQ_GOV_INTERVAL:
		devfreq_interval_update(devfreq, (unsigned int *)data);
		break;

	case DEVFREQ_GOV_SUSPEND:
		devfreq_monitor_suspend(devfreq);
		break;

	case DEVFREQ_GOV_RESUME:
		devfreq_monitor_resume(devfreq);
		break;

	default:
		break;
	}

	return ret;
}

static struct devfreq_governor devfreq_simple_ondemand = {
	.name = DEVFREQ_SIMPLE_ONDEMAND,
	.get_target_freq = devfreq_simple_ondemand_func,
	.event_handler = devfreq_simple_ondemand_handler,
};

static int __init devfreq_simple_ondemand_init(void)
{
	return devfreq_add_governor(&devfreq_simple_ondemand);
}
subsys_initcall(devfreq_simple_ondemand_init);

static void __exit devfreq_simple_ondemand_exit(void)
{
	int ret;

	ret = devfreq_remove_governor(&devfreq_simple_ondemand);
	if (ret)
		pr_err("%s: failed remove governor %d\n", __func__, ret);

	return;
}
module_exit(devfreq_simple_ondemand_exit);
MODULE_LICENSE("GPL");
