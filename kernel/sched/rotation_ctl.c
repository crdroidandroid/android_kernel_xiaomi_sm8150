/* Copyright (c) 2014-2018, 2020, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt)	"rotation_ctl: " fmt

#include "sched.h"
#include "walt.h"

struct cluster_data {
	cpumask_t cpu_mask;
	unsigned int first_cpu;
	bool inited;
};

static struct cluster_data cluster_state[MAX_CLUSTERS];
static unsigned int num_clusters;

#define for_each_cluster(cluster, idx) \
	for (; (idx) < num_clusters && ((cluster) = &cluster_state[idx]);\
		(idx)++)

static bool initialized;

static int cluster_real_big_tasks(int index, const struct sched_avg_stats *nr_stats)
{
	int nr_big = 0;
	int cpu;
	const struct cluster_data *cluster = &cluster_state[index];

	if (!index) {
		for_each_cpu(cpu, &cluster->cpu_mask)
			nr_big += nr_stats[cpu].nr_misfit;
	} else {
		for_each_cpu(cpu, &cluster->cpu_mask)
			nr_big += nr_stats[cpu].nr;
	}

	return nr_big;
}

static void update_running_avg(void)
{
	struct sched_avg_stats nr_stats[NR_CPUS];
	const struct cluster_data *cluster;
	unsigned int index = 0;
	int big_avg = 0;

	sched_get_nr_running_avg(nr_stats);

	for_each_cluster(cluster, index) {
		if (!cluster->inited)
			continue;

		big_avg += cluster_real_big_tasks(index, nr_stats);
	}

	walt_rotation_checkpoint(big_avg);
}

static u64 rotation_ctl_check_timestamp;

void rotation_ctl_check(u64 window_start)
{
	if (unlikely(!initialized))
		return;

	if (window_start == rotation_ctl_check_timestamp)
		return;

	rotation_ctl_check_timestamp = window_start;

	update_running_avg();
}

/* ============================ init code ============================== */

static const struct cluster_data *find_cluster_by_first_cpu(unsigned int first_cpu)
{
	unsigned int i;

	for (i = 0; i < num_clusters; ++i) {
		if (cluster_state[i].first_cpu == first_cpu)
			return &cluster_state[i];
	}

	return NULL;
}

static int cluster_init(const struct cpumask *mask)
{
	struct device *dev;
	unsigned int first_cpu = cpumask_first(mask);
	struct cluster_data *cluster;

	if (find_cluster_by_first_cpu(first_cpu))
		return 0;

	dev = get_cpu_device(first_cpu);
	if (!dev)
		return -ENODEV;

	pr_info("Creating CPU group %d\n", first_cpu);

	if (num_clusters == MAX_CLUSTERS) {
		pr_err("Unsupported number of clusters. Only %u supported\n",
								MAX_CLUSTERS);
		return -EINVAL;
	}
	cluster = &cluster_state[num_clusters];
	++num_clusters;

	cpumask_copy(&cluster->cpu_mask, mask);
	cluster->first_cpu = first_cpu;

	cluster->inited = true;

	return 0;
}

static int __init rotation_ctl_init(void)
{
	const struct sched_cluster *cluster;
	int ret;

	for_each_sched_cluster(cluster) {
		ret = cluster_init(&cluster->cpus);
		if (ret)
			pr_warn("unable to create rotation ctl group: %d\n", ret);
	}

	initialized = true;
	return 0;
}

late_initcall(rotation_ctl_init);
