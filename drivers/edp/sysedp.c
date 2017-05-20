/*
 * Copyright (c) 2013-2014, NVIDIA CORPORATION.  All rights reserved.
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

#include <linux/kernel.h>
#include <linux/notifier.h>
#include <linux/sysedp.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/of.h>
#define CREATE_TRACE_POINTS
#include <trace/events/sysedp.h>

#include "sysedp_internal.h"

DEFINE_MUTEX(sysedp_lock);
LIST_HEAD(registered_consumers);
static struct sysedp_platform_data *pdata;
unsigned int avail_budget = 1000000;
int margin;
int min_budget;

void sysedp_set_avail_budget(unsigned int power)
{
	mutex_lock(&sysedp_lock);
	if (avail_budget != power) {
		trace_sysedp_set_avail_budget(avail_budget, power);
		avail_budget = power;
		_sysedp_refresh();
	}
	mutex_unlock(&sysedp_lock);
}

void _sysedp_refresh(void)
{
	struct sysedp_consumer *p;
	int limit; /* Amount of power available when OC=1*/
	int oc_relax; /* Additional power available when OC=0 */
	int pmax_sum = 0; /* sum of peak values (OC=1) */
	int pthres_sum = 0; /* sum of peak values (OC=0) */

	list_for_each_entry(p, &registered_consumers, link) {
		pmax_sum += _cur_oclevel(p);
		pthres_sum += _cur_level(p);
	}

	limit = (int)avail_budget - pmax_sum - margin;
	limit = limit >= min_budget ? limit : min_budget;
	oc_relax = pmax_sum - pthres_sum;
	oc_relax = oc_relax >= 0 ? oc_relax : 0;

	sysedp_set_dynamic_cap((unsigned int)limit, (unsigned int)oc_relax);
}

static struct sysedp_consumer *_sysedp_get_consumer(const char *name)
{
	struct sysedp_consumer *p;
	struct sysedp_consumer *match = NULL;

	list_for_each_entry(p, &registered_consumers, link) {
		if (!strncmp(p->name, name, SYSEDP_NAME_LEN)) {
			match = p;
			break;
		}
	}

	return match;
}


struct sysedp_consumer *sysedp_get_consumer(const char *name)
{
	struct sysedp_consumer *match = NULL;

	mutex_lock(&sysedp_lock);
	match = _sysedp_get_consumer(name);
	mutex_unlock(&sysedp_lock);

	return match;
}

int sysedp_register_consumer(struct sysedp_consumer *consumer)
{
	int r;

	if (!consumer)
		return -EINVAL;

	if (sysedp_get_consumer(consumer->name))
		return -EEXIST;

	r = sysedp_consumer_add_kobject(consumer);
	if (r)
		return r;

	mutex_lock(&sysedp_lock);
	list_add_tail(&consumer->link, &registered_consumers);
	_sysedp_refresh();
	mutex_unlock(&sysedp_lock);
	return 0;
}
EXPORT_SYMBOL(sysedp_register_consumer);

void sysedp_unregister_consumer(struct sysedp_consumer *consumer)
{
	if (!consumer)
		return;

	mutex_lock(&sysedp_lock);
	list_del(&consumer->link);
	_sysedp_refresh();
	mutex_unlock(&sysedp_lock);
	sysedp_consumer_remove_kobject(consumer);
}
EXPORT_SYMBOL(sysedp_unregister_consumer);

void sysedp_free_consumer(struct sysedp_consumer *consumer)
{
	if (consumer) {
		sysedp_unregister_consumer(consumer);
		kfree(consumer);
	}
}
EXPORT_SYMBOL(sysedp_free_consumer);

static struct sysedp_consumer_data *sysedp_find_consumer_data(const char *name)
{
	unsigned int i;
	struct sysedp_consumer_data *match = NULL;

	if (!pdata || !pdata->consumer_data)
		return NULL;

	for (i = 0; i < pdata->consumer_data_size; i++) {
		match = &pdata->consumer_data[i];
		if (!strcmp(match->name, name))
			break;
		match = NULL;
	}
	return match;
}

struct sysedp_consumer *sysedp_create_consumer(const char *specname,
					       const char *consumername)
{
	struct sysedp_consumer *consumer;
	struct sysedp_consumer_data *match;

	if (!pdata || !pdata->consumer_data)
		return NULL;
	match = sysedp_find_consumer_data(specname);
	if (!match) {
		pr_info("sysedp_create_consumer: unable to create %s, no consumer_data for %s found\n",
			consumername, specname);
		return NULL;
	}

	consumer = kzalloc(sizeof(*consumer), GFP_KERNEL);
	if (!consumer)
		return NULL;

	strncpy(consumer->name, consumername, SYSEDP_NAME_LEN-1);
	consumer->name[SYSEDP_NAME_LEN-1] = 0;
	consumer->states = match->states;
	consumer->num_states = match->num_states;

	if (sysedp_register_consumer(consumer)) {
		kfree(consumer);
		return NULL;
	}

	return consumer;
}
EXPORT_SYMBOL(sysedp_create_consumer);

static void _sysedp_set_state(struct sysedp_consumer *consumer,
			      unsigned int new_state)
{
	if (consumer->state != new_state) {
		trace_sysedp_change_state(consumer->name, consumer->state,
					  new_state);
		consumer->state = clamp_t(unsigned int, new_state, 0,
					  consumer->num_states-1);
		_sysedp_refresh();
	}
}

void sysedp_set_state(struct sysedp_consumer *consumer, unsigned int new_state)
{
	if (!consumer)
		return;

	mutex_lock(&sysedp_lock);
	_sysedp_set_state(consumer, new_state);
	mutex_unlock(&sysedp_lock);
}
EXPORT_SYMBOL(sysedp_set_state);

void sysedp_set_state_by_name(const char *name, unsigned int new_state)
{
	struct sysedp_consumer *consumer = NULL;

	if (!name)
		return;

	mutex_lock(&sysedp_lock);
	consumer = _sysedp_get_consumer(name);
	if (consumer)
		_sysedp_set_state(consumer, new_state);
	mutex_unlock(&sysedp_lock);
}
EXPORT_SYMBOL(sysedp_set_state_by_name);

unsigned int sysedp_get_state(struct sysedp_consumer *consumer)
{
	unsigned int state;
	if (!consumer)
		return 0;

	mutex_lock(&sysedp_lock);
	state = consumer->state;
	mutex_unlock(&sysedp_lock);

	return state;
}
EXPORT_SYMBOL(sysedp_get_state);

static int sysedp_get_consumer_data(struct platform_device *pdev,
		struct sysedp_platform_data *obj_ptr, struct device_node *np_consumers)
{
	struct device_node *child;
	int idx = 0;
	int i;
	u32 lenp;
	int n;
	u32 *u32_ptr;
	const char *c_ptr;
	const void *ptr;
	int ret;

	obj_ptr->consumer_data_size = of_get_child_count(np_consumers);
	if (!obj_ptr->consumer_data_size)
		return -EINVAL;

	obj_ptr->consumer_data = devm_kzalloc(&pdev->dev,
		sizeof(struct sysedp_consumer_data) *
		obj_ptr->consumer_data_size, GFP_KERNEL);
	if (!obj_ptr->consumer_data)
		return -EINVAL;

	for_each_child_of_node(np_consumers, child) {
		ptr = of_get_property(child, "nvidia,consumer_name", &lenp);
		if (!ptr)
			return -EINVAL;
		obj_ptr->consumer_data[idx].name = devm_kzalloc(&pdev->dev,
			sizeof(char) * lenp, GFP_KERNEL);
		if (!obj_ptr->consumer_data[idx].name)
			return -EINVAL;
		ret = of_property_read_string(child, "nvidia,consumer_name",
					      &c_ptr);
		if (ret) {
			dev_err(&pdev->dev,
				"The name of consumer %s is not set\n",
				child->name);
			return -EINVAL;
		}
		strncpy(obj_ptr->consumer_data[idx].name, c_ptr, lenp);

		ptr = of_get_property(child, "nvidia,states", &lenp);
		if (!ptr)
			return -EINVAL;
		n = lenp / sizeof(u32);
		if (!n)
			return -EINVAL;
		obj_ptr->consumer_data[idx].states = devm_kzalloc(&pdev->dev,
			sizeof(unsigned int) * n, GFP_KERNEL);
		if (!obj_ptr->consumer_data[idx].states)
			return -EINVAL;
		u32_ptr = kzalloc(sizeof(u32) * n, GFP_KERNEL);
		if (!u32_ptr)
			return -EINVAL;
		ret = of_property_read_u32_array(child, "nvidia,states",
						 u32_ptr, n);
		if (ret) {
			dev_err(&pdev->dev,
				"Fail to read states of consumer %s\n",
				child->name);
			kfree(u32_ptr);
			return -EINVAL;
		}
		for (i = 0; i < n; ++i)
			obj_ptr->consumer_data[idx].states[i] = u32_ptr[i];
		obj_ptr->consumer_data[idx].num_states = n;
		kfree(u32_ptr);

		++idx;
	}

	return 0;
}

static void of_sysedp_get_pdata(struct platform_device *pdev,
		      struct sysedp_platform_data **pdata)
{
	struct device_node *np = pdev->dev.of_node;
	struct device_node *np_consumers;
	struct sysedp_platform_data *obj_ptr;
	u32 val;
	int ret;

	obj_ptr = devm_kzalloc(&pdev->dev, sizeof(struct sysedp_platform_data),
		GFP_KERNEL);
	if (!obj_ptr)
		return;

	np_consumers = of_get_child_by_name(np, "consumers");
	if (np_consumers) {
		ret = sysedp_get_consumer_data(pdev, obj_ptr, np_consumers);
		if (ret) {
			pr_err("sysedp_get_consumer_data: Failed to initialize consumers (%d).\n",
					ret);
			return;
		}
	}

	ret = of_property_read_u32(np, "nvidia,margin", &val);
	if (!ret)
		obj_ptr->margin = (s32)val;

	ret = of_property_read_u32(np, "nvidia,min_budget", &val);
	if (!ret)
		obj_ptr->min_budget = (s32)val;

	ret = of_property_read_u32(np, "nvidia,initial_budget", &val);
	if (!ret)
		avail_budget = (s32)val;

	*pdata = obj_ptr;

	return;
}

static int sysedp_probe(struct platform_device *pdev)
{
	if (pdev->dev.of_node)
		of_sysedp_get_pdata(pdev, &pdata);
	else
		pdata = pdev->dev.platform_data;
	if (!pdata)
		return -EINVAL;

	margin = pdata->margin;
	min_budget = (pdata->min_budget >= 0) ? pdata->min_budget : 0;
	sysedp_init_sysfs();
	sysedp_init_debugfs();
	return 0;
}

static const struct of_device_id sysedp_of_match[] = {
	{ .compatible = "nvidia,tegra124-sysedp", },
	{ },
};
MODULE_DEVICE_TABLE(of, sysedp_of_match);

static struct platform_driver sysedp_driver = {
	.probe = sysedp_probe,
	.driver = {
		.owner = THIS_MODULE,
		.name = "sysedp",
		.of_match_table = sysedp_of_match,
	}
};

static __init int sysedp_init(void)
{
	return platform_driver_register(&sysedp_driver);
}
pure_initcall(sysedp_init);
