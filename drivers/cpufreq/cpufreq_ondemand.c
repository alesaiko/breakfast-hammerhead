/*
 * Copyright (C) 2001, Russell King.
 * Copyright (C) 2003, Venkatesh Pallipadi <venkatesh.pallipadi@intel.com>
 * Copyright (C) 2003, Jun Nakajima <jun.nakajima@intel.com>
 * Copyright (C) 2013, The Linux Foundation. All rights reserved.
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
#define DEF_FREQUENCY_DOWN_DIFFERENTIAL		(10)
#define DEF_FREQUENCY_SAMPLING_DOWN_FACTOR	(1)
#define DEF_FREQUENCY_SYNCHRONIZATION		(1)
#define DEF_FREQUENCY_LOAD_DEPENDENT_SCALING	(0)

#define MICRO_FREQUENCY_UP_THRESHOLD		(95)
#define MICRO_FREQUENCY_DOWN_DIFFERENTIAL	(3)

struct dbs_work_struct {
	struct work_struct work;
	u32 cpu;
};

static DEFINE_PER_CPU(struct dbs_work_struct, dbs_refresh_work);
static DEFINE_PER_CPU(struct od_cpu_dbs_info_s, od_cpu_dbs_info);
define_get_cpu_dbs_routines(od_cpu_dbs_info);

static struct dbs_data od_dbs_data;
static struct od_dbs_tuners od_tuners = {
	.sampling_down_factor		= DEF_FREQUENCY_SAMPLING_DOWN_FACTOR,
	.up_threshold			= DEF_FREQUENCY_UP_THRESHOLD,
	.up_threshold_multi_core	= DEF_FREQUENCY_UP_THRESHOLD,
	.up_threshold_any_cpu_load	= DEF_FREQUENCY_UP_THRESHOLD,
	.down_differential		= DEF_FREQUENCY_DOWN_DIFFERENTIAL,
	.down_differential_multi_core	= MICRO_FREQUENCY_DOWN_DIFFERENTIAL,
	.sync_on_migration		= DEF_FREQUENCY_SYNCHRONIZATION,
	.load_scaling			= DEF_FREQUENCY_LOAD_DEPENDENT_SCALING,
};

/* Number of cpus that currently use this governor */
static unsigned int gov_enable_cnt;

/* Minimal sampling rate supported by hardware and aligned with software */
static unsigned int __read_mostly min_sampling_rate;

static void od_check_cpu(struct od_cpu_dbs_info_s *dbs_info)
{
	struct cpufreq_policy *policy = dbs_info->cdbs.cur_policy;
	u32 max_load = 0, max_load_freq, max_load_other_cpu, freq_next;
	u32 min_f = policy->cpuinfo.min_freq, max_f = policy->cpuinfo.max_freq;

	/* Get all 'load' values first */
	max_load = get_policy_max_load(&od_dbs_data, policy,
		od_tuners.sampling_rate, od_tuners.io_is_busy, &max_load_freq);
	max_load_other_cpu = get_policy_max_load_other_cpu(&od_dbs_data, policy,
		od_tuners.optimal_freq, od_tuners.up_threshold_any_cpu_load);

	/* Switch to load dependent algorithm early if specified */
	if (od_tuners.load_scaling)
		goto load_dependent_algorithm;

	/* Immediately burst frequency if averaged 'load' is above threshold */
	if (max_load_freq >= od_tuners.up_threshold * policy->cur) {
		/* Apply sampling down factor to smoothly slow down samples */
		if (policy->cur < policy->max)
			dbs_info->rate_mult = od_tuners.sampling_down_factor;

		switch_freq(policy, policy->max);
		return;
	}

	/* Align frequency if there are some other cpus online right now */
	if (num_online_cpus() > 1) {
		if (max_load_other_cpu > od_tuners.up_threshold_any_cpu_load) {
			if (policy->cur < od_tuners.sync_freq)
				switch_freq(policy, od_tuners.sync_freq);
			return;
		}

		if (max_load_freq >=
		    od_tuners.up_threshold_multi_core * policy->cur) {
			if (policy->cur < od_tuners.optimal_freq)
				switch_freq(policy, od_tuners.optimal_freq);
			return;
		}
	}

	/* Return early if there is already nowhere to move */
	if (policy->cur == policy->min)
		return;

	/*
	 * Try to slow down only if an averaged 'load' value is less than the
	 * difference between up_threshold and down_differential. This way
	 * we can achieve a bit more performance under medium load.
	 */
	if (max_load_freq <=
	   (od_tuners.up_threshold - od_tuners.down_differential) *
	    policy->cur) {
		dbs_info->rate_mult = 1;

		/* Safely count target frequency based on load frequency */
		freq_next = max(max_load_freq / (od_tuners.up_threshold -
				od_tuners.down_differential), policy->min);

		/* Align next frequency with online cpus */
		if (num_online_cpus() > 1) {
			if (max_load_other_cpu >=
			   (od_tuners.up_threshold_multi_core -
			    od_tuners.down_differential) &&
			    freq_next < od_tuners.sync_freq)
				freq_next = od_tuners.sync_freq;

			if (max_load_freq >=
			   (od_tuners.up_threshold_multi_core -
			    od_tuners.down_differential_multi_core) *
			    policy->cur && freq_next < od_tuners.optimal_freq)
				freq_next = od_tuners.optimal_freq;
		}

		/* Use closest frequency selection here */
		__cpufreq_driver_target(policy, freq_next, CPUFREQ_RELATION_C);
	}

	return;

load_dependent_algorithm:
	/* The same as in above algorithm */
	if (max_load >= od_tuners.up_threshold) {
		/* Apply sampling down factor to slow down sampling rate */
		if (policy->cur < policy->max)
			dbs_info->rate_mult = od_tuners.sampling_down_factor;

		/* Move to maximum frequency instantly */
		switch_freq(policy, policy->max);
	} else {
		dbs_info->rate_mult = 1;

		/* Scale the frequency using a current load as a multiplier */
		freq_next = min_f + max_load * (max_f - min_f) / 100;
		__cpufreq_driver_target(policy, freq_next, CPUFREQ_RELATION_C);
	}
}

static int od_migration_notify(struct notifier_block *nb,
			       unsigned long target_cpu, void *arg)
{
	int src_cpu, dest_cpu;
	struct od_cpu_dbs_info_s *dbs_info;
#ifdef CONFIG_SCHED_FREQ_INPUT
	struct migration_notify_data *mnd = arg;

	/* Window-stats use Migration Notify Data instead of target_cpu */
	src_cpu = mnd->src_cpu;
	dest_cpu = mnd->dest_cpu;
#else
	src_cpu = (int)arg;
	dest_cpu = (int)target_cpu;
#endif
	/* Return early if synchronization is disabled */
	if (!od_tuners.sync_on_migration)
		return NOTIFY_OK;

	dbs_info = get_cpu_dbs_info_s(dest_cpu);
	/* Assign source cpu id to an atomic value */
	atomic_set(&dbs_info->src_sync_cpu, src_cpu);

	/*
	 * Avoid issuing recursive wakeup call, as sync thread itself could be
	 * seen as migrating triggering this notification. Note that sync thread
	 * of a cpu could be running for a short while with its affinity broken
	 * because of CPU hotplug.
	 */
	if (likely(!atomic_cmpxchg(&dbs_info->being_woken, 0, 1))) {
		wake_up(&dbs_info->sync_wq);
		atomic_set(&dbs_info->being_woken, 0);
	}

	return NOTIFY_DONE;
}

static struct notifier_block od_migration_nb = {
	.notifier_call = od_migration_notify,
};

static inline bool sync_pending(struct od_cpu_dbs_info_s *dbs_info)
{
	return atomic_read(&dbs_info->src_sync_cpu) >= 0;
}

static int od_sync_thread(void *data)
{
	int src_cpu, cpu = (int)data, delay;
	struct cpu_dbs_common_info *dest_cdbs = get_cpu_cdbs(cpu);
	struct od_cpu_dbs_info_s *dest_dbs_info = get_cpu_dbs_info_s(cpu);
	struct od_cpu_dbs_info_s *src_dbs_info;
	struct cpufreq_policy *policy;
	u32 src_freq, src_max_load;

	while (true) {
		wait_event(dest_dbs_info->sync_wq,
			   sync_pending(dest_dbs_info) ||
			   kthread_should_stop());

		if (kthread_should_stop())
			break;

		get_online_cpus();
		/* Return early if synchronization is disabled */
		if (!atomic_read(&dest_dbs_info->sync_enabled)) {
			atomic_set(&dest_dbs_info->src_sync_cpu, -1);
			put_online_cpus();
			continue;
		}

		/* Try to load dbs data of a source cpu */
		src_cpu = atomic_read(&dest_dbs_info->src_sync_cpu);
		src_dbs_info = get_cpu_dbs_info_s(src_cpu);

		/*
		 * Try to get a current frequency of a source cpu
		 * and snapshot its 'load' value if available.
		 */
		if (src_dbs_info && src_dbs_info->cdbs.cur_policy) {
			src_freq = src_dbs_info->cdbs.cur_policy->cur;
			src_max_load = src_dbs_info->cdbs.max_load;
		} else {
			src_freq = od_tuners.sync_freq;
			src_max_load = 0;
		}

		if (IS_ERR_VALUE(lock_policy_rwsem_write(cpu)))
			goto bail_acq_sema_failed;

		/* Load policy of a destination cpu */
		policy = dest_dbs_info->cdbs.cur_policy;
		if (IS_ERR_OR_NULL(policy))
			goto bail_incorrect_governor;

		/* Use a delay value without sampling_down_factor */
		delay = usecs_to_jiffies(od_tuners.sampling_rate);

		/*
		 * Try to synchronize a current frequency between
		 * a source cpu and a destination one.
		 */
		if (policy->cur < src_freq) {
			cancel_delayed_work_sync(&dest_dbs_info->cdbs.work);
			/*
			 * Arch specific cpufreq driver may fail.
			 * Don't update governor frequency upon failure.
			 */
			if (__cpufreq_driver_target(policy, src_freq,
			    CPUFREQ_RELATION_C) >= 0) {
				policy->cur = src_freq;
				/*
				 * Update load values of a destination cpu to
				 * affect the results of load computation.
				 */
				if (src_max_load > dest_cdbs->max_load) {
					dest_cdbs->max_load = src_max_load;
					dest_cdbs->prev_load = src_max_load;
				}
			}

			mutex_lock(&dest_dbs_info->cdbs.timer_mutex);
			/* Put a new sample work onto a global workqueue */
			schedule_delayed_work_on(cpu,
				&dest_dbs_info->cdbs.work, delay);
			mutex_unlock(&dest_dbs_info->cdbs.timer_mutex);
		}

bail_incorrect_governor:
		unlock_policy_rwsem_write(cpu);
bail_acq_sema_failed:
		put_online_cpus();
		atomic_set(&dest_dbs_info->src_sync_cpu, -1);
	}

	return 0;
}

static void od_input_boost(struct work_struct *work)
{
	struct dbs_work_struct *dbs_work =
		container_of(work, struct dbs_work_struct, work);
	struct cpu_dbs_common_info *cdbs;
	struct cpufreq_policy *policy;
	u32 cpu = dbs_work->cpu, target_freq;

	get_online_cpus();
	if (IS_ERR_VALUE(lock_policy_rwsem_write(cpu)))
		goto bail_acq_sema_failed;

	cdbs = get_cpu_cdbs(cpu);
	/*
	 * od_exit() sets policy to NULL to disable input boosting before the
	 * unregistration of input handler happens.
	 */
	policy = cdbs->cur_policy;
	if (IS_ERR_OR_NULL(policy))
		goto bail_incorrect_governor;

	/* Do not boost over current maximum frequency of a policy */
	target_freq = min(od_tuners.input_boost_freq, policy->max);
	if (policy->cur < target_freq) {
		/*
		 * Arch specific cpufreq driver may fail.
		 * Don't update governor frequency upon failure.
		 */
		if (__cpufreq_driver_target(policy, target_freq,
		    CPUFREQ_RELATION_C) >= 0)
			policy->cur = target_freq;

		/* Update time slice as it is expected to change after boost */
		cdbs->prev_cpu_idle = get_cpu_idle_time(cpu,
			&cdbs->prev_cpu_wall, od_tuners.io_is_busy);
	}

bail_incorrect_governor:
	unlock_policy_rwsem_write(cpu);
bail_acq_sema_failed:
	put_online_cpus();
}

static void od_input_event(struct input_handle *handle,
			   unsigned int type, unsigned int code,
			   int value)
{
	int i;

	/* Return early if input boosting is disabled */
	if (!od_tuners.input_boost_freq)
		return;

	/* Boost all online cpus on input event */
	for_each_online_cpu(i)
		queue_work_on(i, od_dbs_data.gov_wq,
			      &per_cpu(dbs_refresh_work, i).work);
}

static int od_input_connect(struct input_handler *handler,
			    struct input_dev *dev,
			    const struct input_device_id *id)
{
	struct input_handle *handle;
	int error;

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (IS_ERR_OR_NULL(handle))
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "cpufreq";

	error = input_register_handle(handle);
	if (IS_ERR_VALUE(error))
		goto err2;

	error = input_open_device(handle);
	if (IS_ERR_VALUE(error))
		goto err1;

	return 0;

err1:
	input_unregister_handle(handle);
err2:
	kfree(handle);

	return error;
}

static void od_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id od_ids[] = {
	/* React to all input events */
	{ .driver_info = 1 },
	{ }
};

static struct input_handler od_input_handler = {
	.name		= "cpufreq_ond",
	.event		= od_input_event,
	.connect	= od_input_connect,
	.disconnect	= od_input_disconnect,
	.id_table	= od_ids,
};

define_sampling_rate_node(od);
define_min_sampling_rate_node(od);
define_sampling_down_factor_node(od);
define_one_dbs_node(od, up_threshold, (od_tuners.down_differential + 1), 100);
define_one_dbs_node(od, up_threshold_multi_core,
		   (od_tuners.down_differential_multi_core + 1), 100);
define_one_dbs_node(od, up_threshold_any_cpu_load, 1, 100);
define_one_dbs_node(od, down_differential, 0, (od_tuners.up_threshold - 1));
define_one_dbs_node(od, down_differential_multi_core, 0,
		   (od_tuners.up_threshold_multi_core - 1));
define_one_dbs_node(od, input_boost_freq, 0, UINT_MAX);
define_one_dbs_node(od, optimal_freq, 0, UINT_MAX);
define_one_dbs_node(od, sync_freq, 0, UINT_MAX);
define_one_dbs_node(od, sync_on_migration, 0, 1);
define_one_dbs_node(od, load_scaling, 0, 1);
define_one_dbs_node(od, io_is_busy, 0, 1);

static struct attribute *od_attributes[] = {
	&sampling_rate_min.attr,
	&sampling_rate.attr,
	&sampling_down_factor.attr,
	&up_threshold.attr,
	&up_threshold_multi_core.attr,
	&up_threshold_any_cpu_load.attr,
	&down_differential.attr,
	&down_differential_multi_core.attr,
	&input_boost_freq.attr,
	&optimal_freq.attr,
	&sync_freq.attr,
	&sync_on_migration.attr,
	&load_scaling.attr,
	&io_is_busy.attr,
	NULL
};

static struct attribute_group od_attr_group = {
	.name = "ondemand",
	.attrs = od_attributes,
};

static inline int od_init(struct cpu_dbs_common_info *cdbs,
			  struct cpufreq_policy *policy)
{
	int ret;

	if (likely(++gov_enable_cnt != 1))
		return 0;

	/* Bring kernel and HW constraints together */
	od_tuners.sampling_rate = get_trans_latency(policy, &min_sampling_rate);

	if (likely(!od_tuners.io_is_busy))
		od_tuners.io_is_busy = should_io_be_busy();
	if (likely(!od_tuners.input_boost_freq))
		od_tuners.input_boost_freq = policy->max;

	od_tuners.optimal_freq =
		clamp(od_tuners.optimal_freq, policy->min, policy->max);
	od_tuners.sync_freq =
		clamp(od_tuners.sync_freq, policy->min, policy->max);

	ret = atomic_notifier_chain_register(&migration_notifier_head,
					     &od_migration_nb);
	if (IS_ERR_VALUE(ret)) {
		pr_err("Unable to register atomic notifier\n");
		goto fail_notifier;
	}

	ret = input_register_handler(&od_input_handler);
	if (IS_ERR_VALUE(ret)) {
		pr_err("Unable to register input handler\n");
		goto fail_input;
	}

	ret = sysfs_create_group(cpufreq_global_kobject, &od_attr_group);
	if (IS_ERR_VALUE(ret)) {
		pr_err("Unable to create sysfs group\n");
		goto fail_sysfs;
	}

	return 0;

fail_sysfs:
	input_unregister_handler(&od_input_handler);
fail_input:
	atomic_notifier_chain_unregister(&migration_notifier_head,
					 &od_migration_nb);
fail_notifier:
	gov_enable_cnt--;

	return ret;
}

static inline void od_exit(struct cpu_dbs_common_info *cdbs,
			   struct cpufreq_policy *policy)
{
	int cpu;

	/*
	 * Stop frequency synchronization in a whole policy as one of the
	 * cpus in this policy is going to leave ondemand service.
	 */
	for_each_cpu(cpu, policy->cpus) {
		struct od_cpu_dbs_info_s *j_dbs_info = get_cpu_dbs_info_s(cpu);
		atomic_set(&j_dbs_info->sync_enabled, 0);
	}

	/* Nullify cpufreq policy to stop input handler first */
	cdbs->cur_policy = NULL;

	if (likely(--gov_enable_cnt != 0))
		return;

	sysfs_remove_group(cpufreq_global_kobject, &od_attr_group);
	input_unregister_handler(&od_input_handler);
	atomic_notifier_chain_unregister(&migration_notifier_head,
					 &od_migration_nb);
}

define_dbs_timer(od);
static struct dbs_data od_dbs_data = {
	.governor		= GOV_ONDEMAND,
	.tuners			= &od_tuners,
	.get_cpu_cdbs		= get_cpu_cdbs,
	.get_cpu_dbs_info_s	= get_cpu_dbs_info_s,
	.gov_dbs_timer		= od_dbs_timer,
	.od_check_cpu		= od_check_cpu,
	.init			= od_init,
	.exit			= od_exit,
	.gov_wq			= NULL, /* Initialized in __init function */
	.mutex			= __MUTEX_INITIALIZER(od_dbs_data.mutex),
};

static int od_cpufreq_governor_dbs(struct cpufreq_policy *policy, u32 event)
{
	return cpufreq_governor_dbs(&od_dbs_data, policy, event);
}

#ifndef CONFIG_CPU_FREQ_DEFAULT_GOV_ONDEMAND
static
#endif
struct cpufreq_governor cpufreq_gov_ondemand = {
	.name			= "ondemand",
	.governor		= od_cpufreq_governor_dbs,
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
	od_dbs_data.gov_wq = alloc_workqueue("od_wq", WQ_HIGHPRI, 0);
	if (IS_ERR_OR_NULL(od_dbs_data.gov_wq)) {
		pr_err("Unable to allocate high-priority workqueue\n");
		return -EFAULT;
	}

	for_each_possible_cpu(cpu) {
		struct od_cpu_dbs_info_s *dbs_info = get_cpu_dbs_info_s(cpu);
		struct dbs_work_struct *dbs_work =
				&per_cpu(dbs_refresh_work, cpu);

		dbs_work->cpu = cpu;
		INIT_WORK(&dbs_work->work, od_input_boost);

		mutex_init(&dbs_info->cdbs.timer_mutex);

		atomic_set(&dbs_info->src_sync_cpu, -1);
		atomic_set(&dbs_info->being_woken, 0);
		init_waitqueue_head(&dbs_info->sync_wq);

		dbs_info->sync_thread = kthread_run(od_sync_thread,
					(void *)cpu, "dbs_sync/%d", cpu);
	}

	/*
	 * In NOHZ/micro accounting case we set the minimum frequency
	 * not depending on HZ, but fixed (very low).  The deferred
	 * timer might skip some samples if idle/sleeping as needed.
	 */
	if (nohz_idle_used()) {
		od_tuners.up_threshold = MICRO_FREQUENCY_UP_THRESHOLD;
		od_tuners.down_differential = MICRO_FREQUENCY_DOWN_DIFFERENTIAL;
		od_tuners.down_differential_multi_core =
					      MICRO_FREQUENCY_DOWN_DIFFERENTIAL;
		min_sampling_rate = MICRO_FREQUENCY_MIN_SAMPLE_RATE;
	} else {
		min_sampling_rate = jiffy_sampling_rate();
	}

	return cpufreq_register_governor(&cpufreq_gov_ondemand);
}

static void __exit cpufreq_gov_dbs_exit(void)
{
	int cpu;

	cpufreq_unregister_governor(&cpufreq_gov_ondemand);

	for_each_possible_cpu(cpu) {
		struct od_cpu_dbs_info_s *dbs_info = get_cpu_dbs_info_s(cpu);
		mutex_destroy(&dbs_info->cdbs.timer_mutex);
		kthread_stop(dbs_info->sync_thread);
	}

	destroy_workqueue(od_dbs_data.gov_wq);
}

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_ONDEMAND
fs_initcall(cpufreq_gov_dbs_init);
#else
module_init(cpufreq_gov_dbs_init);
#endif
module_exit(cpufreq_gov_dbs_exit);

MODULE_AUTHOR("Venkatesh Pallipadi <venkatesh.pallipadi@intel.com>");
MODULE_AUTHOR("Alexey Starikovskiy <alexey.y.starikovskiy@intel.com>");
MODULE_DESCRIPTION("'cpufreq_ondemand' - A dynamic cpufreq governor for "
		   "Low Latency Frequency Transition capable processors");
MODULE_LICENSE("GPL v2");
