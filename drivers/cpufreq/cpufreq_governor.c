/*
 * Source file for CPUFreq governors common code.
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

#include "cpufreq_governor.h"

/**
 * get_policy_max_load() - get maximum load across all cpus in a policy.
 * @dbs_data: pointer to governor dbs data.
 * @policy: pointer to cpufreq policy.
 * @sampling_rate: time in usecs to be used in load burst scenario.
 * @io_is_busy: flag to determine whether to subtract iowait time from idle one.
 * @max_load_freq: pointer to maximum load freq variable to store averaged load.
 *
 * Returns maximum 'load' calculated with help of kernel cpu times across all
 * cpus in a cpufreq policy. max_load_freq pointer is used to store that load
 * multiplied by average cpu frequency.  This is useful for governors like
 * ondemand where it can be used in linear scaling implementation, etc.
 *
 * ! This function must be called with get_cpu_cdbs() call filled in dbs_data.
 */
u32 get_policy_max_load(struct dbs_data *dbs_data,
			struct cpufreq_policy *policy,
			u32 sampling_rate, u32 io_is_busy,
			u32 *max_load_freq)
{
	struct cpu_dbs_common_info *j_cdbs;
	u32 _max_load_freq = 0, max_load = 0, load_at_max_freq;
	int cpu;

	/* Iterate through all cpus in a passed policy */
	for_each_cpu(cpu, policy->cpus) {
		u32 idle_time, wall_time, cur_load, load_freq;
		u64 cur_idle_time, cur_wall_time;

		/* Some targets want iowait time to be subtracted from idle */
		cur_idle_time = get_cpu_idle_time(cpu, &cur_wall_time,
						  io_is_busy);

		/* Update time slices */
		j_cdbs = dbs_data->get_cpu_cdbs(cpu);
		idle_time = (u32)(cur_idle_time - j_cdbs->prev_cpu_idle);
		j_cdbs->prev_cpu_idle = cur_idle_time;

		wall_time = (u32)(cur_wall_time - j_cdbs->prev_cpu_wall);
		j_cdbs->prev_cpu_wall = cur_wall_time;

		/* Safely count 'load' value */
		if (unlikely(!wall_time)) {
			/*
			 * That can only happen when this function is called
			 * twice in a row with a very short interval between
			 * the calls, so the previous load value can be used
			 * then.
			 */
			cur_load = j_cdbs->prev_load;
		} else if (unlikely(wall_time < idle_time)) {
			/*
			 * That can happen if idle_time is returned by
			 * get_cpu_idle_time_jiffy(). In that case idle_time
			 * is roughly equal to the difference between wall_time
			 * and "busy time" obtained from CPU statistics. Then,
			 * the "busy time" can end up being greater than
			 * wall_time (for example, if jiffies_64 and the CPU
			 * statistics are updated by different CPUs), so
			 * idle_time may in fact be negative. That means,
			 * though, that the CPU was busy all the time (on the
			 * rough average) during the last sampling interval and
			 * 100 can be returned as the load.
			 */
			cur_load = (int)idle_time < 0 ? 100 : 0;
		} else {
			/*
			 * All the required data is valid. We can calculate
			 * current load of a CPU in an ordinary way without
			 * worrying about a theoretical failure.
			 */
			cur_load = 100 * (wall_time - idle_time) / wall_time;
		}

		/* Load burst logic */
		if (unlikely(wall_time > (sampling_rate * 2) &&
		    cur_load < j_cdbs->prev_load)) {
			/*
			 * If the CPU had gone completely idle and a task has
			 * just woken up on this CPU now, it would be unfair to
			 * calculate 'load' the usual way for this elapsed
			 * time-window, because it would show near-zero load,
			 * irrespective of how CPU intensive that task actually
			 * was. This is undesirable for latency-sensitive bursty
			 * workloads.
			 *
			 * To avoid this, reuse the 'load' from the previous
			 * time-window and give this task a chance to start with
			 * a reasonably high CPU frequency.  However, that
			 * shouldn't be over-done, lest we get stuck at a high
			 * load (high frequency) for too long, even when the
			 * current system load has actually dropped down, so
			 * clear prev_load to guarantee that the load will be
			 * computed again next time.
			 *
			 * Detecting this situation is easy: the governor's
			 * utilization update handler would not have run during
			 * CPU-idle periods.  Hence, an unusually large
			 * 'wall_time' (as compared to the sampling rate)
			 * indicates this scenario.
			 */
			cur_load = j_cdbs->prev_load;
			j_cdbs->prev_load = 0;
		} else {
			/*
			 * Save the current 'load' value to keep a reliable
			 * value for load-burst scenario, where previous 'load'
			 * will be used to align the real 'load' after a long
			 * idle period.
			 */
			j_cdbs->prev_load = cur_load;
		}

		/* Count a maximum 'load' value across all cpus in a policy */
		max_load = max(max_load, cur_load);

		/* Store maximum load of a cpu in a per-cpu variable */
		j_cdbs->max_load = max(cur_load, j_cdbs->prev_load);

		/* Some governors require averaged frequency-multiplied value */
		if (max_load_freq) {
			int freq_avg = __cpufreq_driver_getavg(policy, cpu);
			if (freq_avg <= 0)
				freq_avg = policy->cur;

			load_freq = cur_load * freq_avg;
			_max_load_freq = max(_max_load_freq, load_freq);
		}
	}

	/* Use max_load_freq to provide real load_at_max if specified */
	load_at_max_freq = (max_load_freq ? _max_load_freq :
			   (max_load * policy->cur)) / policy->max;

	/* Report the policy utilization to userspace */
	cpufreq_notify_utilization(policy, load_at_max_freq);

	/* Store averaged maximum load if specified */
	if (max_load_freq)
		*max_load_freq = _max_load_freq;

	return max_load;
}
EXPORT_SYMBOL_GPL(get_policy_max_load);

/**
 * get_policy_max_load_other_cpu() - get maximum load across other online cpus.
 * @dbs_data: pointer to governor dbs data.
 * @policy: pointer to cpufreq policy.
 * @optimal_freq: frequency cap to consider other cpu as loaded.
 * @target_load: emulated load of a running cpu.
 *
 * Returns maximum 'load' value of every cpu except the caller. Originating
 * 'loads' should be calculated by get_policy_max_load() call first.
 *
 * ! This function must be called with get_cpu_cdbs() call filled in dbs_data.
 */
u32 get_policy_max_load_other_cpu(struct dbs_data *dbs_data,
				  struct cpufreq_policy *policy,
				  u32 optimal_freq, u32 target_load)
{
	struct cpu_dbs_common_info *j_cdbs;
	u32 max_load_other_cpu = 0;
	int cpu;

	/* Iterate through online cpus only */
	for_each_online_cpu(cpu) {
		/* We want to get maximum load across other cpus */
		if (cpu == policy->cpu)
			continue;

		j_cdbs = dbs_data->get_cpu_cdbs(cpu);
		max_load_other_cpu = max(max_load_other_cpu, j_cdbs->max_load);

		/*
		 * The other CPU could be running at higher frequency,
		 * but may not have completed its sampling_down_factor.
		 * For that case consider other CPU is loaded so that
		 * frequency imbalance does not occur.
		 */
		if (j_cdbs->cur_policy && j_cdbs->cur_policy->cur ==
		    j_cdbs->cur_policy->max && policy->cur >= optimal_freq)
			max_load_other_cpu =
				max(max_load_other_cpu, target_load);
	}

	return max_load_other_cpu;
}
EXPORT_SYMBOL_GPL(get_policy_max_load_other_cpu);

/**
 * update_sampling_rate() - update sampling rate effective immediately.
 * @dbs_data: pointer to governor dbs data.
 * @sampling_rate: pointer to sampling rate value to change.
 * @new_rate: new sampling rate.
 *
 * If new sampling rate is smaller than the old, simply updaing sampling_rate
 * might not be appropriate. For example, if the original sampling_rate was 1
 * second and the requested new sampling rate is 10 ms because the user needs
 * immediate reaction from ondemand governor, but not sure if higher frequency
 * will be required or not, then the governor may change the sampling rate too
 * late, up to 1 second later.  Thus, if we are reducing the sampling rate, we
 * need to make the new value effective immediately.
 *
 * ! This function must be called with filled dbs_data structure.
 */
void update_sampling_rate(struct dbs_data *dbs_data,
			  u32 *sampling_rate, u32 new_rate)
{
	int cpu;

	*sampling_rate = new_rate;

	get_online_cpus();
	for_each_online_cpu(cpu) {
		struct cpufreq_policy *policy;
		struct cpu_dbs_common_info *cdbs;
		unsigned long next_sampling, appointed_at;

		/* Try to get cpufreq policy of a cpu */
		policy = cpufreq_cpu_get(cpu);
		if (IS_ERR_OR_NULL(policy))
			continue;

		/* Load common dbs information of a get cpu */
		cdbs = dbs_data->get_cpu_cdbs(policy->cpu);
		cpufreq_cpu_put(policy);

		mutex_lock(&cdbs->timer_mutex);
		/* If sampling is not in progress now, return early */
		if (!delayed_work_pending(&cdbs->work)) {
			mutex_unlock(&cdbs->timer_mutex);
			continue;
		}

		/* Count both expire time samples */
		next_sampling = jiffies + usecs_to_jiffies(new_rate);
		appointed_at  = cdbs->work.timer.expires;

		/*
		 * If a new delay expires earlier than the current one, restart
		 * the timer with a new sampling rate to immediately prepare
		 * for a new sampling.
		 */
		if (time_before(next_sampling, appointed_at))
			mod_delayed_work_on(cdbs->cpu, dbs_data->gov_wq,
				&cdbs->work, usecs_to_jiffies(new_rate));
		mutex_unlock(&cdbs->timer_mutex);
	}
	put_online_cpus();
}
EXPORT_SYMBOL_GPL(update_sampling_rate);

static inline void dbs_timer_init(struct dbs_data *dbs_data,
				  struct cpu_dbs_common_info *cdbs,
				  unsigned int sampling_rate)
{
	int delay = align_delay(sampling_rate, 1);

	INIT_DEFERRABLE_WORK(&cdbs->work, dbs_data->gov_dbs_timer);
	queue_delayed_work_on(cdbs->cpu, dbs_data->gov_wq, &cdbs->work, delay);
}

static inline void dbs_timer_exit(struct cpu_dbs_common_info *cdbs)
{
	cancel_delayed_work_sync(&cdbs->work);
}

int cpufreq_governor_dbs(struct dbs_data *dbs_data,
			 struct cpufreq_policy *policy, u32 event)
{
	u32 sampling_rate = 0;
	int cpu = policy->cpu, ret, j;
	struct cpu_dbs_common_info *cdbs = dbs_data->get_cpu_cdbs(cpu), *j_cdbs;
	struct od_cpu_dbs_info_s *od_dbs_info = NULL, *j_od_dbs_info;
	struct cs_cpu_dbs_info_s *cs_dbs_info = NULL;
	struct od_dbs_tuners *od_tuners; /* ondemand gov. tunables */
	struct cs_dbs_tuners *cs_tuners; /* conservative gov. tunables */

	/* Load governor-specific data */
	switch (dbs_data->governor) {
	case GOV_ONDEMAND:
		od_dbs_info = dbs_data->get_cpu_dbs_info_s(cpu);
		od_tuners = dbs_data->tuners;
		sampling_rate = od_tuners->sampling_rate;
		break;
	case GOV_CONSERVATIVE:
		cs_dbs_info = dbs_data->get_cpu_dbs_info_s(cpu);
		cs_tuners = dbs_data->tuners;
		sampling_rate = cs_tuners->sampling_rate;
		break;
	}

	switch (event) {
	case CPUFREQ_GOV_START:
		/* This cannot happen, but be safe anyway */
		if (unlikely(!cpu_online(cpu) || !policy->cur))
			return -EINVAL;

		mutex_lock(&dbs_data->mutex);
		/* Update percpu data in a whole policy */
		for_each_cpu(j, policy->cpus) {
			j_cdbs = dbs_data->get_cpu_cdbs(j);

			j_cdbs->cpu = j;
			j_cdbs->prev_load = 0;
			j_cdbs->cur_policy = policy;
			j_cdbs->prev_cpu_idle = get_cpu_idle_time(j,
				&j_cdbs->prev_cpu_wall, should_io_be_busy());

			/* Some governors require specific initializations */
			switch (dbs_data->governor) {
			case GOV_ONDEMAND:
				j_od_dbs_info = dbs_data->get_cpu_dbs_info_s(j);
				set_cpus_allowed(j_od_dbs_info->sync_thread,
						 *cpumask_of(j));
				atomic_set(&j_od_dbs_info->sync_enabled, 1);
				break;
			}
		}

		/* Update the data of a main cpu in policy */
		switch (dbs_data->governor) {
		case GOV_ONDEMAND:
			od_dbs_info->rate_mult = 1;
			break;
		case GOV_CONSERVATIVE:
			cs_dbs_info->rate_mult = 1;
			cs_dbs_info->target_freq = policy->cur;
			break;
		}

		/* Some resources have to be initialized only once */
		ret = dbs_data->init(cdbs, policy);
		if (IS_ERR_VALUE(ret)) {
			mutex_unlock(&dbs_data->mutex);
			return ret;
		}
		mutex_unlock(&dbs_data->mutex);

		dbs_timer_init(dbs_data, cdbs, sampling_rate);
		break;
	case CPUFREQ_GOV_STOP:
		dbs_timer_exit(cdbs);

		mutex_lock(&dbs_data->mutex);
		dbs_data->exit(cdbs, policy);
		mutex_unlock(&dbs_data->mutex);
		break;
	case CPUFREQ_GOV_LIMITS:
		mutex_lock(&cdbs->timer_mutex);
		if (cdbs->cur_policy->cur > policy->max)
			__cpufreq_driver_target(cdbs->cur_policy, policy->max,
						CPUFREQ_RELATION_H);
		else if (cdbs->cur_policy->cur < policy->min)
			__cpufreq_driver_target(cdbs->cur_policy, policy->min,
						CPUFREQ_RELATION_L);
		/* Do not miss a sample here */
		switch (dbs_data->governor) {
		case GOV_ONDEMAND:
			dbs_data->od_check_cpu(od_dbs_info);
			break;
		case GOV_CONSERVATIVE:
			dbs_data->cs_check_cpu(cs_dbs_info);
			break;
		}
		mutex_unlock(&cdbs->timer_mutex);
		break;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(cpufreq_governor_dbs);
