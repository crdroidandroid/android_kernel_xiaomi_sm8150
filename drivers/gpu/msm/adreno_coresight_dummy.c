/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/platform_device.h>
#include <linux/coresight.h>

#include "adreno.h"
#define TO_ADRENO_CORESIGHT_ATTR(_attr) \
	container_of(_attr, struct adreno_coresight_attr, attr)

ssize_t adreno_coresight_show_register(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return 0;
}

ssize_t adreno_coresight_store_register(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
    return 0;
}

static void adreno_coresight_disable(struct coresight_device *csdev,
					struct perf_event *event)
{
    return;
}

static int adreno_coresight_enable(struct coresight_device *csdev,
				struct perf_event *event, u32 mode)
{
    return 0;
}

void adreno_coresight_stop(struct adreno_device *adreno_dev)
{
    return;
}

void adreno_coresight_start(struct adreno_device *adreno_dev)
{
    return;
}

static int adreno_coresight_trace_id(struct coresight_device *csdev)
{
    return 0;
}

static const struct coresight_ops_source adreno_coresight_source_ops = {
	.trace_id = adreno_coresight_trace_id,
	.enable = adreno_coresight_enable,
	.disable = adreno_coresight_disable,
};

static const struct coresight_ops adreno_coresight_ops = {
	.source_ops = &adreno_coresight_source_ops,
};

void adreno_coresight_remove(struct adreno_device *adreno_dev)
{
    return;
}

int adreno_coresight_init(struct adreno_device *adreno_dev)
{
    return 0;
}
