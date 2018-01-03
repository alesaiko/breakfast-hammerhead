/*
 * Copyright (C) 2001, Russell King.
 * Copyright (C) 2003, Venkatesh Pallipadi <venkatesh.pallipadi@intel.com>
 * Copyright (C) 2003, Jun Nakajima <jun.nakajima@intel.com>
 * Copyright (C) 2009, Alexander Clouter <alex@digriz.org.uk>
 * Copyright (C) 2017-2019, Alex Saiko <solcmdr@gmail.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt)	KBUILD_MODNAME ": %s: " fmt, __func__

#include "cpufreq_governor.h"

#define DEF_FREQUENCY_UP_THRESHOLD		(80)
#define DEF_FREQUENCY_UP_THRESHOLD_BURST	(95)
#define DEF_FREQUENCY_UP_THRESHOLD_AT_LOW_FREQ	(60)
#define DEF_FREQUENCY_DOWN_THRESHOLD		(20)
#define DEF_FREQUENCY_UP_STEP			(5)
#define DEF_FREQUENCY_DOWN_STEP			(10)
#define DEF_FREQUENCY_SAMPLING_DOWN_FACTOR	(1)

enum scale_direction {
	SCALE_UP,
	SCALE_DOWN
};

static DEFINE_PER_CPU(struct cs_cpu_dbs_info_s, cs_cpu_dbs_info);
define_get_cpu_dbs_routines(cs_cpu_dbs_info);

static struct dbs_data cs_dbs_data;
static struct cs_dbs_tuners cs_tuners = {
	.sampling_down_factor	  = DEF_FREQUENCY_SAMPLING_DOWN_FACTOR,
	.up_threshold		  = DEF_FREQUENCY_UP_THRESHOLD,
	.up_threshold_burst	  = DEF_FREQUENCY_UP_THRESHOLD_BURST,
	.up_threshold_at_low_freq = DEF_FREQUENCY_UP_THRESHOLD_AT_LOW_FREQ,
	.down_threshold		  = DEF_FREQUENCY_DOWN_THRESHOLD,
	.freq_up_step		  = DEF_FREQUENCY_UP_STEP,
	.freq_down_step		  = DEF_FREQUENCY_DOWN_STEP,
};

/* Number of cpus that currently use this governor */
static unsigned int gov_enable_cnt;

/* Minimal sampling rate supported by hardware and aligned with software */
static unsigned int __read_mostly min_sampling_rate;

static inline void scale_freq(struct cs_cpu_dbs_info_s *dbs_info,
			      struct cpufreq_policy *policy,
			      enum scale_direction decrease)
{
	u32 freq_diff;

	/* This function is called in non-burst scenarios only */
	dbs_info->rate_mult = 1;

	/* Return early if there is nowhere to move */
	if (policy->cur == (decrease ? policy->min : policy->max))
		return;

	/* Calculate the difference using an appropriate step factor */
	freq_diff = policy->max * (decrease ? cs_tuners.freq_down_step :
					      cs_tuners.freq_up_step) / 100;

	/* Move target frequency and ensure it is within limits */
	dbs_info->target_freq += (decrease ? -freq_diff : freq_diff);
	dbs_info->target_freq  = clamp_t(int, dbs_info->target_freq,
					 policy->min, policy->max);
	/*
	 * Use closest frequency in case of a decrease and higher frequency
	 * otherwise to comfort both power and energy sides.
	 */
	__cpufreq_driver_target(policy, dbs_info->target_freq, decrease ?
				CPUFREQ_RELATION_C : CPUFREQ_RELATION_H);
}

static void cs_check_cpu(struct cs_cpu_dbs_info_s *dbs_info)
{
	struct cpufreq_policy *policy = dbs_info->cdbs.cur_policy;
	u32 up_threshold_burst = cs_tuners.up_threshold_burst;
	u32 up_threshold, max_load;

	max_load = get_policy_max_load(&cs_dbs_data, policy,
			cs_tuners.sampling_rate, cs_tuners.io_is_busy,
			/* max_load_freq */ NULL);

	/* Use frequency burst if an appropriate threshold is set up */
	if (up_threshold_burst && max_load >= up_threshold_burst) {
		if (policy->cur < policy->max)
			dbs_info->rate_mult = cs_tuners.sampling_down_factor;
		/*
		 * Align target frequency to a maximum one to avoid frequency
		 * drop to a very low value during the next sample.
		 */
		dbs_info->target_freq = policy->max;
		switch_freq(policy, dbs_info->target_freq);
		return;
	}

	/*
	 * Use lower frequency up threshold if current frequency is below or at
	 * the freq_cons_low corner.
	 */
	up_threshold = (policy->cur <= cs_tuners.freq_cons_low) ?
			cs_tuners.up_threshold_at_low_freq :
			cs_tuners.up_threshold;

	/* Scale current frequency using threshold values as borders */
	if (max_load >= up_threshold)
		scale_freq(dbs_info, policy, SCALE_UP);
	else if (max_load <= cs_tuners.down_threshold)
		scale_freq(dbs_info, policy, SCALE_DOWN);
}

static int cs_cpufreq_notifier(struct notifier_block *nb,
			       unsigned long val, void *data)
{
	struct cs_cpu_dbs_info_s *dbs_info;
	struct cpufreq_freqs *freq = data;
	struct cpufreq_policy *policy;

	dbs_info = get_cpu_dbs_info_s(freq->cpu);

	/*
	 * cs_exit() sets policy to NULL to stop the notifier before the
	 * unregistration of it happens.
	 */
	if (IS_ERR_OR_NULL(dbs_info->cdbs.cur_policy))
		return NOTIFY_OK;

	/*
	 * We only care if our internally tracked freq moves outside the 'valid'
	 * ranges of frequency available to us.  Otherwise, we do not change it.
	 */
	policy = dbs_info->cdbs.cur_policy;
	if (dbs_info->target_freq < policy->min ||
	    dbs_info->target_freq > policy->max)
		dbs_info->target_freq = freq->new;

	return NOTIFY_DONE;
}

static struct notifier_block cs_cpufreq_notifier_block = {
	.notifier_call = cs_cpufreq_notifier,
};

define_sampling_rate_node(cs);
define_min_sampling_rate_node(cs);
define_sampling_down_factor_node(cs);
define_one_dbs_node(cs, up_threshold, (cs_tuners.down_threshold + 1), 100);
define_one_dbs_node(cs, up_threshold_burst, 0, 100);
define_one_dbs_node(cs, up_threshold_at_low_freq, 0, 100);
define_one_dbs_node(cs, down_threshold, 0, (cs_tuners.up_threshold - 1));
define_one_dbs_node(cs, freq_up_step, 1, 100);
define_one_dbs_node(cs, freq_down_step, 1, 100);
define_one_dbs_node(cs, freq_cons_low, 0, UINT_MAX);
define_one_dbs_node(cs, io_is_busy, 0, 1);

static struct attribute *cs_attributes[] = {
	&sampling_rate_min.attr,
	&sampling_rate.attr,
	&sampling_down_factor.attr,
	&up_threshold.attr,
	&up_threshold_burst.attr,
	&up_threshold_at_low_freq.attr,
	&down_threshold.attr,
	&freq_up_step.attr,
	&freq_down_step.attr,
	&freq_cons_low.attr,
	&io_is_busy.attr,
	NULL
};

static struct attribute_group cs_attr_group = {
	.name = "conservative",
	.attrs = cs_attributes,
};

static inline int cs_init(struct cpu_dbs_common_info *cdbs,
			  struct cpufreq_policy *policy)
{
	int ret;

	if (likely(++gov_enable_cnt != 1))
		return 0;

	/* Bring kernel and HW constraints together */
	cs_tuners.sampling_rate = get_trans_latency(policy, &min_sampling_rate);

	if (likely(!cs_tuners.io_is_busy))
		cs_tuners.io_is_busy = should_io_be_busy();

	cs_tuners.freq_cons_low = clamp(cs_tuners.freq_cons_low,
					policy->min, policy->max);

	ret = cpufreq_register_notifier(&cs_cpufreq_notifier_block,
					CPUFREQ_TRANSITION_NOTIFIER);
	if (IS_ERR_VALUE(ret)) {
		pr_err("Unable to register cpufreq notifier\n");
		goto fail_notifier;
	}

	ret = sysfs_create_group(cpufreq_global_kobject, &cs_attr_group);
	if (IS_ERR_VALUE(ret)) {
		pr_err("Unable to create sysfs group\n");
		goto fail_sysfs;
	}

	return 0;

fail_sysfs:
	cpufreq_unregister_notifier(&cs_cpufreq_notifier_block,
				    CPUFREQ_TRANSITION_NOTIFIER);
fail_notifier:
	gov_enable_cnt--;

	return ret;
}

static inline void cs_exit(struct cpu_dbs_common_info *cdbs,
			   struct cpufreq_policy *policy)
{
	/* Nullify policy to stop cpufreq notifier of a cpu */
	cdbs->cur_policy = NULL;

	if (likely(--gov_enable_cnt != 0))
		return;

	sysfs_remove_group(cpufreq_global_kobject, &cs_attr_group);
	cpufreq_unregister_notifier(&cs_cpufreq_notifier_block,
				    CPUFREQ_TRANSITION_NOTIFIER);
}

define_dbs_timer(cs);
static struct dbs_data cs_dbs_data = {
	.governor		= GOV_CONSERVATIVE,
	.tuners			= &cs_tuners,
	.get_cpu_cdbs		= get_cpu_cdbs,
	.get_cpu_dbs_info_s	= get_cpu_dbs_info_s,
	.gov_dbs_timer		= cs_dbs_timer,
	.cs_check_cpu		= cs_check_cpu,
	.init			= cs_init,
	.exit			= cs_exit,
	.gov_wq			= NULL, /* Initialized in __init function */
	.mutex			= __MUTEX_INITIALIZER(cs_dbs_data.mutex),
};

static int cs_cpufreq_governor_dbs(struct cpufreq_policy *policy, u32 event)
{
	return cpufreq_governor_dbs(&cs_dbs_data, policy, event);
}

#ifndef CONFIG_CPU_FREQ_DEFAULT_GOV_CONSERVATIVE
static
#endif
struct cpufreq_governor cpufreq_gov_conservative = {
	.name			= "conservative",
	.governor		= cs_cpufreq_governor_dbs,
	.max_transition_latency	= TRANSITION_LATENCY_LIMIT,
	.owner			= THIS_MODULE,
};

static int __init cpufreq_gov_dbs_init(void)
{
	int cpu;

	/*
	 * Run governor in a separate high priority workqueue to avoid
	 * resource race with critical user-space system sections like
	 * thermal engine.
	 */
	cs_dbs_data.gov_wq = alloc_workqueue("cs_wq", WQ_HIGHPRI, 0);
	if (IS_ERR_OR_NULL(cs_dbs_data.gov_wq)) {
		pr_err("Unable to allocate high-priority workqueue\n");
		return -EFAULT;
	}

	/*
	 * Initlialize mutex during module start-up to save the resources
	 * in hotplug-sensitive governor preparation code path.
	 */
	for_each_possible_cpu(cpu) {
		struct cpu_dbs_common_info *cdbs = get_cpu_cdbs(cpu);
		mutex_init(&cdbs->timer_mutex);
	}

	/*
	 * In NOHZ/micro accounting case we set the minimum frequency
	 * not depending on HZ, but fixed (very low).  The deferred
	 * timer might skip some samples if idle/sleeping as needed.
	 */
	min_sampling_rate = nohz_idle_used() ?
		MICRO_FREQUENCY_MIN_SAMPLE_RATE : jiffy_sampling_rate();

	return cpufreq_register_governor(&cpufreq_gov_conservative);
}

static void __exit cpufreq_gov_dbs_exit(void)
{
	int cpu;

	cpufreq_unregister_governor(&cpufreq_gov_conservative);

	for_each_possible_cpu(cpu) {
		struct cpu_dbs_common_info *cdbs = get_cpu_cdbs(cpu);
		mutex_destroy(&cdbs->timer_mutex);
	}

	destroy_workqueue(cs_dbs_data.gov_wq);
}

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_CONSERVATIVE
fs_initcall(cpufreq_gov_dbs_init);
#else
module_init(cpufreq_gov_dbs_init);
#endif
module_exit(cpufreq_gov_dbs_exit);

MODULE_AUTHOR("Alexander Clouter <alex@digriz.org.uk>");
MODULE_DESCRIPTION("'cpufreq_conservative' - A dynamic cpufreq governor for "
		   "Low Latency Frequency Transition capable processors "
		   "optimised for use in a battery environment");
MODULE_LICENSE("GPL v2");
