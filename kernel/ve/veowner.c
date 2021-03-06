/*
 *  kernel/ve/veowner.c
 *
 *  Copyright (C) 2000-2005  SWsoft
 *  All rights reserved.
 *  
 *  Licensing governed by "linux/COPYING.SWsoft" file.
 *
 */

#include <linux/sched.h>
#include <linux/ve.h>
#include <linux/ve_proto.h>
#include <linux/ipc.h>
#include <linux/fs_struct.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/delay.h>
#include <linux/vmalloc.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/list.h>
#include <linux/inetdevice.h>
#include <linux/pid_namespace.h>
#include <asm/system.h>
#include <asm/io.h>

#include <net/tcp.h>

void prepare_ve0_process(struct task_struct *tsk)
{
	VE_TASK_INFO(tsk)->exec_env = get_ve0();
	VE_TASK_INFO(tsk)->owner_env = get_ve0();
	VE_TASK_INFO(tsk)->sleep_time = 0;
	VE_TASK_INFO(tsk)->wakeup_stamp = 0;
	VE_TASK_INFO(tsk)->sched_time = 0;
	seqcount_init(&VE_TASK_INFO(tsk)->wakeup_lock);

	if (tsk->pid) {
		list_add_rcu(&tsk->ve_task_info.vetask_list,
				&get_ve0()->vetask_lh);
		atomic_inc(&get_ve0()->pcounter);
	}
}

/*
 * ------------------------------------------------------------------------
 * proc entries
 * ------------------------------------------------------------------------
 */

#ifdef CONFIG_PROC_FS
struct proc_dir_entry *proc_vz_dir;
EXPORT_SYMBOL(proc_vz_dir);

struct proc_dir_entry *glob_proc_vz_dir;
EXPORT_SYMBOL(glob_proc_vz_dir);

static void prepare_proc(void)
{
	proc_vz_dir = proc_mkdir("vz", NULL);
	if (!proc_vz_dir)
		panic("Can't create /proc/vz dir\n");

	glob_proc_vz_dir = proc_mkdir("vz", &glob_proc_root);
	if (!proc_vz_dir)
		panic("Can't create /proc/vz dir\n");
}
#endif

/*
 * ------------------------------------------------------------------------
 * OpenVZ sysctl
 * ------------------------------------------------------------------------
 */
extern int ve_area_access_check;

#ifdef CONFIG_INET
static struct ctl_table vz_ipv4_route_table[] = {
	{
		.procname	= "src_check",
		.data		= &ip_rt_src_check,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec,
	},
	{ 0 }
};

static struct ctl_path net_ipv4_route_path[] = {
	{ .ctl_name = CTL_NET, .procname = "net", },
	{ .ctl_name = NET_IPV4, .procname = "ipv4", },
	{ .ctl_name = NET_IPV4_ROUTE, .procname = "route", },
	{ }
};
#endif

static struct ctl_table vz_fs_table[] = {
	{
		.procname	= "ve-area-access-check",
		.data		= &ve_area_access_check,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec,
	},
	{ 0 }
};

static struct ctl_path fs_path[] = {
	{ .ctl_name = CTL_FS, .procname = "fs", },
	{ }
};

static void prepare_sysctl(void)
{
#ifdef CONFIG_INET
	register_sysctl_paths(net_ipv4_route_path, vz_ipv4_route_table);
#endif
	register_sysctl_paths(fs_path, vz_fs_table);
}

/*
 * ------------------------------------------------------------------------
 * XXX init_ve_system
 * ------------------------------------------------------------------------
 */

void init_ve_system(void)
{
	struct task_struct *init_entry;
	struct ve_struct *ve;

	ve = get_ve0();

	init_entry = init_pid_ns.child_reaper;
	/* if ve_move_task to VE0 (e.g. in cpt code)	*
	 * occurs, ve_cap_bset on VE0 is required	*/
	ve->ve_cap_bset = CAP_INIT_EFF_SET;

	read_lock(&init_entry->fs->lock);
	ve->root_path = init_entry->fs->root;
	read_unlock(&init_entry->fs->lock);

#ifdef CONFIG_PROC_FS
	prepare_proc();
#endif
	prepare_sysctl();
}
