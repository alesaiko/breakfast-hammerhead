/*
 * Header file for CPUFreq governors common code.
 *
 * Copyright (C) 2001, Russell King.
 * Copyright (C) 2003, Venkatesh Pallipadi <venkatesh.pallipadi@intel.com>
 * Copyright (C) 2003, Jun Nakajima <jun.nakajima@intel.com>
 * Copyright (C) 2009, Alexander Clouter <alex@digriz.org.uk>
 * Copyright (C) 2012, Viresh Kumar <viresh.kumar@linaro.org>
 * Copyright (C) 2017-2018, Alex Saiko <solcmdr@gmail.com>
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

#ifndef __CPUFREQ_GOVERNOR_COMMON_H__
#define __CPUFREQ_GOVERNOR_COMMON_H__

#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/kernel_stat.h>
#include <linux/module.h>
#include <linux/tick.h>
#include <linux/kthread.h>
#include <linux/input.h>
#include <linux/slab.h>

/*
 * The polling frequency of these governors depends on the capability of the
 * processor. Default polling frequency is 1000 times the transition latency
 * of the processor.
 *
 * These governors will work on any processor with transition latency <= 10ms,
 * using appropriate sampling rate. For CPUs with transition latency > 10ms
 * (mostly drivers with CPUFREQ_ETERNAL) these governors will not work.
 *
 * All times here are in us.
 */
#define MIN_SAMPLING_RATE_RATIO			(2)
#define MIN_LATENCY_MULTIPLIER			(100)
#define LATENCY_MULTIPLIER			(1000)
#define MICRO_FREQUENCY_MIN_SAMPLE_RATE		(10000)
#define TRANSITION_LATENCY_LIMIT		(10 * 1000 * 1000)

/* Macroses to implement governor sysfs nodes */
#define show_one_dbs(gov, object)					\
static ssize_t show_##object(struct kobject *kobj,			\
			     struct attribute *attr,			\
			     char *buf)					\
{									\
	return scnprintf(buf, 12, "%u\n", gov##_tuners.object);		\
}

#define store_one_dbs(gov, object, min, max)				\
static ssize_t store_##object(struct kobject *kobj,			\
			      struct attribute *attr,			\
			      const char *buf, size_t count)		\
{									\
	int ret;							\
	unsigned int val;						\
									\
	ret = kstrtouint(buf, 10, &val);				\
	if (ret || val < min || val > max)				\
		return -EINVAL;						\
									\
	gov##_tuners.object = val;					\
									\
	return count;							\
}

#define define_one_dbs_node(gov, object, min, max)			\
show_one_dbs(gov, object);						\
store_one_dbs(gov, object, min, max);					\
define_one_global_rw(object)

#define define_min_sampling_rate_node(gov)				\
static ssize_t show_sampling_rate_min(struct kobject *kobj,		\
				      struct attribute *attr,		\
				      char *buf)			\
{									\
	return scnprintf(buf, 12, "%u\n", min_sampling_rate);		\
}									\
									\
define_one_global_ro(sampling_rate_min)

#define define_sampling_rate_node(gov)					\
static ssize_t store_sampling_rate(struct kobject *kobj,		\
				   struct attribute *attr,		\
				   const char *buf, size_t count)	\
{									\
	int ret;							\
	unsigned int val;						\
									\
	ret = kstrtouint(buf, 10, &val);				\
	if (ret || val < min_sampling_rate)				\
		return -EINVAL;						\
									\
	update_sampling_rate(&gov##_dbs_data,				\
			     &gov##_tuners.sampling_rate, val);		\
									\
	return count;							\
}									\
									\
show_one_dbs(gov, sampling_rate);					\
define_one_global_rw(sampling_rate)

#define define_sampling_down_factor_node(gov)				\
static ssize_t								\
store_sampling_down_factor(struct kobject *kobj,			\
			   struct attribute *attr,			\
			   const char *buf, size_t count)		\
{									\
	struct gov##_cpu_dbs_info_s *j_dbs_info;			\
	unsigned int val;						\
	int ret, cpu;							\
									\
	ret = kstrtouint(buf, 10, &val);				\
	if (ret || val < 1)						\
		return -EINVAL;						\
									\
	gov##_tuners.sampling_down_factor = val;			\
									\
	for_each_online_cpu(cpu) {					\
		j_dbs_info = get_cpu_dbs_info_s(cpu);			\
		j_dbs_info->rate_mult = 1;				\
	}								\
									\
	return count;							\
}									\
									\
show_one_dbs(gov, sampling_down_factor);				\
define_one_global_rw(sampling_down_factor)

/* dbs helpers */
#define define_get_cpu_dbs_routines(dbs_info)				\
static inline struct cpu_dbs_common_info *get_cpu_cdbs(int cpu)		\
{									\
	return &per_cpu(dbs_info, cpu).cdbs;				\
}									\
									\
static inline void *get_cpu_dbs_info_s(int cpu)				\
{									\
	return &per_cpu(dbs_info, cpu);					\
}

#define define_dbs_timer(gov)						\
static void gov##_dbs_timer(struct work_struct *work)			\
{									\
	struct gov##_cpu_dbs_info_s *dbs_info = container_of(work,	\
			struct gov##_cpu_dbs_info_s, cdbs.work.work);	\
	int delay = align_delay(gov##_tuners.sampling_rate,		\
				dbs_info->rate_mult);			\
	int cpu = dbs_info->cdbs.cpu;					\
									\
	mutex_lock(&dbs_info->cdbs.timer_mutex);			\
	gov##_check_cpu(dbs_info);					\
									\
	queue_delayed_work_on(cpu, gov##_dbs_data.gov_wq,		\
			     &dbs_info->cdbs.work, delay);		\
	mutex_unlock(&dbs_info->cdbs.timer_mutex);			\
}

/*
 * Not all CPUs want IO time to be accounted as busy; this dependson how
 * efficient idling at a higher frequency/voltage is. Pavel Machek says this
 * is not so for various generations of AMD and old Intel systems. Mike Chan
 * (androidlcom) calis this is also not true for ARM.
 *
 * Because of this, whitelist specific known (series) of CPUs by default, and
 * leave all others up to the user.
 */
static inline int should_io_be_busy(void)
{
#ifdef CONFIG_X86
	/* Intel Core 2 (model 15) and later has an efficient idle */
	if (boot_cpu_data.x86_vendor == X86_VENDOR_INTEL &&
	    boot_cpu_data.x86 == 6 &&
	    boot_cpu_data.x86_model >= 15)
		return 1;
#endif
	/* Processors for embedded devices have an efficient idle */
	return IS_ENABLED(CONFIG_ARM);
}

/**
 * jiffy_sampling_rate() - return sampling rate ratio multiplied by 10 jiffies.
 */
static inline u32 jiffy_sampling_rate(void)
{
	return (u32)(jiffies_to_usecs(10) * MIN_SAMPLING_RATE_RATIO);
}

/**
 * nohz_idle_used() - check whether Micro/NOHZ idle accounting is used.
 */
static inline bool nohz_idle_used(void)
{
	int cpu = get_cpu();
	u64 idle_time = get_cpu_idle_time_us(cpu, NULL);
	put_cpu();

	return idle_time != -1ULL;
}

/**
 * align_delay() - align delay with online cpus and rate multiplicator.
 * @sampling_rate: delay to be aligned in ms.
 * @rate_mult: multiplicator to be used on that delay.
 */
static inline u32 align_delay(u32 sampling_rate, u32 rate_mult)
{
	int delay;

	/* We want all cpus to do sampling nearly on same jiffy */
	delay = usecs_to_jiffies(sampling_rate * rate_mult);
	if (num_online_cpus() > 1)
		delay -= jiffies % delay;

	return delay;
}

/**
 * switch_freq() - switch the frequency of a cpufreq policy to a passed value.
 * @policy: pointer to cpufreq policy.
 * @target_freq: target frequency of a policy in kHz.
 */
static inline void switch_freq(struct cpufreq_policy *policy, u32 target_freq)
{
	if (policy->cur == policy->max)
		return;

	__cpufreq_driver_target(policy, target_freq, CPUFREQ_RELATION_C);
}

/**
 * get_trans_latency() - get minimum possible transition latency of a cpu.
 * @policy: pointer to cpufreq policy.
 * @min_sampling_rate: pointer to minimum sampling rate variable.
 */
static inline u32 get_trans_latency(struct cpufreq_policy *policy,
				    u32 *min_sampling_rate)
{
	u32 latency = max_t(u32, policy->cpuinfo.transition_latency / 1000, 1);

	/* Count the lowest possible frequency transition latency */
	*min_sampling_rate = max(*min_sampling_rate, latency *
				  MIN_LATENCY_MULTIPLIER);

	/* Return an ordinary transition latency */
	return max(*min_sampling_rate, latency * LATENCY_MULTIPLIER);
}

/*
 * Abbreviations:
 * dbs: a shortform for Demand-Based Switching.
 *	It helps to keep variable names smaller, simpler.
 * cdbs: common dbs.
 * od_*: on-demand governor.
 * cs_*: conservative governor.
 */

/* Per cpu structures */
struct cpu_dbs_common_info {
	struct cpufreq_policy *cur_policy;
	struct delayed_work work;
	u64 prev_cpu_idle;
	u64 prev_cpu_wall;
	u32 prev_load;
	u32 max_load;
	int cpu;
	/*
	 * percpu mutex that serializes governor limit change with dbs_timer
	 * invocation. We do not want dbs_timer to run when user is changing
	 * the governor or limits.
	 */
	struct mutex timer_mutex;
};

/* IDs of cpufreq governor common code clients */
enum governor_id {
	GOV_ONDEMAND,
	GOV_CONSERVATIVE,
};

/* Per governor dbs data */
struct od_cpu_dbs_info_s {
	struct cpu_dbs_common_info cdbs;
	u32 rate_mult;

	struct task_struct *sync_thread;
	wait_queue_head_t sync_wq;
	atomic_t src_sync_cpu;
	atomic_t sync_enabled;
	atomic_t being_woken;
};

struct cs_cpu_dbs_info_s {
	struct cpu_dbs_common_info cdbs;
	u32 target_freq;
	u32 rate_mult;
};

/* Global sysfs tunables */
struct od_dbs_tuners {
	u32 sampling_rate;
	u32 sampling_down_factor;
	u32 up_threshold;
	u32 up_threshold_multi_core;
	u32 up_threshold_any_cpu_load;
	u32 down_differential;
	u32 down_differential_multi_core;
	u32 input_boost_freq;
	u32 optimal_freq;
	u32 sync_freq;
	u32 sync_on_migration:1;
	u32 load_scaling:1;
	u32 io_is_busy:1;
};

struct cs_dbs_tuners {
	u32 sampling_rate;
	u32 sampling_down_factor;
	u32 up_threshold;
	u32 up_threshold_burst;
	u32 up_threshold_at_low_freq;
	u32 down_threshold;
	u32 freq_up_step;
	u32 freq_down_step;
	u32 freq_cons_low;
	u32 io_is_busy:1;
};

struct dbs_data {
	/* dbs-based governor identificator */
	int governor;

	/* Pointer to governor-specific dbs tuners */
	void *tuners;

	/* Internal calls to quickly get governor data */
	struct cpu_dbs_common_info
		*(*get_cpu_cdbs)	(int cpu);
	void	*(*get_cpu_dbs_info_s)	(int cpu);

	/* Pointer to governor dbs timer work */
	void	(*gov_dbs_timer)	(struct work_struct *work);

	/* Pointer to main governor initialization code */
	int	(*init)			(struct cpu_dbs_common_info *cdbs,
					 struct cpufreq_policy *policy);

	/* Pointer to main governor exit code */
	void	(*exit)			(struct cpu_dbs_common_info *cdbs,
					 struct cpufreq_policy *policy);

	/* Pointer to the governor algorithm itself */
	void	(*od_check_cpu)		(struct od_cpu_dbs_info_s *dbs_info);
	void	(*cs_check_cpu)		(struct cs_cpu_dbs_info_s *dbs_info);

	/* Pointer to governor-specific workqueue to run one on */
	struct workqueue_struct *gov_wq;

	/* Mutex that protects governor start/stop routines */
	struct mutex mutex;
};

u32 get_policy_max_load(struct dbs_data *dbs_data,
			struct cpufreq_policy *policy,
			u32 sampling_rate, u32 io_is_busy,
			u32 *max_load_freq);
u32 get_policy_max_load_other_cpu(struct dbs_data *dbs_data,
				  struct cpufreq_policy *policy,
				  u32 optimal_freq, u32 target_load);

void update_sampling_rate(struct dbs_data *dbs_data,
			  u32 *sampling_rate, u32 new_rate);

int cpufreq_governor_dbs(struct dbs_data *dbs_data,
			 struct cpufreq_policy *policy, u32 event);

#endif /* __CPUFREQ_GOVERNOR_COMMON_H__ */
