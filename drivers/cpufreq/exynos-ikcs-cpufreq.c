/*
 * Copyright (c) 2012 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * EXYNOS - CPU frequency scaling support for EXYNOS series
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/regulator/consumer.h>
#include <linux/cpufreq.h>
#include <linux/suspend.h>
#include <linux/module.h>
#include <linux/reboot.h>
#include <linux/delay.h>
#include <linux/cpu.h>
#include <linux/pm_qos.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/sysfs_helpers.h>
#include <linux/cpumask.h>

#include <asm/cputype.h>
#include <asm/bL_switcher.h>
#include <mach/cpufreq.h>
#include <mach/regs-pmu.h>
#include <mach/tmu.h>
#include <mach/asv-exynos.h>
#include <plat/cpu.h>

struct lpj_info {
	unsigned long   ref;
	unsigned int    freq;
};

static struct lpj_info global_lpj_ref;

/* Use boot_freq when entering sleep mode */
static unsigned int boot_freq;

/* For switcher */
static unsigned int freq_min[CA_END] __read_mostly;	/* Minimum (Big/Little) clock frequency */
static unsigned int freq_max[CA_END] __read_mostly;	/* Maximum (Big/Little) clock frequency */

#define ACTUAL_FREQ(x, cur)  ((cur == CA7) ? (x) << 1 : (x))
#define VIRT_FREQ(x, cur)    ((cur == CA7) ? (x) >> 1 : (x))

/*
 * This value is based on the difference between the dmips value of A15/A7
 * It is used to revise cpu frequency when changing cluster
 */
#ifdef CONFIG_SOC_EXYNOS5410_CA7_OVERCLOCK
#define STEP_LEVEL_CA7_MAX	650000
#else
#define STEP_LEVEL_CA7_MAX	600000
#endif
#define STEP_LEVEL_CA15_MIN	700000

#define LIMIT_COLD_VOLTAGE	1250000
#define COLD_VOLTAGE_OFFSET	75000

/*
 * Taken from asv-exynos5410.h
 */
#define ARM_MAX_VOLT		1362500
#define KFC_MAX_VOLT		1312500

static struct exynos_dvfs_info *exynos_info[CA_END];
static struct exynos_dvfs_info exynos_info_CA7;
static struct exynos_dvfs_info exynos_info_CA15;

static struct cpufreq_frequency_table *merge_freq_table;
static unsigned int *merge_index_table;

static struct regulator *arm_regulator;
static struct regulator *kfc_regulator;
static unsigned int volt_offset;
static int volt_powerdown[CA_END];

static struct cpufreq_freqs *freqs[CA_END];

static unsigned int exynos5410_bb_con0;

static DEFINE_MUTEX(cpufreq_lock);
static DEFINE_MUTEX(cpufreq_scale_lock);

static bool exynos_cpufreq_init_done;

/* Include CPU mask of each cluster */
static cluster_type boot_cluster;

DEFINE_PER_CPU(cluster_type, cpu_cur_cluster);

static struct pm_qos_request boot_cpu_qos;
static struct pm_qos_request min_cpu_qos;
static struct pm_qos_request max_cpu_qos;
struct pm_qos_request max_cpu_qos_blank;

#ifdef CONFIG_ASV_MARGIN_TEST
static int set_cpu_freq = 0;
static int __init get_cpu_freq(char *str)
{
	get_option(&str, &set_cpu_freq);
	return 0;
}
early_param("cpufreq", get_cpu_freq);
#endif

static unsigned int get_limit_voltage(unsigned int voltage)
{
	if (voltage > LIMIT_COLD_VOLTAGE)
		return voltage;

	if (voltage + volt_offset > LIMIT_COLD_VOLTAGE)
		return LIMIT_COLD_VOLTAGE;

	return voltage + volt_offset;
}

static void init_cpumask_cluster_set(unsigned int cluster)
{
	unsigned int i;

	for_each_cpu(i, cpu_possible_mask)
		per_cpu(cpu_cur_cluster, i) = cluster;
}

/*
 * get_cur_cluster - return current cluster
 *
 * You may reference this fuction directly, but it cannot be
 * standard of judging current cluster. If you make a decision
 * of operation by this function, it occurs system hang.
 */
cluster_type get_cur_cluster(unsigned int cpu)
{
	return per_cpu(cpu_cur_cluster, cpu);
}

cluster_type get_boot_cluster(void)
{
	return boot_cluster;
}

void reset_lpj_for_cluster(cluster_type cluster)
{
}

static void set_boot_freq(void)
{
	int i;

	for (i = 0; i < CA_END; i++) {
		if (exynos_info[i] == NULL)
			continue;

		exynos_info[i]->boot_freq
				= clk_get_rate(exynos_info[i]->cpu_clk) / 1000;
	}
}

static unsigned int get_boot_freq(unsigned int cluster)
{
	if (exynos_info[cluster] == NULL)
		return 0;

	return exynos_info[cluster]->boot_freq;
}

static unsigned int get_real_index(unsigned int index)
{
	return merge_index_table[index];
}

/* Get table size */
static unsigned int cpufreq_get_table_size(
				struct cpufreq_frequency_table *table,
				unsigned int cluster_id)
{
	int size = 0;

	if (cluster_id == CA15) {
		for (; (table[size].frequency != CPUFREQ_TABLE_END); size++)
			;
	} else {
		for (; (table[size].frequency != CPUFREQ_TABLE_END); size++)
			;
	}
	return size;
}

/*
 * copy entries of all the per-cluster cpufreq_frequency_table entries
 * into a single frequency table which is published to cpufreq core
 */
static int cpufreq_merge_tables(void)
{
	int cluster_id, i, index = 0;
	unsigned int total_sz = 0, size[CA_END];
	struct cpufreq_frequency_table *freq_table;

	for (cluster_id = 0; cluster_id < CA_END; cluster_id++) {
		size[cluster_id] =  cpufreq_get_table_size(
		   exynos_info[cluster_id]->freq_table, cluster_id);
		total_sz += size[cluster_id];
	}

	merge_index_table = kzalloc(sizeof(unsigned int) *
						(total_sz + 1), GFP_KERNEL);
	if (!merge_index_table)
		return -ENOMEM;

	freq_table = kzalloc(sizeof(struct cpufreq_frequency_table) *
						(total_sz + 1), GFP_KERNEL);
	if (!freq_table)
		return -ENOMEM;

	merge_freq_table = freq_table;

	memcpy(freq_table, exynos_info[CA15]->freq_table,
			size[CA15] * sizeof(struct cpufreq_frequency_table));
	freq_table += size[CA15];
	memcpy(freq_table, exynos_info[CA7]->freq_table,
			size[CA7] * sizeof(struct cpufreq_frequency_table));

	for (i = size[CA15]; i <= total_sz ; i++) {
		if (merge_freq_table[i].frequency != CPUFREQ_ENTRY_INVALID)
			merge_freq_table[i].frequency >>= 1;
	}

	merge_freq_table[total_sz].frequency = CPUFREQ_TABLE_END;

	for (i = 0; merge_freq_table[i].frequency != CPUFREQ_TABLE_END; i++) {

		merge_index_table[i] = index++;

		if (index == size[CA15])
			index = 0;

		pr_debug("merged_table index: %d freq: %d\n", i,
						merge_freq_table[i].frequency);
	}

	return 0;
}

static bool is_alive(unsigned int cluster)
{
	unsigned int tmp;
	tmp = __raw_readl(cluster == CA15 ? EXYNOS5410_ARM_COMMON_STATUS :
					EXYNOS5410_KFC_COMMON_STATUS) & 0x3;

	return tmp ? true : false;
}

int exynos_verify_speed(struct cpufreq_policy *policy)
{
	return cpufreq_frequency_table_verify(policy, merge_freq_table);
}

unsigned int exynos_getspeed_cluster(cluster_type cluster)
{
	return VIRT_FREQ(clk_get_rate(exynos_info[cluster]->cpu_clk) / 1000, cluster);
}

unsigned int exynos_getspeed(unsigned int cpu)
{
	unsigned int cur = get_cur_cluster(cpu);

	return exynos_getspeed_cluster(cur);
}

static unsigned int exynos_get_safe_volt(unsigned int old_index,
					unsigned int new_index,
					unsigned int cur)
{
	unsigned int safe_arm_volt = 0;
	struct cpufreq_frequency_table *freq_table
					= exynos_info[cur]->freq_table;
	unsigned int *volt_table = exynos_info[cur]->volt_table;

	/*
	 * ARM clock source will be changed APLL to MPLL temporary
	 * To support this level, need to control regulator for
	 * reguired voltage level
	 */
	if (exynos_info[cur]->need_apll_change != NULL) {
		if (exynos_info[cur]->need_apll_change(old_index, new_index) &&
			(freq_table[new_index].frequency
					< exynos_info[cur]->mpll_freq_khz) &&
			(freq_table[old_index].frequency
					< exynos_info[cur]->mpll_freq_khz)) {
				safe_arm_volt
				  = volt_table[exynos_info[cur]->pll_safe_idx];
		}
	}

	return safe_arm_volt;
}

/* Determine valid target frequency using freq_table */
int exynos5_frequency_table_target(struct cpufreq_policy *policy,
				   struct cpufreq_frequency_table *table,
				   unsigned int target_freq,
				   unsigned int relation,
				   unsigned int *index)
{
	unsigned int i;

	if (!cpu_online(policy->cpu))
		return -EINVAL;

	for (i = 0; (table[i].frequency != CPUFREQ_TABLE_END); i++) {
		unsigned int freq = table[i].frequency;
		if (freq == CPUFREQ_ENTRY_INVALID)
			continue;

		if (target_freq == freq) {
			*index = i;
			break;
		}
	}

	if (table[i].frequency == CPUFREQ_TABLE_END)
		return -EINVAL;

	return 0;
}

static int exynos_cpufreq_scale(unsigned int target_freq,
				unsigned int curr_freq, unsigned int cpu)
{
	unsigned int cur = get_cur_cluster(cpu);
	unsigned int *volt_table = exynos_info[cur]->volt_table;
	struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);
	struct regulator *regulator = exynos_info[cur]->regulator;
	unsigned int new_index, old_index;
	unsigned int volt, safe_volt = 0;
	int ret = 0;

	if (!policy)
		return ret;

	if (!is_alive(cur))
		goto out;

	freqs[cur]->cpu = cpu;
	freqs[cur]->new = target_freq;

	if (exynos5_frequency_table_target(policy, merge_freq_table,
				curr_freq, CPUFREQ_RELATION_L, &old_index)) {
		ret = -EINVAL;
		goto out;
	}

	if (exynos5_frequency_table_target(policy, merge_freq_table,
				freqs[cur]->new, CPUFREQ_RELATION_L, &new_index)) {
		ret = -EINVAL;
		goto out;
	}

	if (old_index == new_index)
		goto out;

	old_index = get_real_index(old_index);
	new_index = get_real_index(new_index);

	/*
	 * ARM clock source will be changed APLL to MPLL temporary
	 * To support this level, need to control regulator for
	 * required voltage level
	 */
	safe_volt = exynos_get_safe_volt(old_index, new_index, cur);
	if (safe_volt)
		safe_volt = get_limit_voltage(safe_volt);

	volt = get_limit_voltage(volt_table[new_index]);

	/* Update policy current frequency */
	cpufreq_notify_transition(freqs[cur], CPUFREQ_PRECHANGE);

	/* When the new frequency is higher than current frequency */
	if ((ACTUAL_FREQ(freqs[cur]->new, cur) >
			ACTUAL_FREQ(freqs[cur]->old, cur)) && !safe_volt){
		/* Firstly, voltage up to increase frequency */
		regulator_set_voltage(regulator, volt, volt);

		if (exynos_info[cur]->set_ema)
			exynos_info[cur]->set_ema(volt);
	}

	if (safe_volt) {
		regulator_set_voltage(regulator, safe_volt, safe_volt);

		if (exynos_info[cur]->set_ema)
			exynos_info[cur]->set_ema(safe_volt);
	}
	exynos_info[cur]->set_freq(old_index, new_index);

	if (!global_lpj_ref.freq) {
		global_lpj_ref.ref = loops_per_jiffy;
		global_lpj_ref.freq = freqs[cur]->old;
	}

	loops_per_jiffy = cpufreq_scale(global_lpj_ref.ref,
			global_lpj_ref.freq, freqs[cur]->new);
	if (cur == CA7)
		loops_per_jiffy = loops_per_jiffy * 2;

	cpufreq_notify_transition(freqs[cur], CPUFREQ_POSTCHANGE);

	/* When the new frequency is lower than current frequency */
	if ((ACTUAL_FREQ(freqs[cur]->new, cur) <
					ACTUAL_FREQ(freqs[cur]->old, cur)) ||
	   ((ACTUAL_FREQ(freqs[cur]->new, cur) >
			ACTUAL_FREQ(freqs[cur]->old, cur)) && safe_volt)) {
		/* down the voltage after frequency change */
		if (exynos_info[cur]->set_ema)
			 exynos_info[cur]->set_ema(volt);

		regulator_set_voltage(regulator, volt, volt);
	}

out:
	cpufreq_cpu_put(policy);
	return ret;
}

struct exynos_switch_arg {
       unsigned int cluster;
       int result;
       struct work_struct work;
};

static void exynos_cluster_switch_request(struct work_struct* work)
{
       struct exynos_switch_arg *args = container_of(work,
                                               struct exynos_switch_arg, work);
       args->result = bL_cluster_switch_request(!args->cluster);
}

static cluster_type exynos_switch(struct cpufreq_policy *policy, cluster_type cur)
{
	int cpu;
	cluster_type new_cluster;
	struct exynos_switch_arg args;
	unsigned int core;

	asm volatile ("mrc\tp15, 0, %0, c0, c0, 5\n":"=r"(core));
	core = core & 0xf;
	new_cluster = !cur;

	if ((core == 0) &&
	    (cpumask_equal(&current->cpus_allowed, cpumask_of(core)))) {
		if (bL_cluster_switch_request(!new_cluster))
			return cur;
	} else {
		args.cluster = new_cluster;
		args.result = -1;
		INIT_WORK_ONSTACK(&args.work, exynos_cluster_switch_request);
		schedule_work_on(0, &args.work);
		flush_work(&args.work);
		if (args.result)
			return cur;
	}

	for_each_online_cpu(cpu)
		per_cpu(cpu_cur_cluster, cpu) = new_cluster;

	return new_cluster;
}

unsigned int g_cpufreq;
/* Set clock frequency */
static int exynos_target(struct cpufreq_policy *policy,
			  unsigned int target_freq,
			  unsigned int relation)
{
	/* read current cluster */
	cluster_type cur, old_cur;
	unsigned int index;
	int count, ret = 0;
	bool do_switch = false;

	mutex_lock(&cpufreq_lock);

	cur = get_cur_cluster(policy->cpu);
	old_cur = cur;

	if (exynos_info[cur]->blocked)
		goto out;

	/* get current frequency */
	freqs[cur]->old = exynos_getspeed(policy->cpu);

	target_freq = max((unsigned int)pm_qos_request(PM_QOS_CPU_FREQ_MIN), target_freq);
	target_freq = min((unsigned int)pm_qos_request(PM_QOS_CPU_FREQ_MAX), target_freq);

	count = num_online_cpus();
	target_freq = min(target_freq, exynos_info[cur]->max_op_freqs[count]);

#ifdef CONFIG_ASV_MARGIN_TEST
	if (set_cpu_freq > 0) {
		target_freq = set_cpu_freq;
	}
#endif

	if (cpufreq_frequency_table_target(policy, merge_freq_table,
				target_freq, relation, &index)) {
		ret = -EINVAL;
		goto out;
	}

	target_freq = merge_freq_table[index].frequency;

	if (cur == CA15 && target_freq < STEP_LEVEL_CA15_MIN)
		do_switch = true;
	else if (cur == CA7 && target_freq > STEP_LEVEL_CA7_MAX)
		do_switch = true;

#ifdef CONFIG_BL_SWITCHER
	if (do_switch) {
		cur = exynos_switch(policy, old_cur);
		if (old_cur == cur)
			goto out;	/* Switching failed, No operation */

		freqs[cur]->old = exynos_getspeed_cluster(cur);
		policy->cur = freqs[cur]->old;
	}
#endif
	/* frequency and volt scaling */
	ret = exynos_cpufreq_scale(target_freq, freqs[cur]->old, policy->cpu);

out:
	mutex_unlock(&cpufreq_lock);
	g_cpufreq = target_freq;
	return ret;
}

#ifdef CONFIG_PM
static int exynos_cpufreq_suspend(struct cpufreq_policy *policy)
{
	exynos5410_bb_con0 = __raw_readl(EXYNOS5410_BB_CON0);
	return 0;
}

static int exynos_cpufreq_resume(struct cpufreq_policy *policy)
{
	freqs[CA7]->old = VIRT_FREQ(get_boot_freq(CA7), CA7);
	freqs[CA15]->old = VIRT_FREQ(get_boot_freq(CA15), CA15);

	__raw_writel(exynos5410_bb_con0, EXYNOS5410_BB_CON0);
	return 0;
}
#endif

void exynos_lowpower_for_cluster(cluster_type cluster, bool on)
{
	int volt;

	mutex_lock(&cpufreq_lock);
	if (cluster == CA15) {
		if (on) {
			volt_powerdown[CA15] = regulator_get_voltage(arm_regulator);
			volt = get_match_volt(ID_ARM, ACTUAL_FREQ(freq_min[CA15], CA15));
			volt = get_limit_voltage(volt);
			regulator_set_voltage(arm_regulator, volt, volt);
			if (exynos_info[CA15]->set_ema)
				exynos_info[CA15]->set_ema(volt);
		} else {
			volt = volt_powerdown[CA15];
			volt = get_limit_voltage(volt);
			regulator_set_voltage(arm_regulator, volt, volt);
		}
	} else {
		if (on) {
			volt_powerdown[CA7] = regulator_get_voltage(kfc_regulator);
			volt = get_match_volt(ID_KFC, ACTUAL_FREQ(freq_min[CA7], CA7));
			volt = get_limit_voltage(volt);
			regulator_set_voltage(kfc_regulator, volt, volt);
			if (exynos_info[CA7]->set_ema)
				exynos_info[CA7]->set_ema(volt);
		} else {
			volt = volt_powerdown[CA7];
			volt = get_limit_voltage(volt);
			regulator_set_voltage(kfc_regulator, volt, volt);
		}
	}
	mutex_unlock(&cpufreq_lock);
}

/*
 * exynos_cpufreq_pm_notifier - block CPUFREQ's activities in suspend-resume
 *			context
 * @notifier
 * @pm_event
 * @v
 *
 * While cpufreq_disable == true, target() ignores every frequency but
 * boot_freq. The boot_freq value is the initial frequency,
 * which is set by the bootloader. In order to eliminate possible
 * inconsistency in clock values, we save and restore frequencies during
 * suspend and resume and block CPUFREQ activities. Note that the standard
 * suspend/resume cannot be used as they are too deep (syscore_ops) for
 * regulator actions.
 */
static int exynos_cpufreq_pm_notifier(struct notifier_block *notifier,
				       unsigned long pm_event, void *v)
{
	unsigned int freqCA7, freqCA15;
	unsigned int bootfreqCA7, bootfreqCA15;
	int volt;

	switch (pm_event) {
	case PM_SUSPEND_PREPARE:
		/*
		 * CPU frequency is limited when System enter sleep
		 * This limitation should be release when exiting sleep mode
		 * You can find releasing this pm_qos request at cpufreq_ondemand
		 */
		if (pm_qos_request_active(&max_cpu_qos_blank))
			pm_qos_update_request(&max_cpu_qos_blank, 600000);
		else
			pm_qos_add_request(&max_cpu_qos_blank, PM_QOS_CPU_FREQ_MAX, 600000);

		mutex_lock(&cpufreq_lock);
		exynos_info[CA7]->blocked = true;
		exynos_info[CA15]->blocked = true;
		mutex_unlock(&cpufreq_lock);

		bootfreqCA7 = VIRT_FREQ(get_boot_freq(CA7), CA7);
		bootfreqCA15 = VIRT_FREQ(get_boot_freq(CA15), CA15);

		freqCA7 = exynos_getspeed_cluster(CA7);
		freqCA15 = exynos_getspeed_cluster(CA15);

		volt = max(get_match_volt(ID_KFC, ACTUAL_FREQ(bootfreqCA7, CA7)),
				get_match_volt(ID_KFC, ACTUAL_FREQ(freqCA7, CA7)));
		volt = get_limit_voltage(volt);

		if (regulator_set_voltage(kfc_regulator, volt, volt))
			goto err;

		volt = max(get_match_volt(ID_ARM, ACTUAL_FREQ(bootfreqCA15, CA15)),
				get_match_volt(ID_ARM, ACTUAL_FREQ(freqCA15, CA15)));
		volt = get_limit_voltage(volt);

		if (regulator_set_voltage(arm_regulator, volt, volt))
			goto err;

		pr_debug("PM_SUSPEND_PREPARE for CPUFREQ\n");
		break;
	case PM_POST_SUSPEND:
		pr_debug("PM_POST_SUSPEND for CPUFREQ\n");

		mutex_lock(&cpufreq_lock);
		exynos_info[CA7]->blocked = false;
		exynos_info[CA15]->blocked = false;
		mutex_unlock(&cpufreq_lock);

		break;
	}
	return NOTIFY_OK;
err:
	pr_err("%s: failed to set voltage\n", __func__);

	return NOTIFY_BAD;
}

static struct notifier_block exynos_cpufreq_nb = {
	.notifier_call = exynos_cpufreq_pm_notifier,
};

static int exynos_cpufreq_tmu_notifier(struct notifier_block *notifier,
				       unsigned long event, void *v)
{
	int volt;
	int *on = v;

	if (event != TMU_COLD)
		return NOTIFY_OK;

	mutex_lock(&cpufreq_lock);
	if (*on) {
		if (volt_offset)
			goto out;
		else
			volt_offset = COLD_VOLTAGE_OFFSET;

		volt = get_limit_voltage(regulator_get_voltage(arm_regulator));
		regulator_set_voltage(arm_regulator, volt, volt);

		volt = get_limit_voltage(regulator_get_voltage(kfc_regulator));
		regulator_set_voltage(kfc_regulator, volt, volt);
	} else {
		if (!volt_offset)
			goto out;
		else
			volt_offset = 0;

		volt = get_limit_voltage(regulator_get_voltage(arm_regulator)
							- COLD_VOLTAGE_OFFSET);
		regulator_set_voltage(arm_regulator, volt, volt);

		volt = get_limit_voltage(regulator_get_voltage(kfc_regulator)
							- COLD_VOLTAGE_OFFSET);
		regulator_set_voltage(kfc_regulator, volt, volt);
	}

	if (exynos_info[CA15]->set_ema)
		exynos_info[CA15]->set_ema(volt);

	if (exynos_info[CA7]->set_ema)
		exynos_info[CA7]->set_ema(volt);

out:
	mutex_unlock(&cpufreq_lock);

	return NOTIFY_OK;
}

static struct notifier_block exynos_tmu_nb = {
	.notifier_call = exynos_cpufreq_tmu_notifier,
};

static int exynos_cpufreq_cpu_init(struct cpufreq_policy *policy)
{
	pr_debug("%s: cpu[%d]\n", __func__, policy->cpu);
	policy->cur = policy->min = policy->max = exynos_getspeed(policy->cpu);
	freqs[CA7]->old = exynos_getspeed_cluster(CA7);
	freqs[CA15]->old = exynos_getspeed_cluster(CA15);

	boot_freq = exynos_getspeed(policy->cpu);

	cpufreq_frequency_table_get_attr(
		merge_freq_table, policy->cpu);

	/* set the transition latency value */
	policy->cpuinfo.transition_latency = 100000;

	if (num_online_cpus() == 1) {
		cpumask_copy(policy->related_cpus, cpu_possible_mask);
		cpumask_copy(policy->cpus, cpu_online_mask);
	} else {
		cpumask_setall(policy->cpus);
	}

	return cpufreq_frequency_table_cpuinfo(
		policy, merge_freq_table);
}

static struct cpufreq_driver exynos_driver = {
	.flags		= CPUFREQ_STICKY,
	.verify		= exynos_verify_speed,
	.target		= exynos_target,
	.get		= exynos_getspeed,
	.init		= exynos_cpufreq_cpu_init,
	.name		= "exynos_cpufreq",
#ifdef CONFIG_PM
	.suspend	= exynos_cpufreq_suspend,
	.resume		= exynos_cpufreq_resume,
#endif

};

/************************** sysfs interface ************************/

static ssize_t show_freq_table(struct kobject *kobj,
			     struct attribute *attr, char *buf)
{
	int i, count = 0;

	for (i = 0; merge_freq_table[i].frequency != CPUFREQ_TABLE_END; i++) {
		if (merge_freq_table[i].frequency != CPUFREQ_ENTRY_INVALID)
			count += sprintf(&buf[count], "%d ",
				merge_freq_table[i].frequency);
        }

        count += sprintf(&buf[count], "\n");
        return count;
}

static ssize_t show_min_freq(struct kobject *kobj,
			     struct attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", (unsigned int)pm_qos_request(PM_QOS_CPU_FREQ_MIN));
}

static ssize_t show_max_freq(struct kobject *kobj,
			     struct attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", (unsigned int)pm_qos_request(PM_QOS_CPU_FREQ_MAX));
}

static ssize_t store_min_freq(struct kobject *kobj, struct attribute *attr,
			      const char *buf, size_t count)
{
	int input;

	if (!sscanf(buf, "%d", &input))
		return -EINVAL;

	if (input > 0)
		input = min(input, (int)freq_max[CA15]);

	if (input <= (int)freq_min[CA7]) {
		if (pm_qos_request_active(&min_cpu_qos))
			pm_qos_update_request(&min_cpu_qos, freq_min[CA7]);
	} else {
		if (pm_qos_request_active(&min_cpu_qos))
			pm_qos_update_request(&min_cpu_qos, input);
		else
			pm_qos_add_request(&min_cpu_qos, PM_QOS_CPU_FREQ_MIN, input);
	}

	return count;
}

static ssize_t store_max_freq(struct kobject *kobj, struct attribute *attr,
			      const char *buf, size_t count)
{
	int input;

	if (!sscanf(buf, "%d", &input))
		return -EINVAL;

	if (input > 0)
		input = max(input, (int)freq_min[CA7]);

	if (input >= (int)freq_max[CA15] || input <= 0) {
		if (pm_qos_request_active(&max_cpu_qos))
			pm_qos_update_request(&max_cpu_qos, freq_max[CA15]);
	} else {
		if (pm_qos_request_active(&max_cpu_qos))
			pm_qos_update_request(&max_cpu_qos, input);
		else
			pm_qos_add_request(&max_cpu_qos, PM_QOS_CPU_FREQ_MAX, input);
	}

	return count;
}

define_one_global_ro(freq_table);
define_one_global_rw(min_freq);
define_one_global_rw(max_freq);

static struct global_attr cpufreq_table =
		__ATTR(cpufreq_table, S_IRUGO, show_freq_table, NULL);
static struct global_attr cpufreq_min_limit =
		__ATTR(cpufreq_min_limit, S_IRUGO | S_IWUSR, show_min_freq, store_min_freq);
static struct global_attr cpufreq_max_limit =
		__ATTR(cpufreq_max_limit, S_IRUGO | S_IWUSR, show_max_freq, store_max_freq);

static struct attribute *iks_attributes[] = {
	&freq_table.attr,
	&min_freq.attr,
	&max_freq.attr,
	NULL
};

static struct attribute_group iks_attr_group = {
	.attrs = iks_attributes,
	.name = "ikcs-cpufreq",
};


/********************* CPUFreq core attributes ********************/

ssize_t show_UV_table_prototype(struct cpufreq_policy *policy, char *buf, bool mv)
{
	cluster_type cur = CA15;
	int i, len = 0;

	for (i = 0; merge_freq_table[i].frequency != CPUFREQ_TABLE_END; i++) {
		if (merge_freq_table[i].frequency != CPUFREQ_ENTRY_INVALID) {
			cur = (merge_freq_table[i].frequency > STEP_LEVEL_CA7_MAX) ?
				CA15 : CA7;

			if (mv) {
				len += sprintf(buf + len, "%dmhz: %d mV\n",
					merge_freq_table[i].frequency / 1000,
					exynos_info[cur]->volt_table[merge_index_table[i]] / 1000);
			} else {
				len += sprintf(buf + len, "%d %d \n", 
					merge_freq_table[i].frequency / 1000,
					exynos_info[cur]->volt_table[merge_index_table[i]]);
			}
		}
        }

	return len;
}

ssize_t store_UV_table_prototype(struct cpufreq_policy *policy, 
				 const char *buf, size_t count, bool mv) {

	int i, tokens, rest, invalid_offset;
	ssize_t tsize = cpufreq_get_table_size(merge_freq_table, CA15);
	cluster_type cur = CA15;
	int t[tsize];

	invalid_offset = 0;

	if ((tokens = read_into((int*)&t, tsize, buf, count)) < 0)
		return -EINVAL;

	mutex_lock(&cpufreq_lock);

	for (i = 0; i < tokens; i++) {
		while (merge_freq_table[i + invalid_offset].frequency == CPUFREQ_ENTRY_INVALID)
			++invalid_offset;

		cur = (merge_freq_table[i + invalid_offset].frequency > STEP_LEVEL_CA7_MAX)
			? CA15 : CA7;

		if (mv)
			t[i] *= 1000;

		if ((rest = t[i] % 6250) != 0)
			t[i] += 6250 - rest;

		if (cur == CA15) {
			sanitize_min_max(t[i], 600000, ARM_MAX_VOLT);
		} else {
			sanitize_min_max(t[i], 600000, KFC_MAX_VOLT);
		}

		exynos_info[cur]->volt_table[merge_index_table[i + invalid_offset]] = t[i];
	}

	mutex_unlock(&cpufreq_lock);

	return count;
}

ssize_t show_UV_uV_table(struct cpufreq_policy *policy, char *buf)
{
	return show_UV_table_prototype(policy, buf, false);
}

ssize_t store_UV_uV_table(struct cpufreq_policy *policy, 
				 const char *buf, size_t count)
{
	return store_UV_table_prototype(policy, buf, count, false);
}

ssize_t show_UV_mV_table(struct cpufreq_policy *policy, char *buf)
{
	return show_UV_table_prototype(policy, buf, true);
}

ssize_t store_UV_mV_table(struct cpufreq_policy *policy, 
				 const char *buf, size_t count)
{
	return store_UV_table_prototype(policy, buf, count, true);
}

/************************** sysfs end ************************/

static int exynos_cpufreq_reboot_notifier_call(struct notifier_block *this,
				   unsigned long code, void *_cmd)
{
	unsigned int freqCA7, freqCA15;
	unsigned int bootfreqCA7, bootfreqCA15;
	int volt;

	mutex_lock(&cpufreq_lock);
	exynos_info[CA7]->blocked = true;
	exynos_info[CA15]->blocked = true;
	mutex_unlock(&cpufreq_lock);

	bootfreqCA7 = VIRT_FREQ(get_boot_freq(CA7), CA7);
	bootfreqCA15 = VIRT_FREQ(get_boot_freq(CA15), CA15);

	freqCA7 = exynos_getspeed_cluster(CA7);
	freqCA15 = exynos_getspeed_cluster(CA15);

	volt = max(get_match_volt(ID_KFC, ACTUAL_FREQ(bootfreqCA7, CA7)),
			get_match_volt(ID_KFC, ACTUAL_FREQ(freqCA7, CA7)));
	volt = get_limit_voltage(volt);

	if (regulator_set_voltage(kfc_regulator, volt, volt))
		goto err;

	if (exynos_info[CA7]->set_ema)
		exynos_info[CA7]->set_ema(volt);

	volt = max(get_match_volt(ID_ARM, ACTUAL_FREQ(bootfreqCA15, CA15)),
			get_match_volt(ID_ARM, ACTUAL_FREQ(freqCA15, CA15)));
	volt = get_limit_voltage(volt);

	if (regulator_set_voltage(arm_regulator, volt, volt))
		goto err;

	if (exynos_info[CA15]->set_ema)
		exynos_info[CA15]->set_ema(volt);

	return NOTIFY_DONE;
err:
	pr_err("%s: failed to set voltage\n", __func__);

	return NOTIFY_BAD;
}

static struct notifier_block exynos_cpufreq_reboot_notifier = {
	.notifier_call = exynos_cpufreq_reboot_notifier_call,
};

static int exynos_min_qos_handler(struct notifier_block *b, unsigned long val, void *v)
{
	int ret;
	unsigned long freq;
	struct cpufreq_policy *policy;
	int cpu = 0;

#ifdef CONFIG_EXYNOS5_DYNAMIC_CPU_HOTPLUG
	mutex_lock(&hotplug_mutex);
#endif

	freq = exynos_getspeed(cpu);
	if (freq >= val)
		goto good;

	policy = cpufreq_cpu_get(cpu);

	if (!policy)
		goto bad;

#if defined(CONFIG_CPU_FREQ_GOV_USERSPACE) || defined(CONFIG_CPU_FREQ_GOV_PERFORMANCE)
	if ((strcmp(policy->governor->name, "userspace") == 0)
			|| strcmp(policy->governor->name, "performance") == 0) {
		cpufreq_cpu_put(policy);
		goto good;
	}
#endif

#ifdef CONFIG_EXYNOS5_DYNAMIC_CPU_HOTPLUG
	if (!cpumask_empty(&out_cpus)) {
		hotplug_out = false;
		__do_hotplug();
	}
#endif
	ret = __cpufreq_driver_target(policy, val, CPUFREQ_RELATION_H);

	cpufreq_cpu_put(policy);

	if (ret < 0)
		goto bad;

good:
#ifdef CONFIG_EXYNOS5_DYNAMIC_CPU_HOTPLUG
	mutex_unlock(&hotplug_mutex);
#endif
	return NOTIFY_OK;
bad:
#ifdef CONFIG_EXYNOS5_DYNAMIC_CPU_HOTPLUG
	mutex_unlock(&hotplug_mutex);
#endif
	return NOTIFY_BAD;
}

static struct notifier_block exynos_min_qos_notifier = {
	.notifier_call = exynos_min_qos_handler,
};

static int exynos_max_qos_handler(struct notifier_block *b, unsigned long val, void *v)
{
	int ret;
	unsigned long freq;
	struct cpufreq_policy *policy;
	int cpu = 0;

#ifdef CONFIG_EXYNOS5_DYNAMIC_CPU_HOTPLUG
	mutex_lock(&hotplug_mutex);
#endif

	freq = exynos_getspeed(cpu);
	if (freq <= val)
		goto good;

	policy = cpufreq_cpu_get(cpu);

	if (!policy)
		goto bad;

#if defined(CONFIG_CPU_FREQ_GOV_USERSPACE) || defined(CONFIG_CPU_FREQ_GOV_PERFORMANCE)
	if ((strcmp(policy->governor->name, "userspace") == 0)
			|| strcmp(policy->governor->name, "performance") == 0) {
		cpufreq_cpu_put(policy);
		goto good;
	}
#endif

#ifdef CONFIG_EXYNOS5_DYNAMIC_CPU_HOTPLUG
	if (!cpumask_empty(&out_cpus)) {
		hotplug_out = false;
		__do_hotplug();
	}
#endif
	ret = __cpufreq_driver_target(policy, val, CPUFREQ_RELATION_H);

	cpufreq_cpu_put(policy);

	if (ret < 0)
		goto bad;

good:
#ifdef CONFIG_EXYNOS5_DYNAMIC_CPU_HOTPLUG
	mutex_unlock(&hotplug_mutex);
#endif
	return NOTIFY_OK;
bad:
#ifdef CONFIG_EXYNOS5_DYNAMIC_CPU_HOTPLUG
	mutex_unlock(&hotplug_mutex);
#endif
	return NOTIFY_BAD;
}

static struct notifier_block exynos_max_qos_notifier = {
	.notifier_call = exynos_max_qos_handler,
};

static int __init exynos_cpufreq_init(void)
{
	int ret = -EINVAL;

	boot_cluster = 0;

	exynos_info[CA7] = kzalloc(sizeof(struct exynos_dvfs_info), GFP_KERNEL);
	if (!exynos_info[CA7]) {
		ret = -ENOMEM;
		goto err_alloc_info_CA7;
	}

	exynos_info[CA15] = kzalloc(sizeof(struct exynos_dvfs_info), GFP_KERNEL);
	if (!exynos_info[CA15]) {
		ret = -ENOMEM;
		goto err_alloc_info_CA15;
	}

	freqs[CA7] = kzalloc(sizeof(struct cpufreq_freqs), GFP_KERNEL);
	if (!freqs[CA7]) {
		ret = -ENOMEM;
		goto err_alloc_freqs_CA7;
	}

	freqs[CA15] = kzalloc(sizeof(struct cpufreq_freqs), GFP_KERNEL);
	if (!freqs[CA15]) {
		ret = -ENOMEM;
		goto err_alloc_freqs_CA15;
	}

	/* Get to boot_cluster_num - 0 for CA7; 1 for CA15 */
	boot_cluster = !(read_cpuid(CPUID_MPIDR) >> 8 & 0xf);
	pr_debug("%s: boot_cluster is %s\n", __func__,
					boot_cluster == CA7 ? "CA7" : "CA15");

	init_cpumask_cluster_set(boot_cluster);

	ret = exynos5410_cpufreq_CA7_init(&exynos_info_CA7);
	if (ret)
		goto err_init_cpufreq;

	ret = exynos5410_cpufreq_CA15_init(&exynos_info_CA15);
	if (ret)
		goto err_init_cpufreq;

	arm_regulator = regulator_get(NULL, "vdd_arm");
	if (IS_ERR(arm_regulator)) {
		pr_err("%s: failed to get resource vdd_arm\n", __func__);
		goto err_vdd_arm;
	}

	kfc_regulator = regulator_get(NULL, "vdd_kfc");
	if (IS_ERR(kfc_regulator)) {
		pr_err("%s:failed to get resource vdd_kfc\n", __func__);
		goto err_vdd_kfc;
	}

	memcpy(exynos_info[CA7], &exynos_info_CA7,
				sizeof(struct exynos_dvfs_info));
	exynos_info[CA7]->regulator = kfc_regulator;

	memcpy(exynos_info[CA15], &exynos_info_CA15,
				sizeof(struct exynos_dvfs_info));
	exynos_info[CA15]->regulator = arm_regulator;

	if (exynos_info[CA7]->set_freq == NULL) {
		pr_err("%s: No set_freq function (ERR)\n", __func__);
		goto err_set_freq;
	}

	freq_max[CA15] = exynos_info[CA15]->
		freq_table[exynos_info[CA15]->max_support_idx].frequency;
	freq_min[CA15] = exynos_info[CA15]->
		freq_table[exynos_info[CA15]->min_support_idx].frequency;
	freq_max[CA7] = VIRT_FREQ(exynos_info[CA7]->
		freq_table[exynos_info[CA7]->max_support_idx].frequency, CA7);
	freq_min[CA7] = VIRT_FREQ(exynos_info[CA7]->
		freq_table[exynos_info[CA7]->min_support_idx].frequency, CA7);

	if (cpufreq_merge_tables() < 0) {
		ret = -ENOMEM;
		goto err_cpufreq;
	}

	set_boot_freq();

	register_pm_notifier(&exynos_cpufreq_nb);
	register_reboot_notifier(&exynos_cpufreq_reboot_notifier);
	exynos_tmu_add_notifier(&exynos_tmu_nb);
	pm_qos_add_notifier(PM_QOS_CPU_FREQ_MIN, &exynos_min_qos_notifier);
	pm_qos_add_notifier(PM_QOS_CPU_FREQ_MAX, &exynos_max_qos_notifier);

	if (cpufreq_register_driver(&exynos_driver)) {
		pr_err("%s: failed to register cpufreq driver\n", __func__);
		goto err_cpufreq;
	}

	pm_qos_add_request(&min_cpu_qos, PM_QOS_CPU_FREQ_MIN, freq_min[CA7]);
	pm_qos_add_request(&max_cpu_qos, PM_QOS_CPU_FREQ_MAX, freq_max[CA15]);

	ret = sysfs_create_group(cpufreq_global_kobject, &iks_attr_group);
	if (ret) {
		pr_err("%s: failed to create iks-cpufreq sysfs interface\n", __func__);
		goto err_cpufreq;
	}

	ret = sysfs_create_file(power_kobj, &cpufreq_table.attr);
	if (ret) {
		pr_err("%s: failed to create cpufreq_table sysfs interface\n", __func__);
		goto err_cpufreq;
	}

	ret = sysfs_create_file(power_kobj, &cpufreq_min_limit.attr);
	if (ret) {
		pr_err("%s: failed to create cpufreq_min_limit sysfs interface\n", __func__);
		goto err_cpufreq;
	}

	ret = sysfs_create_file(power_kobj, &cpufreq_max_limit.attr);
	if (ret) {
		pr_err("%s: failed to create cpufreq_max_limit sysfs interface\n", __func__);
		goto err_cpufreq;
	}

	pm_qos_add_request(&boot_cpu_qos, PM_QOS_CPU_FREQ_MIN, 0);
#ifdef CONFIG_TARGET_LOCALE_KOR
	pm_qos_update_request_timeout(&boot_cpu_qos, 1200000, 40000 * 1000);
#else
	pm_qos_update_request_timeout(&boot_cpu_qos, 800000, 40000 * 1000);
#endif

	exynos_cpufreq_init_done = true;

	return 0;

err_cpufreq:
	unregister_pm_notifier(&exynos_cpufreq_nb);
err_set_freq:
	regulator_put(kfc_regulator);
err_vdd_kfc:
	if (!IS_ERR(kfc_regulator))
		regulator_put(kfc_regulator);
err_vdd_arm:
	if (!IS_ERR(arm_regulator))
		regulator_put(arm_regulator);
err_init_cpufreq:
	kfree(freqs[CA15]);
err_alloc_freqs_CA15:
	kfree(freqs[CA7]);
err_alloc_freqs_CA7:
	kfree(exynos_info[CA15]);
err_alloc_info_CA15:
	kfree(exynos_info[CA7]);
err_alloc_info_CA7:
	pr_err("%s: failed initialization\n", __func__);

	return ret;
}

late_initcall(exynos_cpufreq_init);
