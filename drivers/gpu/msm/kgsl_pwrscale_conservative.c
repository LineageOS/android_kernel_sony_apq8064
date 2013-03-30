/* Copyright (c) 2010-2012, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/spinlock.h>
#include <mach/socinfo.h>
#include <mach/scm.h>

#include "kgsl.h"
#include "kgsl_pwrscale.h"
#include "kgsl_device.h"

//250 msec max interval for now, 10msec min...
#define MIN_SAMPLE_INTERVAL 10000
#define MAX_SAMPLE_INTERVAL 250000

unsigned long walltime_total = 0;
unsigned long busytime_total = 0;

unsigned int gpu_sample_interval = 100000;

#define ADRENO_CLOCK_STEPS 5

typedef struct adreno_clock_thresholdEntry{
	unsigned int up_threshold;
	unsigned int down_threshold;
}adreno_clock_threshold_table;

adreno_clock_threshold_table adreno_clock_thresholds[ADRENO_CLOCK_STEPS]={
  {110,60},
  {90,45},
  {80,45},
  {50,0},
  {100,0} };

static ssize_t conservative_sample_interval_show(struct kgsl_device *device,
					   struct kgsl_pwrscale *pwrscale,
					   char *buf)
{
  return sprintf(buf,"%u\n",gpu_sample_interval);
}

static ssize_t conservative_sample_interval_store(struct kgsl_device *device,
						  struct kgsl_pwrscale *pwrscale,
						  const char *buf, size_t count)
{
  unsigned int input;
  int ret;
  ret = sscanf(buf,"%u",&input);

  if(ret != 1 || input > MAX_SAMPLE_INTERVAL || input < MIN_SAMPLE_INTERVAL)
    return -EINVAL;
  
  gpu_sample_interval = input;
  return count;
}

PWRSCALE_POLICY_ATTR(sample_interval, 0644, conservative_sample_interval_show, conservative_sample_interval_store);

static struct attribute *conservative_attrs[] = {
	&policy_attr_sample_interval.attr,
	NULL
};

static struct attribute_group conservative_attr_group = {
	.attrs = conservative_attrs,
};
				  
static void conservative_wake(struct kgsl_device *device, struct kgsl_pwrscale *pwrscale)
{
  struct kgsl_power_stats stats;
  printk("%s: GPU waking up\n",__func__);
  if (device->state != KGSL_STATE_NAP) {
    kgsl_pwrctrl_pwrlevel_change(device,
				 device->pwrctrl.default_pwrlevel);
    //reset the power stats counters;
    device->ftbl->power_stats(device,&stats);
    walltime_total = 0;
    busytime_total = 0;
  }
    
}

static void conservative_idle(struct kgsl_device *device, struct kgsl_pwrscale *pwrscale,
						unsigned int ignore_idle)
{
	struct kgsl_pwrctrl *pwr = &device->pwrctrl;
	struct kgsl_power_stats stats;
	int val = 0;
	unsigned int loadpct;

	if (ignore_idle)
		return;

	device->ftbl->power_stats(device, &stats);
	if (stats.total_time == 0)
		return;

	walltime_total += (unsigned long) stats.total_time;
	busytime_total += (unsigned long) stats.busy_time;
	if(walltime_total > gpu_sample_interval)
	  {
	    pr_debug("%s: walltime_total = %lu, busytime_total = %lu\n",__func__,walltime_total,busytime_total);

	    loadpct = (100*busytime_total)/walltime_total;
	    pr_debug("%s: loadpct = %d\n",__func__,loadpct);

	    walltime_total = busytime_total = 0;

	    if(loadpct < adreno_clock_thresholds[pwr->active_pwrlevel].down_threshold)
	      val = 1;
	    else if(loadpct > adreno_clock_thresholds[pwr->active_pwrlevel].up_threshold)
	      val = -1;

	    pr_debug("%s: active_pwrlevel = %d, change = %d\n",__func__,pwr->active_pwrlevel,val);

	    if (val)
	      kgsl_pwrctrl_pwrlevel_change(device,
					   pwr->active_pwrlevel + val);
	  }
}

static void conservative_busy(struct kgsl_device *device,
	struct kgsl_pwrscale *pwrscale)
{
	device->on_time = ktime_to_us(ktime_get());
}

static void conservative_sleep(struct kgsl_device *device,
	struct kgsl_pwrscale *pwrscale)
{
  printk("%s: GPU going to sleep\n",__func__);
}

static int conservative_init(struct kgsl_device *device, struct kgsl_pwrscale *pwrscale)
{
	kgsl_pwrscale_policy_add_files(device, pwrscale, &conservative_attr_group);
	return 0;
}

static void conservative_close(struct kgsl_device *device, struct kgsl_pwrscale *pwrscale)
{
	kgsl_pwrscale_policy_remove_files(device, pwrscale, &conservative_attr_group);
}

struct kgsl_pwrscale_policy kgsl_pwrscale_policy_conservative = {
	.name = "conservative",
	.init = conservative_init,
	.busy = conservative_busy,
	.idle = conservative_idle,
	.sleep = conservative_sleep,
	.wake = conservative_wake,
	.close = conservative_close
};
EXPORT_SYMBOL(kgsl_pwrscale_policy_conservative);
