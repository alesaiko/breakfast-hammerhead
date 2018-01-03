/*
 * Copyright (C) 2001, Russell King.
 * Copyright (C) 2002-2004, Dominik Brodowski <linux@brodo.de>
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

#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/cpufreq.h>

static DEFINE_PER_CPU(bool, cpu_is_managed);
static DEFINE_MUTEX(userspace_mutex);

/**
 * set_speed() - set the CPU frequency.
 * @policy: pointer to policy struct where freq is being set.
 * @freq: target frequency in kHz.
 */
static int set_speed(struct cpufreq_policy *policy, u32 freq)
{
	int ret = -EINVAL;

	mutex_lock(&userspace_mutex);
	/* Return early if userspace is unused */
	if (!per_cpu(cpu_is_managed, policy->cpu))
		goto err;

	/* Ensure the target frequency is within limits */
	freq = clamp_t(int, freq, policy->min, policy->max);

	/*
	 * We are safe from concurrent calls to ->target() here
	 * as we hold the userspace_mutex lock. If we were calling
	 * cpufreq_driver_target, a deadlock situation might occur:
	 * A: cpufreq_set (lock userspace_mutex) ->
	 *      cpufreq_driver_target (lock policy->lock)
	 * B: cpufreq_set_policy (lock policy->lock) ->
	 *      __cpufreq_governor ->
	 *        cpufreq_governor_userspace (lock userspace_mutex)
	 */
	ret = __cpufreq_driver_target(policy, freq, CPUFREQ_RELATION_C);
err:
	mutex_unlock(&userspace_mutex);

	return ret;
}

static ssize_t show_speed(struct cpufreq_policy *policy, char *buf)
{
	return scnprintf(buf, 12, "%u\n", policy->cur);
}

static int cpufreq_governor_userspace(struct cpufreq_policy *policy, u32 event)
{
	int cpu = policy->cpu;

	switch (event) {
	case CPUFREQ_GOV_START:
		/* This is unlikely to happen, but be safe anyway */
		if (unlikely(!cpu_online(cpu) || !policy->cur))
			return -EINVAL;

		mutex_lock(&userspace_mutex);
		per_cpu(cpu_is_managed, cpu) = true;
		mutex_unlock(&userspace_mutex);

		pr_debug("Started to manage CPU%u\n", cpu);
		break;
	case CPUFREQ_GOV_STOP:
		mutex_lock(&userspace_mutex);
		per_cpu(cpu_is_managed, cpu) = false;
		mutex_unlock(&userspace_mutex);

		pr_debug("Stopped to manage CPU%u\n", cpu);
		break;
	case CPUFREQ_GOV_LIMITS:
		pr_debug("Limit event for CPU%u: %u - %u kHz, cur -> %ukHz\n",
			  cpu, policy->min, policy->max, policy->cur);

		mutex_lock(&userspace_mutex);
		if (policy->cur > policy->max)
			__cpufreq_driver_target(policy, policy->max,
						CPUFREQ_RELATION_H);
		else if (policy->cur < policy->min)
			__cpufreq_driver_target(policy, policy->min,
						CPUFREQ_RELATION_L);
		mutex_unlock(&userspace_mutex);
		break;
	}

	return 0;
}

#ifndef CONFIG_CPU_FREQ_DEFAULT_GOV_USERSPACE
static
#endif
struct cpufreq_governor cpufreq_gov_userspace = {
	.name		= "userspace",
	.governor	= cpufreq_governor_userspace,
	.store_setspeed	= set_speed,
	.show_setspeed	= show_speed,
	.owner		= THIS_MODULE,
};

static int __init cpufreq_governor_init(void)
{
	return cpufreq_register_governor(&cpufreq_gov_userspace);
}

static void __exit cpufreq_governor_exit(void)
{
	cpufreq_unregister_governor(&cpufreq_gov_userspace);
}

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_USERSPACE
fs_initcall(cpufreq_governor_init);
#else
module_init(cpufreq_governor_init);
#endif
module_exit(cpufreq_governor_exit);

MODULE_AUTHOR("Dominik Brodowski <linux@brodo.de>, "
	      "Russell King <rmk@arm.linux.org.uk>");
MODULE_DESCRIPTION("CPUfreq policy governor 'userspace'");
MODULE_LICENSE("GPL v2");
