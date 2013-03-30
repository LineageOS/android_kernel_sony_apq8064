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

struct kgsl_conservative_attribute {
    struct attribute attr;
    ssize_t (*show)(struct kgsl_device *device, char *buf);
    ssize_t (*store)(struct kgsl_device *device, const char *buf,
             size_t count);
};

#define to_pwrscale(k) container_of(k, struct kgsl_pwrscale, kobj)
#define pwrscale_to_device(p) container_of(p, struct kgsl_device, pwrscale)
#define to_device(k) container_of(k, struct kgsl_device, pwrscale_kobj)
#define to_pwrscale_attr(a) \
container_of(a, struct kgsl_conservative_attribute, attr)
#define to_policy_attr(a) \
container_of(a, struct kgsl_pwrscale_policy_attribute, attr)

#define CONSERVATIVE_ATTR(_name, _mode, _show, _store) \
struct kgsl_conservative_attribute conservative_attr_##_name = \
__ATTR(_name, _mode, _show, _store)

#define MIN_POLL_INTERVAL 10
#define MAX_POLL_INTERVAL 1000
static unsigned long g_polling_interval = 100;

static unsigned long walltime_total = 0;
static unsigned long busytime_total = 0;

struct gpu_thresh_tbl{
    const unsigned int up_threshold;
    const unsigned int down_threshold;
};

#define GPU_SCALE(u,d) \
    { \
        .up_threshold = u, \
        .down_threshold = d, \
    }

static struct gpu_thresh_tbl thresh_tbl[] = {
    GPU_SCALE(110,60),
    GPU_SCALE(90,45),
    GPU_SCALE(80,45),
    GPU_SCALE(50,0),
    GPU_SCALE(100,0),
};

static void conservative_wake(struct kgsl_device *device, struct kgsl_pwrscale *pwrscale)
{
    struct kgsl_power_stats stats;

    pr_info("%s: GPU waking up\n",__func__);

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
    if (walltime_total > (g_polling_interval * 1000))
    {
        pr_info("%s: walltime_total = %lu, busytime_total = %lu\n",__func__,walltime_total,busytime_total);

        loadpct = (100 * busytime_total) / walltime_total;
        pr_info("%s: loadpct = %d\n",__func__,loadpct);

        walltime_total = busytime_total = 0;

        if(loadpct < thresh_tbl[pwr->active_pwrlevel].down_threshold)
            val = 1;
        else if(loadpct > thresh_tbl[pwr->active_pwrlevel].up_threshold)
            val = -1;

        pr_info("%s: active_pwrlevel = %d, change = %d\n",__func__,pwr->active_pwrlevel,val);

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
    pr_info("%s: GPU going to sleep\n",__func__);
}

static ssize_t conservative_polling_interval_show(
                    struct kgsl_device *device, 
                    char *buf)
{
    int count;

    count = sprintf(buf, "%lu\n", g_polling_interval);
    pr_info("%s: polling interval = %lu\n", __func__, g_polling_interval);

    return count;
}

static ssize_t conservative_polling_interval_store(
                    struct kgsl_device *device, 
                    const char *buf,
                    size_t count)
{
    unsigned long tmp;
    int err;

    err = kstrtoul(buf, 0, &tmp);
    if (err) {
        pr_err("%s: failed setting new polling interval!\n", __func__);
        return err;
    }

    if (tmp < MIN_POLL_INTERVAL)
        tmp = MIN_POLL_INTERVAL;
    else if (tmp > MAX_POLL_INTERVAL)
        tmp = MAX_POLL_INTERVAL;

    g_polling_interval = tmp;

    pr_info("%s: new polling interval = %lu\n", __func__, g_polling_interval);

    return count;
}

CONSERVATIVE_ATTR(polling_interval, 0664, conservative_polling_interval_show, conservative_polling_interval_store);

static struct attribute *conservative_attrs[] = {
    &conservative_attr_polling_interval.attr,
    NULL
};

static struct attribute_group conservative_attr_group = {
    .name = "attrs",
    .attrs = conservative_attrs,
};

static int conservative_init(struct kgsl_device *device, struct kgsl_pwrscale *pwrscale)
{
    int ret;

    ret = kgsl_pwrscale_policy_add_files(device, pwrscale, &conservative_attr_group);
    
    if (ret)
        pr_err("%s: failed adding conservative attribute group!\n", __func__);
    else
        pr_info("%s: added conservative attribute group.\n", __func__);

    return ret;
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
