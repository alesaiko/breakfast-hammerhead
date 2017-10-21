/*
 * Copyright (C) 2013-2018, The Linux Foundation. All rights reserved.
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

#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt

#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/kernel_stat.h>
#include <linux/module.h>
#include <linux/tick.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/input.h>

struct cpu_sync {
	struct delayed_work boost_rem;
	struct task_struct *thread;
	int cpu, src_cpu;
	bool pending;
	u32 task_load;
	u32 boost_min;
	u32 input_boost_min;
	u32 input_boost_freq;
	spinlock_t lock;
	atomic_t being_woken;
	wait_queue_head_t sync_wq;
};
static DEFINE_PER_CPU(struct cpu_sync, sync_info);

static inline void *get_cpu_sync_info(int cpu)
{
	return &per_cpu(sync_info, cpu);
}

/* Workqueue used to run boosting algorithms on */
static struct workqueue_struct *cpu_boost_wq;

/* Instant input boosting work */
static struct work_struct input_boost_work;

/* Work used to stop the boosting after input_boost_ms milliseconds */
static struct delayed_work input_boost_rem;

/*
 * Time in milliseconds to keep frequencies of source and destination cpus
 * synchronized after the task migration event between them reported by sched.
 */
static unsigned int __read_mostly boost_ms;
module_param(boost_ms, uint, 0644);

/*
 * Boolean to determine whether the module should react to all task migration
 * events or only to those which maintain task load at least that specified by
 * migration_load_threshold. This variable also changes the way CPU frequencies
 * are going to be changed: when it is set to false, frequencies of source and
 * destination cpus are simply synchronized to a source's one; in case this is
 * set to true, the frequency is changed to either the load fraction of current
 * policy maximum or source's frequency, choosing the biggest of two.
 */
static bool __read_mostly load_based_syncs = true;
module_param(load_based_syncs, bool, 0644);

/*
 * Minimum task load that is considered as noticeable. If a task load is less
 * than this value, frequency synchronization will not occur.  Note that this
 * threshold is used only if load_based_syncs is enabled.
 */
static unsigned int __read_mostly migration_load_threshold = 30;
module_param(migration_load_threshold, uint, 0644);

/*
 * Frequency cap for synchronization algorithm. Shared frequency of synchronized
 * cpus will not go above this threshold if it is set to non-zero value.
 */
static unsigned int __read_mostly sync_threshold;
module_param(sync_threshold, uint, 0644);

/*
 * Time in milliseconds to keep frequencies of all online cpus boosted after an
 * input event.  Note that multiple input events, that occurred during the time
 * interval which is less or equal to min_input_interval, will be accounted as
 * one.
 */
static unsigned int __read_mostly input_boost_ms;
module_param(input_boost_ms, uint, 0644);

static unsigned long __read_mostly min_input_interval = 40 * USEC_PER_MSEC;
module_param(min_input_interval, ulong, 0644);

/*
 * Flag that is used to enable input boosting.  It is set by
 * set_input_boost_freq() call if input_boost_freq is non-zero.
 */
static bool __read_mostly input_boost_enabled;

static int set_input_boost_freq(const char *buf, const struct kernel_param *kp)
{
	int ntokens = 0, ret, i;
	const char *cp = buf;
	bool enabled = false;
	u32 val, cpu;

	while ((cp = strpbrk(cp + 1, " :")))
		++ntokens;

	/* Single number: apply to all CPUs */
	if (!ntokens) {
		ret = kstrtouint(buf, 10, &val);
		if (IS_ERR_VALUE(ret))
			return -EINVAL;

		for_each_possible_cpu(i)
			per_cpu(sync_info, i).input_boost_freq = val;

		goto check_enable;
	}

	/* CPU:value pair */
	if (unlikely(!(ntokens & 1)))
		return -EINVAL;

	cp = buf;
	for (i = 0; i < ntokens; i += 2) {
		ret = sscanf(cp, "%u:%u", &cpu, &val);
		if (ret != 2 ||
		   (int)cpu < 0 || cpu > (num_possible_cpus() - 1) ||
		   (int)val < 0 || val > (UINT_MAX - 1))
			return -EINVAL;

		per_cpu(sync_info, cpu).input_boost_freq = val;

		cp = strnchr(cp, PAGE_SIZE, ' ');
		if (IS_ERR_OR_NULL(cp))
			break;
		cp++;
	}

check_enable:
	for_each_possible_cpu(i) {
		if (per_cpu(sync_info, i).input_boost_freq) {
			enabled = true;
			break;
		}
	}

	input_boost_enabled = enabled;

	return 0;
}

static int get_input_boost_freq(char *buf, const struct kernel_param *kp)
{
	int len = 0, cpu;

	for_each_possible_cpu(cpu) {
		struct cpu_sync *s = get_cpu_sync_info(cpu);
		len += scnprintf(buf + len, 27, "%d:%u ",
				 cpu, s->input_boost_freq);
	}

	/* Remove whitespace and put a new line in a right position */
	scnprintf(buf + len - 1, 2, "\n");

	return len;
}

static const struct kernel_param_ops param_ops_input_boost_freq = {
	.set = set_input_boost_freq,
	.get = get_input_boost_freq,
};
module_param_cb(input_boost_freq, &param_ops_input_boost_freq, NULL, 0644);

/*
 * The CPUFREQ_ADJUST notifier is used to override the current policy min to
 * make sure policy min >= boost_min. The cpufreq framework then does the job
 * of enforcing the new policy.
 *
 * The sync kthread needs to run on the CPU in question to avoid deadlocks in
 * the wake up code. Achieve this by binding the thread to the respective
 * CPU. But a CPU going offline unbinds threads from that CPU. So, set it up
 * again each time the CPU comes back up. We can use CPUFREQ_START to figure
 * out a CPU is coming online instead of registering for hotplug notifiers.
 */
static int boost_adjust_notify(struct notifier_block *nb,
			       unsigned long val, void *data)
{
	struct cpufreq_policy *policy = data;
	struct cpu_sync *s = get_cpu_sync_info(policy->cpu);
	u32 b_min = s->boost_min, ib_min = s->input_boost_min, min;

	switch (val) {
	case CPUFREQ_ADJUST:
		if (!b_min && !ib_min)
			break;

		min = min(max(b_min, ib_min), policy->max);
		cpufreq_verify_within_limits(policy, min, UINT_MAX);
		break;
	case CPUFREQ_START:
		set_cpus_allowed(s->thread, *cpumask_of(s->cpu));
		break;
	}

	return NOTIFY_DONE;
}

static struct notifier_block boost_adjust_nb = {
	.notifier_call = boost_adjust_notify,
	.priority = SHRT_MAX,
};

static int boost_migration_notify(struct notifier_block *nb,
				  unsigned long unused, void *arg)
{
	struct migration_notify_data *mnd = arg;
	struct cpu_sync *s = get_cpu_sync_info(mnd->dest_cpu);
	unsigned long flags;

	if (!boost_ms)
		return NOTIFY_OK;

	if (load_based_syncs && mnd->load < migration_load_threshold)
		return NOTIFY_OK;

	/* Avoid deadlock in try_to_wake_up() */
	if (unlikely(s->thread == current))
		return NOTIFY_OK;

	spin_lock_irqsave(&s->lock, flags);
	s->pending = true;
	s->src_cpu = mnd->src_cpu;
	s->task_load = load_based_syncs ? mnd->load : 0;
	spin_unlock_irqrestore(&s->lock, flags);

	/*
	 * Avoid issuing recursive wakeup call, as sync thread itself could be
	 * seen as migrating triggering this notification. Note that sync thread
	 * of a cpu could be running for a short while with its affinity broken
	 * because of CPU hotplug.
	 */
	if (likely(!atomic_cmpxchg(&s->being_woken, 0, 1))) {
		wake_up(&s->sync_wq);
		atomic_set(&s->being_woken, 0);
	}

	return NOTIFY_DONE;
}

static struct notifier_block boost_migration_nb = {
	.notifier_call = boost_migration_notify,
	.priority = INT_MAX,
};

static void do_boost_rem(struct work_struct *work)
{
	struct cpu_sync *s =
		container_of(work, struct cpu_sync, boost_rem.work);

	s->boost_min = 0;

	cpufreq_update_policy(s->cpu);
}

static int boost_mig_sync_thread(void *data)
{
	int dest_cpu = (int)data, src_cpu, ret;
	struct cpu_sync *s = get_cpu_sync_info(dest_cpu);
	struct cpufreq_policy dest_policy, src_policy;
	unsigned long flags;
	u32 req_freq;

	for (;;) {
		wait_event_interruptible(s->sync_wq, s->pending ||
					 kthread_should_stop());

		if (kthread_should_stop())
			break;

		spin_lock_irqsave(&s->lock, flags);
		s->pending = false;
		src_cpu = s->src_cpu;
		spin_unlock_irqrestore(&s->lock, flags);

		ret  = cpufreq_get_policy(&src_policy, src_cpu);
		ret |= cpufreq_get_policy(&dest_policy, dest_cpu);
		if (IS_ERR_VALUE(ret))
			continue;

		req_freq = max(dest_policy.max * s->task_load / 100,
			       src_policy.cur);

		if (sync_threshold)
			req_freq = min(req_freq, sync_threshold);

		if (unlikely(req_freq <= dest_policy.cpuinfo.min_freq))
			continue;

		if (delayed_work_pending(&s->boost_rem))
			cancel_delayed_work_sync(&s->boost_rem);

		s->boost_min = req_freq;

		get_online_cpus();
		if (likely(cpu_online(src_cpu))) {
			/*
			 * Send an unchanged policy update to the source cpu.
			 * Even though the policy is not changed from its
			 * existing boosted or non-boosted state, notifying
			 * the source cpu will let the governor to know a
			 * boost happened on another cpu and that it should
			 * re-evaluate the frequency at the next timer event
			 * without interference from a min sample time.
			 */
			cpufreq_update_policy(src_cpu);
		}

		if (likely(cpu_online(dest_cpu))) {
			cpufreq_update_policy(dest_cpu);
			queue_delayed_work_on(dest_cpu, cpu_boost_wq,
				&s->boost_rem, msecs_to_jiffies(boost_ms));
		} else {
			s->boost_min = 0;
		}
		put_online_cpus();
	}

	return 0;
}

/**
 * update_policy_online() - call cpufreq_update_policy() for every online cpu.
 *
 * This function will lead to POLICY_NOTIFY for all online cpus, therefore
 * it will trigger all registered policy notifiers, including boost_adjust.
 */
static inline void update_policy_online(void)
{
	int cpu;

	get_online_cpus();
	for_each_online_cpu(cpu)
		cpufreq_update_policy(cpu);
	put_online_cpus();
}

static void do_input_boost_rem(struct work_struct *work)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		struct cpu_sync *s = get_cpu_sync_info(cpu);
		s->input_boost_min = 0;
	}

	update_policy_online();
}

static void do_input_boost(struct work_struct *work)
{
	int cpu;

	if (delayed_work_pending(&input_boost_rem))
		cancel_delayed_work_sync(&input_boost_rem);

	for_each_possible_cpu(cpu) {
		struct cpu_sync *s = get_cpu_sync_info(cpu);
		s->input_boost_min = s->input_boost_freq;
	}

	update_policy_online();

	queue_delayed_work(cpu_boost_wq, &input_boost_rem,
			   msecs_to_jiffies(input_boost_ms));
}

static void cpuboost_input_event(struct input_handle *handle,
				 unsigned int type, unsigned int code,
				 int value)
{
	static u64 last_input_time;
	u64 now;

	if (!input_boost_enabled || !input_boost_ms ||
	     work_pending(&input_boost_work))
		return;

	now = ktime_to_us(ktime_get());
	if (now - last_input_time <= min_input_interval)
		return;

	queue_work(cpu_boost_wq, &input_boost_work);
	last_input_time = ktime_to_us(ktime_get());
}

static int cpuboost_input_connect(struct input_handler *handler,
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

static void cpuboost_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id cpuboost_ids[] = {
	/* Multi-touch touchscreen */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			 INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] =
			     BIT_MASK(ABS_MT_POSITION_X) |
			     BIT_MASK(ABS_MT_POSITION_Y) },
	},
	/* Touchpad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_KEYBIT |
			 INPUT_DEVICE_ID_MATCH_ABSBIT,
		.keybit = { [BIT_WORD(BTN_TOUCH)] =
			     BIT_MASK(BTN_TOUCH) },
		.absbit = { [BIT_WORD(ABS_X)] =
			     BIT_MASK(ABS_X) |
			     BIT_MASK(ABS_Y) },
	},
	/* Keypad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_KEY) },
	},
	{ }
};

static struct input_handler cpuboost_input_handler = {
	.name		= "cpu-boost",
	.event		= cpuboost_input_event,
	.connect	= cpuboost_input_connect,
	.disconnect	= cpuboost_input_disconnect,
	.id_table	= cpuboost_ids,
};

static int __init cpu_boost_init(void)
{
	int ret, cpu;

	cpu_boost_wq = alloc_workqueue("cpuboost_wq", WQ_HIGHPRI, 0);
	if (IS_ERR_OR_NULL(cpu_boost_wq)) {
		pr_err("Unable to allocate workqueue\n");
		return -EFAULT;
	}

	INIT_WORK(&input_boost_work, do_input_boost);
	INIT_DELAYED_WORK(&input_boost_rem, do_input_boost_rem);

	for_each_possible_cpu(cpu) {
		struct cpu_sync *s = get_cpu_sync_info(cpu);

		s->cpu = cpu;
		INIT_DELAYED_WORK(&s->boost_rem, do_boost_rem);

		spin_lock_init(&s->lock);
		atomic_set(&s->being_woken, 0);
		init_waitqueue_head(&s->sync_wq);

		s->thread = kthread_run(boost_mig_sync_thread,
				       (void *)(long)cpu, "boost_sync/%d", cpu);
		set_cpus_allowed(s->thread, *cpumask_of(cpu));
	}

	ret = cpufreq_register_notifier(&boost_adjust_nb,
					CPUFREQ_POLICY_NOTIFIER);
	if (IS_ERR_VALUE(ret)) {
		pr_err("Unable to register cpufreq notifier\n");
		goto fail_cpufreq;
	}

	ret = atomic_notifier_chain_register(&migration_notifier_head,
					     &boost_migration_nb);
	if (IS_ERR_VALUE(ret)) {
		pr_err("Unable to register atomic notifier\n");
		goto fail_atomic;
	}

	ret = input_register_handler(&cpuboost_input_handler);
	if (IS_ERR_VALUE(ret)) {
		pr_err("Unable to register input handler\n");
		goto fail_input;
	}

	return 0;

fail_input:
	atomic_notifier_chain_unregister(&migration_notifier_head,
					 &boost_migration_nb);
fail_atomic:
	cpufreq_unregister_notifier(&boost_adjust_nb, CPUFREQ_POLICY_NOTIFIER);
fail_cpufreq:
	for_each_possible_cpu(cpu) {
		struct cpu_sync *s = get_cpu_sync_info(cpu);
		kthread_stop(s->thread);
	}

	destroy_workqueue(cpu_boost_wq);

	return ret;
}
late_initcall(cpu_boost_init);
