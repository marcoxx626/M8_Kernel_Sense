/*
 * PPC 64 oprofile support:
 * Copyright (C) 2004 Anton Blanchard <anton@au.ibm.com>, IBM
 * PPC 32 oprofile support: (based on PPC 64 support)
 * Copyright (C) Freescale Semiconductor, Inc 2004
 *	Author: Andy Fleming
 *
 * Based on alpha version.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/oprofile.h>
#include <linux/init.h>
#include <linux/smp.h>
#include <linux/errno.h>
#include <asm/ptrace.h>
#include <asm/pmc.h>
#include <asm/cputable.h>
#include <asm/oprofile_impl.h>
#include <asm/firmware.h>

static struct op_powerpc_model *model;

static struct op_counter_config ctr[OP_MAX_COUNTER];
static struct op_system_config sys;

static int op_per_cpu_rc;

static void op_handle_interrupt(struct pt_regs *regs)
{
	model->handle_interrupt(regs, ctr);
}

static void op_powerpc_cpu_setup(void *dummy)
{
	int ret;

	ret = model->cpu_setup(ctr);

	if (ret != 0)
		op_per_cpu_rc = ret;
}

static int op_powerpc_setup(void)
{
	int err;

	op_per_cpu_rc = 0;

	
	err = reserve_pmc_hardware(op_handle_interrupt);
	if (err)
		return err;

	
	op_per_cpu_rc = model->reg_setup(ctr, &sys, model->num_counters);

	if (op_per_cpu_rc)
		goto out;

	on_each_cpu(op_powerpc_cpu_setup, NULL, 1);

out:	if (op_per_cpu_rc) {
		
		release_pmc_hardware();
	}

	return op_per_cpu_rc;
}

static void op_powerpc_shutdown(void)
{
	release_pmc_hardware();
}

static void op_powerpc_cpu_start(void *dummy)
{
	int ret;

	ret = model->start(ctr);
	if (ret != 0)
		op_per_cpu_rc = ret;
}

static int op_powerpc_start(void)
{
	op_per_cpu_rc = 0;

	if (model->global_start)
		return model->global_start(ctr);
	if (model->start) {
		on_each_cpu(op_powerpc_cpu_start, NULL, 1);
		return op_per_cpu_rc;
	}
	return -EIO; 
}

static inline void op_powerpc_cpu_stop(void *dummy)
{
	model->stop();
}

static void op_powerpc_stop(void)
{
	if (model->stop)
		on_each_cpu(op_powerpc_cpu_stop, NULL, 1);
        if (model->global_stop)
                model->global_stop();
}

static int op_powerpc_create_files(struct super_block *sb, struct dentry *root)
{
	int i;

#ifdef CONFIG_PPC64
	oprofilefs_create_ulong(sb, root, "mmcr0", &sys.mmcr0);
	oprofilefs_create_ulong(sb, root, "mmcr1", &sys.mmcr1);
	oprofilefs_create_ulong(sb, root, "mmcra", &sys.mmcra);
#ifdef CONFIG_OPROFILE_CELL
	oprofilefs_create_ulong(sb, root, "cell_support", &sys.cell_support);
	sys.cell_support = 0x1; 
#endif
#endif

	for (i = 0; i < model->num_counters; ++i) {
		struct dentry *dir;
		char buf[4];

		snprintf(buf, sizeof buf, "%d", i);
		dir = oprofilefs_mkdir(sb, root, buf);

		oprofilefs_create_ulong(sb, dir, "enabled", &ctr[i].enabled);
		oprofilefs_create_ulong(sb, dir, "event", &ctr[i].event);
		oprofilefs_create_ulong(sb, dir, "count", &ctr[i].count);

		oprofilefs_create_ulong(sb, dir, "kernel", &ctr[i].kernel);
		oprofilefs_create_ulong(sb, dir, "user", &ctr[i].user);

		oprofilefs_create_ulong(sb, dir, "unit_mask", &ctr[i].unit_mask);
	}

	oprofilefs_create_ulong(sb, root, "enable_kernel", &sys.enable_kernel);
	oprofilefs_create_ulong(sb, root, "enable_user", &sys.enable_user);

	
	sys.enable_kernel = 1;
	sys.enable_user = 1;

	return 0;
}

int __init oprofile_arch_init(struct oprofile_operations *ops)
{
	if (!cur_cpu_spec->oprofile_cpu_type)
		return -ENODEV;

	switch (cur_cpu_spec->oprofile_type) {
#ifdef CONFIG_PPC_BOOK3S_64
#ifdef CONFIG_OPROFILE_CELL
		case PPC_OPROFILE_CELL:
			if (firmware_has_feature(FW_FEATURE_LPAR))
				return -ENODEV;
			model = &op_model_cell;
			ops->sync_start = model->sync_start;
			ops->sync_stop = model->sync_stop;
			break;
#endif
		case PPC_OPROFILE_RS64:
			model = &op_model_rs64;
			break;
		case PPC_OPROFILE_POWER4:
			model = &op_model_power4;
			break;
		case PPC_OPROFILE_PA6T:
			model = &op_model_pa6t;
			break;
#endif
#ifdef CONFIG_6xx
		case PPC_OPROFILE_G4:
			model = &op_model_7450;
			break;
#endif
#if defined(CONFIG_FSL_EMB_PERFMON)
		case PPC_OPROFILE_FSL_EMB:
			model = &op_model_fsl_emb;
			break;
#endif
		default:
			return -ENODEV;
	}

	model->num_counters = cur_cpu_spec->num_pmcs;

	ops->cpu_type = cur_cpu_spec->oprofile_cpu_type;
	ops->create_files = op_powerpc_create_files;
	ops->setup = op_powerpc_setup;
	ops->shutdown = op_powerpc_shutdown;
	ops->start = op_powerpc_start;
	ops->stop = op_powerpc_stop;
	ops->backtrace = op_powerpc_backtrace;

	printk(KERN_DEBUG "oprofile: using %s performance monitoring.\n",
	       ops->cpu_type);

	return 0;
}

void oprofile_arch_exit(void)
{
}
