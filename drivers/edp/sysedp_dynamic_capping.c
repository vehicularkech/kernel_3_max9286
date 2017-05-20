/*
 * Copyright (c) 2013-2015, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/clk.h>
#include <linux/cpu.h>
#include <linux/debugfs.h>
#include <linux/edp.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_qos.h>
#include <linux/workqueue.h>
#include <linux/platform_data/tegra_edp.h>
#include <linux/debugfs.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <trace/events/sysedp.h>

#include "sysedp_internal.h"

struct sysedpcap {
	unsigned int cpupwr;
	unsigned int gpu;
	unsigned int emc;
};

static unsigned int gpu_high_threshold = 500;
static unsigned int gpu_window = 80;
static unsigned int gpu_high_hist;
static unsigned int gpu_high_count = 2;
static unsigned int priority_bias = 75;
static bool gpu_busy;
static unsigned int fgpu;
static unsigned int avail_power;
static unsigned int avail_oc_relax;
static unsigned int cap_method;

static struct tegra_sysedp_corecap *cur_corecap;
static struct clk *emc_cap_clk;
static struct clk *gpu_cap_clk;
static struct pm_qos_request cpupwr_qos;
static struct pm_qos_request gpupwr_qos;
static unsigned int cpu_power_balance;
static unsigned int force_gpu_pri;
static struct delayed_work capping_work;
static struct tegra_sysedp_platform_data *capping_device_platdata;
static struct sysedpcap core_policy;
static struct sysedpcap forced_caps;
static struct sysedpcap cur_caps;
static DEFINE_MUTEX(core_lock);

static int init_done;

static void pr_caps(struct sysedpcap *old, struct sysedpcap *new)
{
	if (!IS_ENABLED(CONFIG_DEBUG_KERNEL))
		return;

	if ((new->cpupwr == old->cpupwr) &&
	    (new->gpu == old->gpu) &&
	    (new->emc == old->emc))
		return;

	pr_debug("sysedp: gpupri %d, core %5u mW, "
		 "cpu %5u mW, gpu %u %s, emc %u kHz\n",
		 gpu_busy, cur_corecap->power,
		 new->cpupwr, new->gpu,
		 capping_device_platdata->gpu_cap_as_mw ? "mW" : "kHz",
		 new->emc);
}

static void apply_caps(struct tegra_sysedp_devcap *devcap)
{
	struct sysedpcap new;
	int r;
	int do_trace = 0;

	core_policy.cpupwr = devcap->cpu_power + cpu_power_balance;
	core_policy.gpu = devcap->gpu_cap;
	core_policy.emc = devcap->emcfreq;

	new.cpupwr = forced_caps.cpupwr ?: core_policy.cpupwr;
	new.gpu = forced_caps.gpu ?: core_policy.gpu;
	new.emc = forced_caps.emc ?: core_policy.emc;

	if (new.cpupwr != cur_caps.cpupwr) {
		pm_qos_update_request(&cpupwr_qos, new.cpupwr);
		do_trace = 1;
	}

	if (new.emc != cur_caps.emc) {
		r = clk_set_rate(emc_cap_clk, new.emc * 1000);
		WARN_ON(r);
		do_trace = 1;
	}

	if (new.gpu != cur_caps.gpu) {
		if (capping_device_platdata->gpu_cap_as_mw) {
			pm_qos_update_request(&gpupwr_qos, new.gpu);
		} else {
			r = clk_set_rate(gpu_cap_clk, new.gpu * 1000);
			WARN_ON(r && (r != -ENOENT));
		}
		do_trace = 1;
	}

	if (do_trace)
		trace_sysedp_dynamic_capping(new.cpupwr, new.gpu,
					     new.emc, gpu_busy,
					     capping_device_platdata->gpu_cap_as_mw);
	pr_caps(&cur_caps, &new);
	cur_caps = new;
}

static inline bool gpu_priority(void)
{
	bool prefer_gpu = gpu_busy;

	/* NOTE: the policy for selecting between the GPU priority
	 * mode and the CPU priority mode depends on whether GPU
	 * caps are expressed in mW or kHz. The policy is "smarter"
	 * when capping is in terms of kHz. So, if GPU caps are
	 * expressed in mW, it is highly preferred to use supplemental
	 * GPU capping tables expressed in KHz, as well.
	 */
	if ((!capping_device_platdata->gpu_cap_as_mw) ||
		(capping_device_platdata->gpu_cap_as_mw &&
					capping_device_platdata->gpu_supp_freq))
		prefer_gpu = prefer_gpu
			&& (fgpu > ((capping_device_platdata->gpu_supp_freq ?
					cur_corecap->cpupri.gpu_supp_freq :
						cur_corecap->cpupri.gpu_cap)
					  * priority_bias / 100));

	return force_gpu_pri || prefer_gpu;
}

static inline struct tegra_sysedp_devcap *get_devcap(void)
{
	return gpu_priority() ? &cur_corecap->gpupri : &cur_corecap->cpupri;
}

static void __do_cap_control(void)
{
	struct tegra_sysedp_devcap *cap;

	if (!cur_corecap)
		return;

	cap = get_devcap();
	apply_caps(cap);
}

static void do_cap_control(void)
{
	mutex_lock(&core_lock);
	__do_cap_control();
	mutex_unlock(&core_lock);
}

static void update_cur_corecap(void)
{
	struct tegra_sysedp_corecap *cap;
	unsigned int power;
	unsigned int relaxed_power;
	int i;

	if (!capping_device_platdata)
		return;

	power = avail_power * capping_device_platdata->core_gain / 100;

	i = capping_device_platdata->corecap_size - 1;
	cap = capping_device_platdata->corecap + i;

	for (; i >= 0; i--, cap--) {
		switch (cap_method) {
		default:
			pr_warn("%s: Unknown cap_method, %x!  Assuming direct.\n",
					__func__, cap_method);
			cap_method = TEGRA_SYSEDP_CAP_METHOD_DIRECT;
			/* Intentional fall-through*/
		case TEGRA_SYSEDP_CAP_METHOD_DIRECT:
			relaxed_power = 0;
			break;

		case TEGRA_SYSEDP_CAP_METHOD_SIGNAL:
			relaxed_power = min(avail_oc_relax, cap->pthrot);
			break;

		case TEGRA_SYSEDP_CAP_METHOD_RELAX:
			relaxed_power = cap->pthrot;
			break;
		}

		if (cap->power <= power + relaxed_power) {
			cur_corecap = cap;
			cpu_power_balance = power + relaxed_power
				- cap->power;
			return;
		}
	}

	cur_corecap = capping_device_platdata->corecap;
	cpu_power_balance = 0;
}

/* set the available power budget for cpu/gpu/emc (in mW) */
void sysedp_set_dynamic_cap(unsigned int power, unsigned int oc_relax)
{
	if (!init_done)
		return;

	mutex_lock(&core_lock);
	avail_power = power;
	avail_oc_relax = oc_relax;
	update_cur_corecap();
	__do_cap_control();
	mutex_unlock(&core_lock);
}

static void capping_worker(struct work_struct *work)
{
	if (!gpu_busy)
		do_cap_control();
}

/*
 * Return true if load was above threshold for at least
 * gpu_high_count number of notifications
 */
static bool calc_gpu_busy(unsigned int load)
{
	unsigned int mask;

	mask = (1 << gpu_high_count) - 1;

	gpu_high_hist <<= 1;
	if (load >= gpu_high_threshold)
		gpu_high_hist |= 1;

	return (gpu_high_hist & mask) == mask;
}

void tegra_edp_notify_gpu_load(unsigned int load, unsigned int freq_in_hz)
{
	bool old;

	old = gpu_busy;
	gpu_busy = calc_gpu_busy(load);
	fgpu = freq_in_hz / 1000;

	if (gpu_busy == old || force_gpu_pri || !capping_device_platdata)
		return;

	cancel_delayed_work(&capping_work);

	if (gpu_busy)
		do_cap_control();
	else
		schedule_delayed_work(&capping_work,
				msecs_to_jiffies(gpu_window));
}
EXPORT_SYMBOL(tegra_edp_notify_gpu_load);

#ifdef CONFIG_DEBUG_FS
static struct dentry *capping_debugfs_dir;

#define DEFINE_SDC_SIMPLE_ATTR(__name, __var)				     \
static int __name##_set(void *data, u64 val)				     \
{									     \
	if (val != __var) {						     \
		__var = val;						     \
		do_cap_control();					     \
	}								     \
									     \
	return 0;							     \
}									     \
									     \
static int __name##_get(void *data, u64 *val)				     \
{									     \
	*val = __var;							     \
	return 0;							     \
}									     \
									     \
DEFINE_SIMPLE_ATTRIBUTE(__name##_fops, __name##_get, __name##_set, "%lld\n");

DEFINE_SDC_SIMPLE_ATTR(favor_gpu, force_gpu_pri);
DEFINE_SDC_SIMPLE_ATTR(gpu_threshold, gpu_high_threshold);
DEFINE_SDC_SIMPLE_ATTR(force_cpu_power, forced_caps.cpupwr);
DEFINE_SDC_SIMPLE_ATTR(force_gpu, forced_caps.gpu);
DEFINE_SDC_SIMPLE_ATTR(force_emc, forced_caps.emc);
DEFINE_SDC_SIMPLE_ATTR(gpu_window, gpu_window);
DEFINE_SDC_SIMPLE_ATTR(gpu_high_count, gpu_high_count);
DEFINE_SDC_SIMPLE_ATTR(priority_bias, priority_bias);

#define DEFINE_SDC_UPDATE_ATTR(__name, __var)				     \
static int __name##_set(void *data, u64 val)				     \
{									     \
	if (val != __var) {						     \
		__var = val;						     \
		update_cur_corecap();                                        \
		do_cap_control();					     \
	}								     \
									     \
	return 0;							     \
}									     \
									     \
static int __name##_get(void *data, u64 *val)				     \
{									     \
	*val = __var;							     \
	return 0;							     \
}									     \
									     \
DEFINE_SIMPLE_ATTRIBUTE(__name##_fops, __name##_get, __name##_set, "%lld\n");

DEFINE_SDC_UPDATE_ATTR(gain, capping_device_platdata->core_gain);
DEFINE_SDC_UPDATE_ATTR(cap_method, cap_method);

static int corecaps_show(struct seq_file *file, void *data)
{
	int i;
	struct tegra_sysedp_corecap *p;
	struct tegra_sysedp_devcap *c;
	struct tegra_sysedp_devcap *g;
	const char *gpu_label;

	if (!capping_device_platdata || !capping_device_platdata->corecap)
		return -ENODEV;

	gpu_label = (capping_device_platdata->gpu_cap_as_mw
		     ? "GPU-mW"
		     : "GPU-kHz");

	p = capping_device_platdata->corecap;

	seq_printf(file, "%s %s { %s %9s %9s } %s { %s %9s %9s } %7s\n",
		   "E-state",
		   "CPU-pri", "CPU-mW", gpu_label, "EMC-kHz",
		   "GPU-pri", "CPU-mW", gpu_label, "EMC-kHz",
		   "Pthrot");

	for (i = 0; i < capping_device_platdata->corecap_size; i++, p++) {
		c = &p->cpupri;
		g = &p->gpupri;
		seq_printf(file, "%7u %16u %9u %9u %18u %9u %9u %7u\n",
			   p->power,
			   c->cpu_power, c->gpu_cap, c->emcfreq,
			   g->cpu_power, g->gpu_cap, g->emcfreq,
			   p->pthrot);
	}

	return 0;
}

static int corecaps_open(struct inode *inode, struct file *file)
{
	return single_open(file, corecaps_show, inode->i_private);
}

static const struct file_operations corecaps_fops = {
	.open = corecaps_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int status_show(struct seq_file *file, void *data)
{
	mutex_lock(&core_lock);

	seq_printf(file, "gpu priority: %u\n", gpu_priority());
	seq_printf(file, "gain        : %u\n",
		   capping_device_platdata->core_gain);
	seq_printf(file, "core cap    : %u\n", cur_corecap->power);
	seq_printf(file, "max throttle: %u\n", cur_corecap->pthrot);
	seq_printf(file, "cpu balance : %u\n", cpu_power_balance);
	seq_printf(file, "cpu power   : %u\n", get_devcap()->cpu_power +
		   cpu_power_balance);
	seq_printf(file, "gpu cap     : %u %s\n", cur_caps.gpu,
		capping_device_platdata->gpu_cap_as_mw ? "mW" : "kHz");
	seq_printf(file, "emc cap     : %u kHz\n", cur_caps.emc);
	seq_printf(file, "cc method   : %u\n", cap_method);

	mutex_unlock(&core_lock);
	return 0;
}

static int status_open(struct inode *inode, struct file *file)
{
	return single_open(file, status_show, inode->i_private);
}

static const struct file_operations status_fops = {
	.open = status_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

#define SDC_DEBUGFS_CREATE_FILE(__name)	\
	debugfs_create_file(#__name, S_IRUGO | S_IWUSR, d, NULL, &__name##_fops)

static void init_debug(void)
{
	struct dentry *d;
	struct dentry *df;

	if (!sysedp_debugfs_dir)
		return;

	d = debugfs_create_dir("capping", sysedp_debugfs_dir);
	if (IS_ERR_OR_NULL(d)) {
		WARN_ON(1);
		return;
	}

	capping_debugfs_dir = d;

	df = SDC_DEBUGFS_CREATE_FILE(favor_gpu);
	WARN_ON(!df);
	df = SDC_DEBUGFS_CREATE_FILE(gpu_threshold);
	WARN_ON(!df);
	df = SDC_DEBUGFS_CREATE_FILE(force_cpu_power);
	WARN_ON(!df);
	df = SDC_DEBUGFS_CREATE_FILE(force_gpu);
	WARN_ON(!df);
	df = SDC_DEBUGFS_CREATE_FILE(force_emc);
	WARN_ON(!df);
	df = SDC_DEBUGFS_CREATE_FILE(gpu_window);
	WARN_ON(!df);
	df = SDC_DEBUGFS_CREATE_FILE(gpu_high_count);
	WARN_ON(!df);
	df = SDC_DEBUGFS_CREATE_FILE(gain);
	WARN_ON(!df);
	df = SDC_DEBUGFS_CREATE_FILE(cap_method);
	WARN_ON(!df);
	df = SDC_DEBUGFS_CREATE_FILE(corecaps);
	WARN_ON(!df);
	df = SDC_DEBUGFS_CREATE_FILE(priority_bias);
	WARN_ON(!df);
	df = SDC_DEBUGFS_CREATE_FILE(status);
	WARN_ON(!df);
}
#else
static inline void init_debug(void) {}
#endif

static int init_clks(void)
{
	emc_cap_clk = clk_get_sys("battery_edp", "emc");
	if (IS_ERR(emc_cap_clk))
		return -ENODEV;

	if (!capping_device_platdata->gpu_cap_as_mw) {
		gpu_cap_clk = clk_get_sys("battery_edp", "gpu");
		if (IS_ERR(gpu_cap_clk)) {
			clk_put(emc_cap_clk);
			return -ENODEV;
		}
	}

	return 0;
}

static void of_sysedp_dynamic_capping_get_pdata(struct platform_device *pdev,
		struct tegra_sysedp_platform_data **pdata)
{
	struct device_node *np = pdev->dev.of_node;
	struct tegra_sysedp_platform_data *obj_ptr;
	u32 val, lenp;
	int n, i, idx, ret;
	const void *ptr;
	u32 *u32_ptr;

	*pdata = NULL;

	obj_ptr = devm_kzalloc(&pdev->dev,
			       sizeof(struct tegra_sysedp_platform_data),
			       GFP_KERNEL);
	if (!obj_ptr)
		return;

	obj_ptr->gpu_supp_freq =
		!!of_find_property(np, "nvidia,gpu_supp_freq", NULL);

	ptr = of_get_property(np, "nvidia,corecap", &lenp);
	if (!ptr)
		return;

	if (obj_ptr->gpu_supp_freq) {
		if (lenp % (sizeof(u32)*10)) {
			dev_err(&pdev->dev, "sysedp_dynamic_capping: corecap needs to be a multiple of 10 u32s!\n");
			return;
		}
		n = lenp / (sizeof(u32)*10);
	} else {
		if (lenp % (sizeof(u32)*8)) {
			dev_err(&pdev->dev, "sysedp_dynamic_capping: corecap needs to be a multiple of 8 u32s!\n");
			return;
		}
		n = lenp / (sizeof(u32)*8);
	}

	if (!n)
		return;

	obj_ptr->corecap = devm_kzalloc(&pdev->dev,
				sizeof(struct tegra_sysedp_corecap) * n,
				GFP_KERNEL);

	if (!obj_ptr->corecap)
		return;

	u32_ptr = kzalloc(lenp, GFP_KERNEL);
	if (!u32_ptr)
		return;
	ret = of_property_read_u32_array(np, "nvidia,corecap",
					 u32_ptr, (lenp / sizeof(u32)));
	if (ret) {
		dev_err(&pdev->dev,
			"sysedp_dynamic_capping: Fail to read corecap\n");
		kfree(u32_ptr);
		return;
	}

	for (idx = 0, i = 0; idx < n; idx++) {
		obj_ptr->corecap[idx].power            = u32_ptr[i];
		obj_ptr->corecap[idx].cpupri.cpu_power = u32_ptr[i+1];
		obj_ptr->corecap[idx].cpupri.gpu_cap   = u32_ptr[i+2];
		obj_ptr->corecap[idx].cpupri.emcfreq   = u32_ptr[i+3];
		obj_ptr->corecap[idx].gpupri.cpu_power = u32_ptr[i+4];
		obj_ptr->corecap[idx].gpupri.gpu_cap   = u32_ptr[i+5];
		obj_ptr->corecap[idx].gpupri.emcfreq   = u32_ptr[i+6];
		obj_ptr->corecap[idx].pthrot           = u32_ptr[i+7];
		if (obj_ptr->gpu_supp_freq) {
			obj_ptr->corecap[idx].cpupri.gpu_supp_freq =
								u32_ptr[i+8];
			obj_ptr->corecap[idx].gpupri.gpu_supp_freq =
								u32_ptr[i+9];
			i += 10;
		} else
			i += 8;
	}
	obj_ptr->corecap_size = n;
	kfree(u32_ptr);

	ret = of_property_read_u32(np, "nvidia,core_gain", &val);
	if (!ret)
		obj_ptr->core_gain = (unsigned int)val;
	else
		return;

	ret = of_property_read_u32(np, "nvidia,init_req_watts", &val);
	if (!ret)
		obj_ptr->init_req_watts = (unsigned int)val;
	else
		return;

	ret = of_property_read_u32(np, "nvidia,throttle_depth", &val);
	if (!ret) {
		if (val > 100) {
			dev_err(&pdev->dev,
			    "sysedp_dynamic_capping: throttle_depth > 100\n");
			return;
		}
		obj_ptr->pthrot_ratio = (unsigned int)val;
	} else
		return;

	ret = of_property_read_u32(np, "nvidia,cap_method", &val);
	if (!ret)
		obj_ptr->cap_method = (unsigned int)val;
	else
		return;

	obj_ptr->gpu_cap_as_mw =
		!!of_find_property(np, "nvidia,gpu_cap_as_mw", NULL);

	*pdata = obj_ptr;

	return;
}


static int sysedp_dynamic_capping_probe(struct platform_device *pdev)
{
	int r;
	struct tegra_sysedp_corecap *cap;
	int i;

	/* only one instance is allowed */
	if (capping_device_platdata != NULL) {
		WARN_ON(capping_device_platdata != NULL);
		return -EINVAL;
	}

	if (pdev->dev.of_node)
		of_sysedp_dynamic_capping_get_pdata(pdev,
						    &capping_device_platdata);
	else
		capping_device_platdata = pdev->dev.platform_data;

	if (!capping_device_platdata)
		return -EINVAL;

	INIT_DELAYED_WORK(&capping_work, capping_worker);
	pm_qos_add_request(&cpupwr_qos, PM_QOS_MAX_CPU_POWER,
			   PM_QOS_CPU_POWER_MAX_DEFAULT_VALUE);

	if (capping_device_platdata->gpu_cap_as_mw)
		pm_qos_add_request(&gpupwr_qos, PM_QOS_MAX_GPU_POWER,
				   PM_QOS_GPU_POWER_MAX_DEFAULT_VALUE);

	r = init_clks();
	if (r)
		return r;


	mutex_lock(&core_lock);
	avail_power = capping_device_platdata->init_req_watts;
	cap_method = capping_device_platdata->cap_method;
	switch (cap_method) {
	case TEGRA_SYSEDP_CAP_METHOD_DEFAULT:
		cap_method = TEGRA_SYSEDP_CAP_METHOD_SIGNAL;
		break;
	case TEGRA_SYSEDP_CAP_METHOD_DIRECT:
	case TEGRA_SYSEDP_CAP_METHOD_SIGNAL:
	case TEGRA_SYSEDP_CAP_METHOD_RELAX:
		break;
	default:
		pr_warn("%s: Unknown cap_method, %x!  Assuming direct.\n",
				__func__, cap_method);
		cap_method = TEGRA_SYSEDP_CAP_METHOD_DIRECT;
		break;
	}

	/* scale pthrot value in capping table */
	i = capping_device_platdata->corecap_size - 1;
	cap = capping_device_platdata->corecap + i;
	for (; i >= 0; i--, cap--) {
		cap->pthrot *= capping_device_platdata->pthrot_ratio;
		cap->pthrot /= 100;
	}
	update_cur_corecap();
	__do_cap_control();
	mutex_unlock(&core_lock);

	init_debug();

	init_done = 1;
	return 0;
}

static const struct of_device_id sysedp_dynamic_capping_of_match[] = {
	{ .compatible = "nvidia,tegra124-sysedp-dynamic-capping", },
	{ },
};
MODULE_DEVICE_TABLE(of, sysedp_dynamic_capping_of_match);

static struct platform_driver sysedp_dynamic_capping_driver = {
	.probe = sysedp_dynamic_capping_probe,
	.driver = {
		.owner = THIS_MODULE,
		.name = "sysedp_dynamic_capping",
		.of_match_table = sysedp_dynamic_capping_of_match,
	}
};

static __init int sysedp_dynamic_capping_init(void)
{
	return platform_driver_register(&sysedp_dynamic_capping_driver);
}
late_initcall(sysedp_dynamic_capping_init);
