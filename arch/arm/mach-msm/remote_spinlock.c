/*
 * Copyright (c) 2008-2018, The Linux Foundation. All rights reserved.
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

#include <linux/err.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/of_address.h>

#include <mach/remote_spinlock.h>
#include <mach/msm_smem.h>

#define SPINLOCK_PID_APPS		(1)

#define SFPB_SPINLOCK_REG_BASE		(0x01200600)
#define SFPB_SPINLOCK_REG_SIZE		(132)
#define SFPB_SPINLOCK_LOCK_SIZE		(4)
#define SFPB_SPINLOCK_LOCK_COUNT	(8)
#define SFPB_SPINLOCK_LOCK_OFFSET	(4)

#define SMEM_SPINLOCK_COUNT		(8)
#define SMEM_SPINLOCK_ARRAY_SIZE	(sizeof(uint32_t) * SMEM_SPINLOCK_COUNT)

/* SFPB/Hardware Implementation */
static uint32_t __read_mostly lock_size;
static uint32_t __read_mostly lock_count;
static uint32_t __read_mostly lock_offset;

static bool __read_mostly hardware_lock;
static void *hw_mutex_reg_base;

static DEFINE_MUTEX(hw_map_lock);
static DEFINE_MUTEX(ops_lock);

struct spinlock_ops {
	void (*lock)	(raw_remote_spinlock_t *lock);
	void (*unlock)	(raw_remote_spinlock_t *lock);
	int  (*trylock)	(raw_remote_spinlock_t *lock);
	int  (*release)	(raw_remote_spinlock_t *lock, uint32_t pid);
	int  (*owner)	(raw_remote_spinlock_t *lock);
};

static struct spinlock_ops current_ops;

/**
 * __raw_remote_gen_spin_release() - release spinlock if it is owned by @pid.
 * @lock: pointer to lock structure.
 * @pid: identificator of a processor to release.
 *
 * This is designed to be used in situations where the processor owning the
 * spinlock has crashed and the spinlock must be released.
 */
static int
__raw_remote_gen_spin_release(raw_remote_spinlock_t *lock, uint32_t pid)
{
	if (unlikely(readl_relaxed(&lock->lock) != pid))
		return 1;

	writel_relaxed(0, &lock->lock);
	wmb();

	return 0;
}

/**
 * __raw_remote_gen_spin_owner() - return an owner of the spinlock.
 * @lock: pointer to lock structure.
 *
 * This is used for testing only. PIDs are assumed to be 31 bits or less.
 * Returns an owned PID [0; 31].
 */
static int __raw_remote_gen_spin_owner(raw_remote_spinlock_t *lock)
{
	rmb();
	return readl_relaxed(&lock->lock);
}

/**
 * LDREX/Generic Implementation:
 *
 * 1) Load the value from an appropriate memory address to a CPU register.
 * 2) Check whether the loaded value is zero (meaning it is unlocked).
 * 3) If these values are really equal, store the value of SPINLOCK_PID_APPS
 *    to the memory address used in the first step and store the result of the
 *    operation into "tmp" variable.
 * 4) Check whether an operation has been proceed successfully.
 * 5) Return back to the first step if the operation has failed.
 *
 * trylock() context it mostly the same except the last two steps. It simply
 * does not use a loop and tries to set a lock only once, returning the value
 * of "tmp" variable gained during STREX.
 *
 * unlock() just stores a value of zero to a specified memory address.
 *
 * LDREX (LoaD Register EXclusive) and STREX (STore Register EXclusive) are
 * used here. They are available in ARMv6 and above, thus they are completely
 * generic for all modern ARM processors.
 */
static void __raw_remote_ex_spin_lock(raw_remote_spinlock_t *lock)
{
	unsigned long tmp;

	__asm__ __volatile__(
"1:	ldrex	%0, [%1]\n"
"	teq	%0, #0\n"
"	strexeq	%0, %2, [%1]\n"
"	teqeq	%0, #0\n"
"	bne	1b"
	: "=&r" (tmp)
	: "r" (&lock->lock), "r" (SPINLOCK_PID_APPS)
	: "cc");

	smp_mb();
}

static int __raw_remote_ex_spin_trylock(raw_remote_spinlock_t *lock)
{
	unsigned long tmp;

	__asm__ __volatile__(
"	ldrex	%0, [%1]\n"
"	teq	%0, #0\n"
"	strexeq	%0, %2, [%1]\n"
	: "=&r" (tmp)
	: "r" (&lock->lock), "r" (SPINLOCK_PID_APPS)
	: "cc");

	if (likely(!tmp))
		smp_mb();

	return tmp == 0;
}

static void __raw_remote_ex_spin_unlock(raw_remote_spinlock_t *lock)
{
	int lock_owner;

	smp_mb();

	lock_owner = readl_relaxed(&lock->lock);
	if (unlikely(lock_owner != SPINLOCK_PID_APPS))
		pr_err("Spinlock not owned by APPS (%d)\n", lock_owner);

	__asm__ __volatile__(
"	str	%1, [%0]\n"
	:
	: "r" (&lock->lock), "r" (0)
	: "cc");
}

/**
 * SFPB Implementation:
 *
 * The design of this is deadly simple: we just read and store the values to
 * a specified memory location.  This is the simpliest and clearest possible
 * implementation of remote spin locking, therefore it is used by all modern
 * targets instead of *EX-based method.
 */
static void __raw_remote_sfpb_spin_lock(raw_remote_spinlock_t *lock)
{
	do {
		writel_relaxed(SPINLOCK_PID_APPS, lock);
		smp_mb();
	} while (readl_relaxed(lock) != SPINLOCK_PID_APPS);
}

static int __raw_remote_sfpb_spin_trylock(raw_remote_spinlock_t *lock)
{
	writel_relaxed(SPINLOCK_PID_APPS, lock);
	smp_mb();

	return readl_relaxed(lock) == SPINLOCK_PID_APPS;
}

static void __raw_remote_sfpb_spin_unlock(raw_remote_spinlock_t *lock)
{
	int lock_owner;

	lock_owner = readl_relaxed(lock);
	if (unlikely(lock_owner != SPINLOCK_PID_APPS))
		pr_err("Spinlock not owned by APPS (%d)\n", lock_owner);

	writel_relaxed(0, lock);
	smp_mb();
}

/**
 * node_init() - initialize memory area for ipc-spinlock from Device Tree.
 * @node: pointer to Device Tree node.
 * @reg_base: pointer to a variable where to store the register base.
 * @reg_size: pointer to a variable where to store the register size.
 *
 * This is used to read information from the DT node with a compat string of
 * "qcom,ipc-spinlock-sfpb". Read more about the DT implementation itself in
 * Documentation/devicetree/bindings/arm/msm/ipc-spinlock.txt.
 */
static __always_inline void
node_init(struct device_node *node, phys_addr_t *reg_base, uint32_t *reg_size)
{
	struct resource res;
	int ret;

	ret  = of_address_to_resource(node, 0, &res);
	ret |= of_property_read_u32(node, "qcom,num-locks", &lock_count);
	BUG_ON(ret || !lock_count);

	*reg_base = res.start;
	*reg_size = (uint32_t)(resource_size(&res));
	lock_size = *reg_size / lock_count;
	lock_offset = 0;
}

static inline void init_hw_mutex(void)
{
	struct device_node *node;
	phys_addr_t reg_base;
	uint32_t reg_size;

	node = of_find_compatible_node(NULL, NULL, "qcom,ipc-spinlock-sfpb");
	if (node) {
		node_init(node, &reg_base, &reg_size);
	} else {
		reg_base = SFPB_SPINLOCK_REG_BASE;
		reg_size = SFPB_SPINLOCK_REG_SIZE;
		lock_size = SFPB_SPINLOCK_LOCK_SIZE;
		lock_count = SFPB_SPINLOCK_LOCK_COUNT;
		lock_offset = SFPB_SPINLOCK_LOCK_OFFSET;
	}

	hw_mutex_reg_base = ioremap(reg_base, reg_size);
	BUG_ON(!hw_mutex_reg_base);
}

static int remote_spinlock_init_address_hw(int id, _remote_spinlock_t *lock)
{
	/*
	 * Optimistic locking. Init only needs to be done once by the first
	 * caller.  After that, serializing inits between different callers
	 * is unnecessary. The second check after the lock ensures init was
	 * not previously completed by someone else before it was grabbed.
	 */
	if (IS_ERR_OR_NULL(hw_mutex_reg_base)) {
		mutex_lock(&hw_map_lock);
		if (!hw_mutex_reg_base)
			init_hw_mutex();
		mutex_unlock(&hw_map_lock);
	}

	if (unlikely(id >= lock_count))
		return -EINVAL;

	*lock = hw_mutex_reg_base + lock_offset + lock_size * id;

	return 0;
}

static int remote_spinlock_init_address_smem(int id, _remote_spinlock_t *lock)
{
	_remote_spinlock_t spinlock_start;

	lock_count = SMEM_SPINLOCK_COUNT;
	if (unlikely(id >= lock_count))
		return -EINVAL;

	spinlock_start = smem_alloc(SMEM_SPINLOCK_ARRAY,
				    SMEM_SPINLOCK_ARRAY_SIZE);
	if (IS_ERR_OR_NULL(spinlock_start))
		return -ENXIO;

	*lock = spinlock_start + id;

	return 0;
}

static __always_inline int
remote_spinlock_init_address(int id, _remote_spinlock_t *lock)
{
	return hardware_lock ? remote_spinlock_init_address_hw(id, lock) :
			       remote_spinlock_init_address_smem(id, lock);
}

static __always_inline bool is_enabled(struct device_node *node)
{
	bool ret = false;

	/* Return early if Device Tree node is invalid */
	if (IS_ERR_OR_NULL(node))
		return false;
#ifdef CONFIG_OF
	ret = of_property_match_string(node, "status", "disabled") < 0;
#endif
	return ret;
}

static inline void initialize_ops(void)
{
	struct device_node *node;

	/* These calls are shared between both implementations */
	current_ops.release = __raw_remote_gen_spin_release;
	current_ops.owner = __raw_remote_gen_spin_owner;

	/*
	 * of_find_compatible_node() returns a valid pointer even if the status
	 * property is "disabled", so the validity needs to be checked.
	 */
	node = of_find_compatible_node(NULL, NULL, "qcom,ipc-spinlock-sfpb");
	if (is_enabled(node) || IS_ENABLED(CONFIG_MSM_REMOTE_SPINLOCK_SFPB)) {
		current_ops.lock = __raw_remote_sfpb_spin_lock;
		current_ops.unlock = __raw_remote_sfpb_spin_unlock;
		current_ops.trylock = __raw_remote_sfpb_spin_trylock;

		hardware_lock = true;
		pr_info("Hardware implementation was initialized\n");
	} else {
		current_ops.lock = __raw_remote_ex_spin_lock;
		current_ops.unlock = __raw_remote_ex_spin_unlock;
		current_ops.trylock = __raw_remote_ex_spin_trylock;

		hardware_lock = false;
		pr_info("LDREX implementation was initialized\n");
	}
}

int _remote_spin_lock_init(remote_spinlock_id_t id, _remote_spinlock_t *lock)
{
	BUG_ON(!id);

	/*
	 * Optimistic locking. Init only needs to be done once by the first
	 * caller.  After that, serializing inits between different callers
	 * is unnecessary. The second check after the lock ensures init was
	 * not previously completed by someone else before it was grabbed.
	 */
	if (IS_ERR_OR_NULL(current_ops.lock)) {
		mutex_lock(&ops_lock);
		if (!current_ops.lock)
			initialize_ops();
		mutex_unlock(&ops_lock);
	}

	/* Single-digit lock ID follows "S:" */
	BUG_ON(id[0] != 'S' || id[1] != ':' || id[3] != '\0');
	return remote_spinlock_init_address((((uint8_t)id[2]) - '0'), lock);
}

/*
 * Lock comes in as a pointer to a pointer to the lock location, so it
 * must be dereferenced and casted to the right type for the actual lock
 * implementation functions.
 */
int _remote_spin_release(_remote_spinlock_t *lock, uint32_t pid)
{
	BUG_ON(!current_ops.release);
	return current_ops.release((raw_remote_spinlock_t *)(*lock), pid);
}
EXPORT_SYMBOL(_remote_spin_release);

int _remote_spin_owner(_remote_spinlock_t *lock)
{
	BUG_ON(!current_ops.owner);
	return current_ops.owner((raw_remote_spinlock_t *)(*lock));
}
EXPORT_SYMBOL(_remote_spin_owner);

int _remote_spin_trylock(_remote_spinlock_t *lock)
{
	BUG_ON(!current_ops.trylock);
	return current_ops.trylock((raw_remote_spinlock_t *)(*lock));
}
EXPORT_SYMBOL(_remote_spin_trylock);

void _remote_spin_lock(_remote_spinlock_t *lock)
{
	BUG_ON(!current_ops.lock);
	current_ops.lock((raw_remote_spinlock_t *)(*lock));
}
EXPORT_SYMBOL(_remote_spin_lock);

void _remote_spin_unlock(_remote_spinlock_t *lock)
{
	BUG_ON(!current_ops.unlock);
	current_ops.unlock((raw_remote_spinlock_t *)(*lock));
}
EXPORT_SYMBOL(_remote_spin_unlock);

/**
 * _remote_spin_release_all() - release all spinlocks owned by @pid.
 * @pid: processor ID of a processor to release.
 *
 * This is only to be used for situations where the processor owning
 * spinlocks has crashed and the spinlocks must be released.
 */
void _remote_spin_release_all(uint32_t pid)
{
	_remote_spinlock_t lock;
	register int n;

	for (n = 0; n < lock_count; n++)
		if (!remote_spinlock_init_address(n, &lock))
			_remote_spin_release(&lock, pid);
}

/* Remote Mutex Implementation */
int _remote_mutex_init(struct remote_mutex_id *id, _remote_mutex_t *lock)
{
	BUG_ON(!id);

	lock->delay_us = id->delay_us;
	return _remote_spin_lock_init(id->r_spinlock_id, &lock->r_spinlock);
}
EXPORT_SYMBOL(_remote_mutex_init);

int _remote_mutex_trylock(_remote_mutex_t *lock)
{
	return _remote_spin_trylock(&lock->r_spinlock);
}
EXPORT_SYMBOL(_remote_mutex_trylock);

void _remote_mutex_lock(_remote_mutex_t *lock)
{
	while (!_remote_spin_trylock(&lock->r_spinlock))
		lock->delay_us >= 1000 ?
			msleep(lock->delay_us / 1000) :
			udelay(lock->delay_us);
}
EXPORT_SYMBOL(_remote_mutex_lock);

void _remote_mutex_unlock(_remote_mutex_t *lock)
{
	_remote_spin_unlock(&lock->r_spinlock);
}
EXPORT_SYMBOL(_remote_mutex_unlock);
