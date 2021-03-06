/*
 *
 *  kernel/cpt/rst_process.c
 *
 *  Copyright (C) 2000-2005  SWsoft
 *  All rights reserved.
 *
 *  Licensing governed by "linux/COPYING.SWsoft" file.
 *
 */

#include <linux/version.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/posix-timers.h>
#include <linux/slab.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/virtinfo.h>
#include <linux/virtinfoscp.h>
#include <linux/errno.h>
#include <linux/pagemap.h>
#include <linux/ptrace.h>
#include <linux/tty.h>
#include <linux/nsproxy.h>
#include <linux/securebits.h>
#ifdef CONFIG_X86
#include <asm/desc.h>
#endif
#include <asm/unistd.h>

#include <bc/beancounter.h>
#include <bc/misc.h>

#include "cpt_obj.h"
#include "cpt_context.h"
#include "cpt_files.h"
#include "cpt_mm.h"
#include "cpt_ubc.h"
#include "cpt_process.h"
#include "cpt_kernel.h"


#define HOOK_RESERVE	256

struct resume_info
{
	asmlinkage void (*hook)(struct resume_info *);
	unsigned long	hooks;
#define HOOK_TID	0
#define HOOK_CONT	1
#define HOOK_LSI	2
#define HOOK_RESTART	3
	unsigned long	tid_ptrs[2];
	siginfo_t	last_siginfo;
};

#ifdef CONFIG_X86_32

#define IN_SYSCALL(regs)	((long)(regs)->orig_ax >= 0)
#define IN_ERROR(regs)		((long)(regs)->ax < 0)
#define SYSCALL_ERRNO(regs)	(-(long)((regs)->ax))
#define SYSCALL_RETVAL(regs)	((regs)->ax)
#define SYSCALL_NR(regs)	((regs)->orig_ax)

#define SYSCALL_SETRET(regs,val)	do { (regs)->ax = (val); } while (0)

#define SYSCALL_RESTART2(regs,new)	do { (regs)->ax = (new); \
					     (regs)->ip -= 2; } while (0) 

#define syscall_is(tsk,regs,name)	(SYSCALL_NR(regs) == __NR_##name)

/* In new kernels task_pt_regs() is define to something inappropriate */
#undef task_pt_regs
#define task_pt_regs(t) ((struct pt_regs *)((t)->thread.sp0) - 1)

#elif defined(CONFIG_X86_64)

#define IN_SYSCALL(regs)	((long)(regs)->orig_ax >= 0)
#define IN_ERROR(regs)		((long)(regs)->ax < 0)
#define SYSCALL_ERRNO(regs)	(-(long)((regs)->ax))
#define SYSCALL_RETVAL(regs)	((regs)->ax)
#define SYSCALL_NR(regs)	((regs)->orig_ax)

#define SYSCALL_SETRET(regs,val)	do { (regs)->ax = (val); } while (0)

#define SYSCALL_RESTART2(regs,new)	do { (regs)->ax = (new); \
					     (regs)->ip -= 2; } while (0) 

#define __NR32_restart_syscall	0
#define __NR32_rt_sigtimedwait	177
#define __NR32_pause		29
#define __NR32_futex		240

#define syscall_is(tsk,regs,name) ((!(task_thread_info(tsk)->flags&_TIF_IA32) && \
				    SYSCALL_NR(regs) == __NR_##name) || \
				   ((task_thread_info(tsk)->flags&_TIF_IA32) && \
				    SYSCALL_NR(regs) == __NR32_##name))

#elif defined (CONFIG_IA64)

#define IN_SYSCALL(regs)	((long)(regs)->cr_ifs >= 0)
#define IN_ERROR(regs)		((long)(regs)->r10 == -1)
#define SYSCALL_ERRNO(regs)	((regs)->r10 == -1 ? (long)((regs)->r8) : 0)
#define SYSCALL_RETVAL(regs)	((regs)->r8)
#define SYSCALL_NR(regs)	((regs)->cr_ifs >= 0 ? (regs)->r15 : -1)

#define SYSCALL_SETRET(regs,val)	do { (regs)->r8 = (val); } while (0)

#define SYSCALL_RESTART2(regs,new)	do { (regs)->r15 = (new); \
					     (regs)->r10 = 0; \
					     ia64_decrement_ip(regs); } while (0) 

#define syscall_is(tsk,regs,name)	(SYSCALL_NR(regs) == __NR_##name)

#else

#error This arch is not supported

#endif

#define SYSCALL_RESTART(regs) SYSCALL_RESTART2(regs, SYSCALL_NR(regs))

pid_t vpid_to_pid(pid_t nr)
{
	pid_t vnr;
	struct pid *pid;

	rcu_read_lock();
	pid = find_vpid(nr);
	vnr = (pid == NULL ? -1 : pid->numbers[0].nr);
	rcu_read_unlock();
	return vnr;
}

static void decode_siginfo(siginfo_t *info, struct cpt_siginfo_image *si)
{
	memset(info, 0, sizeof(*info));
	switch(si->cpt_code & __SI_MASK) {
	case __SI_TIMER:
		info->si_tid = si->cpt_pid;
		info->si_overrun = si->cpt_uid;
		info->_sifields._timer._sigval.sival_ptr = cpt_ptr_import(si->cpt_sigval);
		info->si_sys_private = si->cpt_utime;
		break;
	case __SI_POLL:
		info->si_band = si->cpt_pid;
		info->si_fd = si->cpt_uid;
		break;
	case __SI_FAULT:
		info->si_addr = cpt_ptr_import(si->cpt_sigval);
#ifdef __ARCH_SI_TRAPNO
		info->si_trapno = si->cpt_pid;
#endif
		break;
	case __SI_CHLD:
		info->si_pid = si->cpt_pid;
		info->si_uid = si->cpt_uid;
		info->si_status = si->cpt_sigval;
		info->si_stime = si->cpt_stime;
		info->si_utime = si->cpt_utime;
		break;
	case __SI_KILL:
	case __SI_RT:
	case __SI_MESGQ:
	default:
		info->si_pid = si->cpt_pid;
		info->si_uid = si->cpt_uid;
		info->si_ptr = cpt_ptr_import(si->cpt_sigval);
		break;
	}
	info->si_signo = si->cpt_signo;
	info->si_errno = si->cpt_errno;
	info->si_code = si->cpt_code;
}

static int restore_sigqueue(struct task_struct *tsk,
			    struct sigpending *queue, unsigned long start,
			    unsigned long end)
{
	while (start < end) {
		struct cpt_siginfo_image *si = (struct cpt_siginfo_image *)start;
		if (si->cpt_object == CPT_OBJ_SIGINFO) {
			struct sigqueue *q = NULL;
			struct user_struct *up;

			up = alloc_uid(get_exec_env()->user_ns, si->cpt_user);
			if (!up)
				return -ENOMEM;
			q = kmem_cache_alloc(sigqueue_cachep, GFP_ATOMIC);
			if (!q) {
				free_uid(up);
				return -ENOMEM;
			}
			if (ub_siginfo_charge(q, get_exec_ub())) {
				kmem_cache_free(sigqueue_cachep, q);
				free_uid(up);
				return -ENOMEM;
			}

			INIT_LIST_HEAD(&q->list);
			/* Preallocated elements (posix timers) are not
			 * supported yet. It is safe to replace them with
			 * a private one. */
			q->flags = 0;
			q->user = up;
			atomic_inc(&q->user->sigpending);

			decode_siginfo(&q->info, si);
			list_add_tail(&q->list, &queue->list);
		}
		start += si->cpt_next;
	}
	return 0;
}

int rst_process_linkage(cpt_context_t *ctx)
{
	cpt_object_t *obj;

	for_each_object(obj, CPT_OBJ_TASK) {
		struct task_struct *tsk = obj->o_obj;
		struct cpt_task_image *ti = obj->o_image;

		if (tsk == NULL) {
			eprintk_ctx("task %u(%s) is missing\n", ti->cpt_pid, ti->cpt_comm);
			return -EINVAL;
		}

		if (task_pgrp_vnr(tsk) != ti->cpt_pgrp) {
			struct pid *pid;

			rcu_read_lock();
			pid = alloc_vpid_safe(ti->cpt_pgrp);
			if (!pid) {
				eprintk_ctx("illegal PGRP " CPT_FID "\n", CPT_TID(tsk));
				return -EINVAL;
			}

			write_lock_irq(&tasklist_lock);
			detach_pid(tsk, PIDTYPE_PGID);
			if (thread_group_leader(tsk))
				attach_pid(tsk, PIDTYPE_PGID, pid);
			write_unlock_irq(&tasklist_lock);

			if (task_pgrp_vnr(tsk) != pid_vnr(pid)) {
				eprintk_ctx("cannot set PGRP " CPT_FID "\n", CPT_TID(tsk));
				return -EINVAL;
			}
			rcu_read_unlock();
		}
		if (task_session_vnr(tsk) != ti->cpt_session) {
			struct pid *pid;

			rcu_read_lock();
			pid = alloc_vpid_safe(ti->cpt_session);
			if (!pid) {
				eprintk_ctx("illegal SID " CPT_FID "\n", CPT_TID(tsk));
				return -EINVAL;
			}

			write_lock_irq(&tasklist_lock);
			detach_pid(tsk, PIDTYPE_SID);
			if (thread_group_leader(tsk))
				attach_pid(tsk, PIDTYPE_SID, pid);
			write_unlock_irq(&tasklist_lock);

			if (task_session_vnr(tsk) != pid_vnr(pid)) {
				eprintk_ctx("cannot set SID " CPT_FID "\n", CPT_TID(tsk));
				return -EINVAL;
			}
			rcu_read_unlock();
		}
		if (ti->cpt_old_pgrp > 0 && !tsk->signal->tty_old_pgrp) {
			struct pid *pid;

			rcu_read_lock();
			pid = get_pid(find_vpid(ti->cpt_old_pgrp));
			if (!pid) {
				eprintk_ctx("illegal OLD_PGRP " CPT_FID "\n", CPT_TID(tsk));
				return -EINVAL;
			}
			tsk->signal->tty_old_pgrp = pid;
			rcu_read_unlock();
		}
	}

	return 0;
}

struct pid *alloc_vpid_safe(pid_t vnr)
{
	struct pid *pid;

	pid = alloc_pid(current->nsproxy->pid_ns, vnr);
	if (!pid)
		pid = find_vpid(vnr);
	return pid;
}

static int
restore_one_signal_struct(struct cpt_task_image *ti, int *exiting, cpt_context_t *ctx)
{
	int err;
	struct cpt_signal_image *si = cpt_get_buf(ctx);

	current->signal->tty = NULL;

	err = rst_get_object(CPT_OBJ_SIGNAL_STRUCT, ti->cpt_signal, si, ctx);
	if (err) {
		cpt_release_buf(ctx);
		return err;
	}

#if 0 /* this should have been restored in rst_process_linkage */
	if (task_pgrp_vnr(current) != si->cpt_pgrp) {
		struct pid * pid = NULL, *free = NULL;

		rcu_read_lock();
		if (si->cpt_pgrp_type == CPT_PGRP_ORPHAN) {
#if 0
			if (!is_virtual_pid(si->cpt_pgrp)) {
				eprintk_ctx("external process group " CPT_FID, CPT_TID(current));
				cpt_release_buf(ctx);
				return -EINVAL;
			}
#endif
			pid = alloc_vpid_safe(si->cpt_pgrp);
			free = pid;
		}
		write_lock_irq(&tasklist_lock);
		if (pid != NULL) {
			if (task_pgrp_nr(current) != pid_nr(pid)) {
				detach_pid(current, PIDTYPE_PGID);
				if (thread_group_leader(current)) {
					attach_pid(current, PIDTYPE_PGID, pid);
					free = NULL;
				}
			}
		}
		write_unlock_irq(&tasklist_lock);
		if (free != NULL)
			free_pid(free);
		rcu_read_unlock();
	}
#endif

	current->signal->tty_old_pgrp = NULL;
	if ((int)si->cpt_old_pgrp > 0) {
		if (si->cpt_old_pgrp_type == CPT_PGRP_STRAY) {
			current->signal->tty_old_pgrp =
					alloc_pid(current->nsproxy->pid_ns, 0);
			if (!current->signal->tty_old_pgrp) {
				eprintk_ctx("failed to allocate stray tty_old_pgrp\n");
				cpt_release_buf(ctx);
				return -EINVAL;
			}
		} else {
			rcu_read_lock();
			current->signal->tty_old_pgrp =
				get_pid(alloc_vpid_safe(si->cpt_old_pgrp));
			rcu_read_unlock();
			if (!current->signal->tty_old_pgrp) {
				dprintk_ctx("forward old tty PGID\n");
				current->signal->tty_old_pgrp = NULL;
			}
		}
	}

#if 0 /* this should have been restored in rst_process_linkage */
	if (task_session_vnr(current) != si->cpt_session) {
		struct pid * pid = NULL, *free = NULL;

		rcu_read_lock();
		if (si->cpt_session_type == CPT_PGRP_ORPHAN) {
#if 0
			if (!is_virtual_pid(si->cpt_session)) {
				eprintk_ctx("external process session " CPT_FID, CPT_TID(current));
				cpt_release_buf(ctx);
				return -EINVAL;
			}
#endif
			pid = alloc_vpid_safe(si->cpt_session);
			free = pid;
		}
		write_lock_irq(&tasklist_lock);
		if (pid == NULL)
			pid = find_vpid(si->cpt_session);
		if (pid != NULL) {
			if (task_session_nr(current) != pid_nr(pid)) {
				detach_pid(current, PIDTYPE_SID);
				set_task_session(current, pid_nr(pid));
				if (thread_group_leader(current)) {
					attach_pid(current, PIDTYPE_SID, pid);
					free = NULL;
				}
			}
		}
		write_unlock_irq(&tasklist_lock);
		if (free != NULL)
			free_pid(free);
		rcu_read_unlock();
	}
#endif

	cpt_sigset_import(&current->signal->shared_pending.signal, si->cpt_sigpending);
	current->signal->leader = si->cpt_leader;
	if (si->cpt_ctty != CPT_NULL) {
		cpt_object_t *obj = lookup_cpt_obj_bypos(CPT_OBJ_TTY, si->cpt_ctty, ctx);
		if (obj) {
			struct tty_struct *tty = obj->o_obj;
			if (!tty->session || tty->session ==
					task_session(current)) {
				tty->session = task_session(current);
				current->signal->tty = tty;
			} else {
				wprintk_ctx("tty session mismatch\n");
			}
		}
	}

	if (si->cpt_curr_target)
		current->signal->curr_target = find_task_by_vpid(si->cpt_curr_target);
	current->signal->flags = 0;
	*exiting = si->cpt_group_exit;
	current->signal->group_exit_code = si->cpt_group_exit_code;
	if (si->cpt_group_exit_task) {
		current->signal->group_exit_task = find_task_by_vpid(si->cpt_group_exit_task);
		if (current->signal->group_exit_task == NULL) {
			eprintk_ctx("oops, group_exit_task=NULL, pid=%u\n", si->cpt_group_exit_task);
			cpt_release_buf(ctx);
			return -EINVAL;
		}
	}
	current->signal->notify_count = si->cpt_notify_count;
	current->signal->group_stop_count = si->cpt_group_stop_count;

	if (si->cpt_next > si->cpt_hdrlen) {
		char *buf = kmalloc(si->cpt_next - si->cpt_hdrlen, GFP_KERNEL);
		if (buf == NULL) {
			cpt_release_buf(ctx);
			return -ENOMEM;
		}
		err = ctx->pread(buf, si->cpt_next - si->cpt_hdrlen, ctx,
				 ti->cpt_signal + si->cpt_hdrlen);
		if (err) {
			kfree(buf);
			cpt_release_buf(ctx);
			return err;
		}
		restore_sigqueue(current,
				 &current->signal->shared_pending, (unsigned long)buf,
				 (unsigned long)buf + si->cpt_next - si->cpt_hdrlen);
		kfree(buf);
	}
	cpt_release_buf(ctx);
	return 0;
}

int restore_one_sighand_struct(struct cpt_task_image *ti, struct cpt_context *ctx)
{
	int err;
	struct cpt_sighand_image si;
	int i;
	loff_t pos, endpos;
	
	err = rst_get_object(CPT_OBJ_SIGHAND_STRUCT, ti->cpt_sighand, &si, ctx);
	if (err)
		return err;

	for (i=0; i<_NSIG; i++) {
		current->sighand->action[i].sa.sa_handler = SIG_DFL;
#ifndef CONFIG_IA64
		current->sighand->action[i].sa.sa_restorer = 0;
#endif
		current->sighand->action[i].sa.sa_flags = 0;
		memset(&current->sighand->action[i].sa.sa_mask, 0, sizeof(sigset_t));
	}

	pos = ti->cpt_sighand + si.cpt_hdrlen;
	endpos = ti->cpt_sighand + si.cpt_next;
	while (pos < endpos) {
		struct cpt_sighandler_image shi;

		err = rst_get_object(CPT_OBJ_SIGHANDLER, pos, &shi, ctx);
		if (err)
			return err;
		current->sighand->action[shi.cpt_signo].sa.sa_handler = (void*)(unsigned long)shi.cpt_handler;
#ifndef CONFIG_IA64
		current->sighand->action[shi.cpt_signo].sa.sa_restorer = (void*)(unsigned long)shi.cpt_restorer;
#endif
		current->sighand->action[shi.cpt_signo].sa.sa_flags = shi.cpt_flags;
		cpt_sigset_import(&current->sighand->action[shi.cpt_signo].sa.sa_mask, shi.cpt_mask);
		pos += shi.cpt_next;
	}

	return 0;
}


__u32 rst_signal_flag(struct cpt_task_image *ti, struct cpt_context *ctx)
{
	__u32 flag = 0;

	if (lookup_cpt_obj_bypos(CPT_OBJ_SIGNAL_STRUCT, ti->cpt_signal, ctx))
		flag |= CLONE_THREAD;
	if (ti->cpt_sighand == CPT_NULL ||
	    lookup_cpt_obj_bypos(CPT_OBJ_SIGHAND_STRUCT, ti->cpt_sighand, ctx))
		flag |= CLONE_SIGHAND;
	return flag;
}

int
rst_signal_complete(struct cpt_task_image *ti, int * exiting, cpt_context_t *ctx)
{
	int err;
	cpt_object_t *obj;

	if (ti->cpt_signal == CPT_NULL || ti->cpt_sighand == CPT_NULL) {
		return -EINVAL;
	}

	obj = lookup_cpt_obj_bypos(CPT_OBJ_SIGHAND_STRUCT, ti->cpt_sighand, ctx);
	if (obj) {
		struct sighand_struct *sig = current->sighand;
		if (obj->o_obj != sig) {
			return -EINVAL;
		}
	} else {
		obj = cpt_object_add(CPT_OBJ_SIGHAND_STRUCT, current->sighand, ctx);
		if (obj == NULL)
			return -ENOMEM;
		cpt_obj_setpos(obj, ti->cpt_sighand, ctx);
		err = restore_one_sighand_struct(ti, ctx);
		if (err)
			return err;
	}


	obj = lookup_cpt_obj_bypos(CPT_OBJ_SIGNAL_STRUCT, ti->cpt_signal, ctx);
	if (obj) {
		struct signal_struct *sig = current->signal;
		if (obj->o_obj != sig) {
			return -EINVAL;
		}
/*		if (current->signal) {
			pid_t session;

			session = process_session(current);
			set_process_vgroup(current, session);
			set_signal_vsession(current->signal, session);
		}*/
	} else {
		obj = cpt_object_add(CPT_OBJ_SIGNAL_STRUCT, current->signal, ctx);
		if (obj == NULL)
			return -ENOMEM;
		cpt_obj_setpos(obj, ti->cpt_signal, ctx);
		err = restore_one_signal_struct(ti, exiting, ctx);
		if (err)
			return err;
	}

	return 0;
}

#ifdef CONFIG_X86
static u32 decode_segment(u32 segid)
{
	if (segid == CPT_SEG_ZERO)
		return 0;

	/* TLS descriptors */
	if (segid <= CPT_SEG_TLS3)
		return ((GDT_ENTRY_TLS_MIN + segid-CPT_SEG_TLS1)<<3) + 3;

	/* LDT descriptor, it is just an index to LDT array */
	if (segid >= CPT_SEG_LDT)
		return ((segid - CPT_SEG_LDT) << 3) | 7;

	/* Check for one of standard descriptors */
#ifdef CONFIG_X86_64
	if (segid == CPT_SEG_USER32_DS)
		return __USER32_DS;
	if (segid == CPT_SEG_USER32_CS)
		return __USER32_CS;
	if (segid == CPT_SEG_USER64_DS)
		return __USER_DS;
	if (segid == CPT_SEG_USER64_CS)
		return __USER_CS;
#else
	if (segid == CPT_SEG_USER32_DS)
		return __USER_DS;
	if (segid == CPT_SEG_USER32_CS)
		return __USER_CS;
#endif
	wprintk("Invalid segment reg %d\n", segid);
	return 0;
}
#endif

#if defined (CONFIG_IA64)
void ia64_decrement_ip (struct pt_regs *regs)
{
	unsigned long w0, ri = ia64_psr(regs)->ri - 1;

	if (ia64_psr(regs)->ri == 0) {
		regs->cr_iip -= 16;
		ri = 2;
		get_user(w0, (char __user *) regs->cr_iip + 0);
		if (((w0 >> 1) & 0xf) == 2) {
			/*
			 * rfi'ing to slot 2 of an MLX bundle causes
			 * an illegal operation fault.  We don't want
			 * that to happen...
			 */
			ri = 1;
		}
	}
	ia64_psr(regs)->ri = ri;
}
#endif

static void rst_child_tid(unsigned long *child_tids)
{
	dprintk("rct: " CPT_FID "\n", CPT_TID(current));
	current->clear_child_tid = (void*)child_tids[0];
	current->set_child_tid = (void*)child_tids[1];
}

static void rst_last_siginfo(void)
{
	int signr;
	siginfo_t *info = current->last_siginfo;
	struct pt_regs *regs = task_pt_regs(current);
	struct k_sigaction *ka;
	int ptrace_id;

	dprintk("rlsi: " CPT_FID "\n", CPT_TID(current));

	spin_lock_irq(&current->sighand->siglock);
	current->last_siginfo = NULL;
	recalc_sigpending();

	ptrace_id = current->pn_state;
	clear_pn_state(current);

	switch (ptrace_id) {
	case PN_STOP_TF:
	case PN_STOP_TF_RT:
		/* frame_*signal */
		dprintk("SIGTRAP %u/%u(%s) %u/%u %u %ld %u %lu\n",
		       task_pid_vnr(current), current->pid, current->comm,
		       info->si_signo, info->si_code,
		       current->exit_code, SYSCALL_NR(regs),
		       current->ptrace, current->ptrace_message);
		goto out;
	case PN_STOP_ENTRY:
	case PN_STOP_LEAVE:
		/* do_syscall_trace */
		spin_unlock_irq(&current->sighand->siglock);
		dprintk("ptrace do_syscall_trace: %d %d\n", ptrace_id, current->exit_code);
		if (current->exit_code) {
			send_sig(current->exit_code, current, 1);
			current->exit_code = 0;
		}
		if (IN_SYSCALL(regs)) {
			if (ptrace_id == PN_STOP_ENTRY
#ifdef CONFIG_X86
			    && SYSCALL_ERRNO(regs) == ENOSYS
#endif
			    )
				SYSCALL_RESTART(regs);
			else if (IN_ERROR(regs) &&
				 syscall_is(current, regs, rt_sigtimedwait) &&
				 (SYSCALL_ERRNO(regs) == EAGAIN ||
				  SYSCALL_ERRNO(regs) == EINTR))
				SYSCALL_RESTART(regs);
		}
		return;
	case PN_STOP_FORK:
		/* fork */
		SYSCALL_SETRET(regs, current->ptrace_message);
		dprintk("ptrace fork returns pid %ld\n", SYSCALL_RETVAL(regs));
		goto out;
	case PN_STOP_VFORK:
		/* after vfork */
		SYSCALL_SETRET(regs, current->ptrace_message);
		dprintk("ptrace after vfork returns pid %ld\n", SYSCALL_RETVAL(regs));
		goto out;
	case PN_STOP_SIGNAL:
		/* normal case : dequeue signal */
		break;
	case PN_STOP_EXIT:
		dprintk("ptrace exit caught\n");
		current->ptrace &= ~PT_TRACE_EXIT;
		spin_unlock_irq(&current->sighand->siglock);
		module_put(THIS_MODULE);
		complete_and_exit(NULL, current->ptrace_message);
		BUG();
	case PN_STOP_EXEC:
		eprintk("ptrace after exec caught: must not happen\n");
		BUG();
	default:
		eprintk("ptrace with unknown identity %d\n", ptrace_id);
		BUG();
	}

	signr = current->exit_code;
	if (signr == 0) {
		dprintk("rlsi: canceled signal %d\n", info->si_signo);
		goto out;
	}
	current->exit_code = 0;

	if (signr != info->si_signo) {
		info->si_signo = signr;
		info->si_errno = 0;
		info->si_code = SI_USER;
		info->si_pid = task_pid_vnr(current->parent);
		info->si_uid = current->parent->cred->uid;
	}

	/* If the (new) signal is now blocked, requeue it.  */
	if (sigismember(&current->blocked, signr)) {
		dprintk("going to requeue signal %d\n", signr);
		goto out_resend_sig;
	}

	ka = &current->sighand->action[signr-1];
	if (ka->sa.sa_handler == SIG_IGN) {
		dprintk("going to resend signal %d (ignored)\n", signr);
		goto out;
	}
	if (ka->sa.sa_handler != SIG_DFL) {
		dprintk("going to resend signal %d (not SIG_DFL)\n", signr);
		goto out_resend_sig;
	}
        if (signr == SIGCONT ||
	    signr == SIGCHLD ||
	    signr == SIGWINCH ||
	    signr == SIGURG ||
	    current->pid == 1)
		goto out;

	/* All the rest, which we cannot handle are requeued. */
	dprintk("going to resend signal %d (sigh)\n", signr);
out_resend_sig:
	spin_unlock_irq(&current->sighand->siglock);
	send_sig_info(signr, info, current);
	return;

out:
	spin_unlock_irq(&current->sighand->siglock);
}

static void rst_finish_stop(void)
{
	/* ...
	 * do_signal() ->
	 *   get_signal_to_deliver() ->
	 *     do_signal_stop() ->
	 *       finish_stop()
	 *
	 * Normally after SIGCONT it will dequeue the next signal. If no signal
	 * is found, do_signal restarts syscall unconditionally.
	 * Otherwise signal handler is pushed on user stack.
	 */

	dprintk("rfs: " CPT_FID "\n", CPT_TID(current));

	clear_stop_state(current);
	current->exit_code = 0;
}

static void rst_restart_sys(void)
{
	struct pt_regs *regs = task_pt_regs(current);

	/* This hook is supposed to be executed, when we have
	 * to complete some interrupted syscall.
	 */
	dprintk("rrs: " CPT_FID "\n", CPT_TID(current));

	if (!IN_SYSCALL(regs) || !IN_ERROR(regs))
		return;

#ifdef __NR_pause
	if (syscall_is(current,regs,pause)) {
		if (SYSCALL_ERRNO(regs) == ERESTARTNOHAND) {
			current->state = TASK_INTERRUPTIBLE;
			schedule();
		}
	} else
#else
	/* On this arch pause() is simulated with sigsuspend(). */
	if (syscall_is(current,regs,rt_sigsuspend)) {
		if (SYSCALL_ERRNO(regs) == ERESTARTNOHAND) {
			current->state = TASK_INTERRUPTIBLE;
			schedule();
		}
	} else
#endif
	if (syscall_is(current,regs,rt_sigtimedwait)) {
		if (SYSCALL_ERRNO(regs) == EAGAIN ||
		    SYSCALL_ERRNO(regs) == EINTR) {
			SYSCALL_RESTART(regs);
		}
	} else if (syscall_is(current,regs,futex)) {
		if (SYSCALL_ERRNO(regs) == EINTR &&
		    !signal_pending(current)) {
			SYSCALL_RESTART(regs);
		}
	}

	if (!signal_pending(current)) {
		if (SYSCALL_ERRNO(regs) == ERESTARTSYS ||
		    SYSCALL_ERRNO(regs) == ERESTARTNOINTR ||
		    SYSCALL_ERRNO(regs) == ERESTARTNOHAND) {
			SYSCALL_RESTART(regs);
		} else if (SYSCALL_ERRNO(regs) == ERESTART_RESTARTBLOCK) {
			int new = __NR_restart_syscall;
#ifdef CONFIG_X86_64
			if (task_thread_info(current)->flags&_TIF_IA32)
				new = __NR32_restart_syscall;
#endif
			SYSCALL_RESTART2(regs, new);
		}
	}
}

#ifdef CONFIG_X86_32

static int restore_registers(struct task_struct *tsk, struct pt_regs *regs,
			     struct cpt_task_image *ti, struct cpt_x86_regs *b,
			     struct resume_info **rip, struct cpt_context *ctx)
{
	extern char i386_ret_from_resume;

	if (b->cpt_object != CPT_OBJ_X86_REGS)
		return -EINVAL;

	tsk->thread.sp = (unsigned long) regs;
	tsk->thread.sp0 = (unsigned long) (regs+1);
	tsk->thread.ip = (unsigned long) &i386_ret_from_resume;

	tsk->thread.gs = decode_segment(b->cpt_gs);
	tsk->thread.debugreg0 = b->cpt_debugreg[0];
	tsk->thread.debugreg1 = b->cpt_debugreg[1];
	tsk->thread.debugreg2 = b->cpt_debugreg[2];
	tsk->thread.debugreg3 = b->cpt_debugreg[3];
	tsk->thread.debugreg6 = b->cpt_debugreg[6];
	tsk->thread.debugreg7 = b->cpt_debugreg[7];

	regs->bx = b->cpt_ebx;
	regs->cx = b->cpt_ecx;
	regs->dx = b->cpt_edx;
	regs->si = b->cpt_esi;
	regs->di = b->cpt_edi;
	regs->bp = b->cpt_ebp;
	regs->ax = b->cpt_eax;
	regs->ds = b->cpt_xds;
	regs->es = b->cpt_xes;
	regs->orig_ax = b->cpt_orig_eax;
	regs->ip = b->cpt_eip;
	regs->cs = b->cpt_xcs;
	regs->flags = b->cpt_eflags;
	regs->sp = b->cpt_esp;
	regs->ss = b->cpt_xss;

	regs->cs = decode_segment(b->cpt_xcs);
	regs->ss = decode_segment(b->cpt_xss);
	regs->ds = decode_segment(b->cpt_xds);
	regs->es = decode_segment(b->cpt_xes);
	regs->fs = decode_segment(b->cpt_fs);

	tsk->thread.sp -= HOOK_RESERVE;
	memset((void*)tsk->thread.sp, 0, HOOK_RESERVE);
	*rip = (void*)tsk->thread.sp;

	return 0;
}

#elif defined(CONFIG_X86_64)

static void xlate_ptregs_32_to_64(struct pt_regs *d, struct cpt_x86_regs *s)
{
	memset(d, 0, sizeof(struct pt_regs));
	d->bp = s->cpt_ebp;
	d->bx = s->cpt_ebx;
	d->ax = (s32)s->cpt_eax;
	d->cx = s->cpt_ecx;
	d->dx = s->cpt_edx;
	d->si = s->cpt_esi;
	d->di = s->cpt_edi;
	d->orig_ax = (s32)s->cpt_orig_eax;
	d->ip = s->cpt_eip;
	d->cs = s->cpt_xcs;
	d->flags = s->cpt_eflags;
	d->sp = s->cpt_esp;
	d->ss = s->cpt_xss;
}

static int restore_registers(struct task_struct *tsk, struct pt_regs *regs,
			     struct cpt_task_image *ti, struct cpt_obj_bits *hdr,
			     struct resume_info **rip, struct cpt_context *ctx)
{
	if (hdr->cpt_object == CPT_OBJ_X86_64_REGS) {
		struct cpt_x86_64_regs *b = (void*)hdr;

		tsk->thread.sp = (unsigned long) regs;
		tsk->thread.sp0 = (unsigned long) (regs+1);

		tsk->thread.fs = b->cpt_fsbase;
		tsk->thread.gs = b->cpt_gsbase;
		tsk->thread.fsindex = decode_segment(b->cpt_fsindex);
		tsk->thread.gsindex = decode_segment(b->cpt_gsindex);
		tsk->thread.ds = decode_segment(b->cpt_ds);
		tsk->thread.es = decode_segment(b->cpt_es);
		tsk->thread.debugreg0 = b->cpt_debugreg[0];
		tsk->thread.debugreg1 = b->cpt_debugreg[1];
		tsk->thread.debugreg2 = b->cpt_debugreg[2];
		tsk->thread.debugreg3 = b->cpt_debugreg[3];
		tsk->thread.debugreg6 = b->cpt_debugreg[6];
		tsk->thread.debugreg7 = b->cpt_debugreg[7];

		memcpy(regs, &b->cpt_r15, sizeof(struct pt_regs));

		tsk->thread.usersp = regs->sp;
		regs->cs = decode_segment(b->cpt_cs);
		regs->ss = decode_segment(b->cpt_ss);
	} else if (hdr->cpt_object == CPT_OBJ_X86_REGS) {
		struct cpt_x86_regs *b = (void*)hdr;

		tsk->thread.sp = (unsigned long) regs;
		tsk->thread.sp0 = (unsigned long) (regs+1);

		tsk->thread.fs = 0;
		tsk->thread.gs = 0;
		tsk->thread.fsindex = decode_segment(b->cpt_fs);
		tsk->thread.gsindex = decode_segment(b->cpt_gs);
		tsk->thread.debugreg0 = b->cpt_debugreg[0];
		tsk->thread.debugreg1 = b->cpt_debugreg[1];
		tsk->thread.debugreg2 = b->cpt_debugreg[2];
		tsk->thread.debugreg3 = b->cpt_debugreg[3];
		tsk->thread.debugreg6 = b->cpt_debugreg[6];
		tsk->thread.debugreg7 = b->cpt_debugreg[7];

		xlate_ptregs_32_to_64(regs, b);

		tsk->thread.usersp = regs->sp;
		regs->cs = decode_segment(b->cpt_xcs);
		regs->ss = decode_segment(b->cpt_xss);
		tsk->thread.ds = decode_segment(b->cpt_xds);
		tsk->thread.es = decode_segment(b->cpt_xes);
	} else {
		return -EINVAL;
	}

	tsk->thread.sp -= HOOK_RESERVE;
	memset((void*)tsk->thread.sp, 0, HOOK_RESERVE);
	*rip = (void*)tsk->thread.sp;
	return 0;
}

#elif defined(CONFIG_IA64)

#define MASK(nbits)	((1UL << (nbits)) - 1)	/* mask with NBITS bits set */

#define PUT_BITS(first, last, nat)					\
	({								\
		unsigned long bit = ia64_unat_pos(&pt->r##first);	\
		unsigned long nbits = (last - first + 1);		\
		unsigned long mask = MASK(nbits) << first;		\
		long dist;						\
		if (bit < first)					\
			dist = 64 + bit - first;			\
		else							\
			dist = bit - first;				\
		ia64_rotl(nat & mask, dist);				\
	})

unsigned long
ia64_put_scratch_nat_bits (struct pt_regs *pt, unsigned long nat)
{
	unsigned long scratch_unat;

	/*
	 * Registers that are stored consecutively in struct pt_regs
	 * can be handled in parallel.  If the register order in
	 * struct_pt_regs changes, this code MUST be updated.
	 */
	scratch_unat  = PUT_BITS( 1,  1, nat);
	scratch_unat |= PUT_BITS( 2,  3, nat);
	scratch_unat |= PUT_BITS(12, 13, nat);
	scratch_unat |= PUT_BITS(14, 14, nat);
	scratch_unat |= PUT_BITS(15, 15, nat);
	scratch_unat |= PUT_BITS( 8, 11, nat);
	scratch_unat |= PUT_BITS(16, 31, nat);

	return scratch_unat;

}

static unsigned long
ia64_put_saved_nat_bits (struct switch_stack *pt, unsigned long nat)
{
	unsigned long scratch_unat;

	scratch_unat  = PUT_BITS( 4,  7, nat);

	return scratch_unat;

}

#undef PUT_BITS


static int restore_registers(struct task_struct *tsk, struct pt_regs *pt,
			     struct cpt_task_image *ti,
			     struct cpt_ia64_regs *r,
			     struct resume_info **rip,
			     struct cpt_context *ctx)
{
	extern char ia64_ret_from_resume;
	struct switch_stack *sw;
	struct resume_info *ri;
	struct ia64_psr *psr = ia64_psr(pt);
	void *krbs = (void *)tsk + IA64_RBS_OFFSET;
	unsigned long reg;

	if (r->cpt_object != CPT_OBJ_IA64_REGS)
		return -EINVAL;

	if (r->num_regs > 96) {
		eprintk(CPT_FID " too much RSE regs %lu\n",
			CPT_TID(tsk), r->num_regs);
		return -EINVAL;
	}

	*rip = ri = ((void*)pt) - HOOK_RESERVE;
	sw = ((struct switch_stack *) ri) - 1;

	memmove(sw, (void*)tsk->thread.ksp + 16, sizeof(struct switch_stack));
	memset(ri, 0, HOOK_RESERVE);

	/* gr 1,2-3,8-11,12-13,14,15,16-31 are on pt_regs */
	memcpy(&pt->r1,  &r->gr[1],  8*(2-1));
	memcpy(&pt->r2,  &r->gr[2],  8*(4-2));
	memcpy(&pt->r8,  &r->gr[8],  8*(12-8));
	memcpy(&pt->r12, &r->gr[12], 8*(14-12));
	memcpy(&pt->r14, &r->gr[14], 8*(15-14));
	memcpy(&pt->r15, &r->gr[15], 8*(16-15));
	memcpy(&pt->r16, &r->gr[16], 8*(32-16));

	pt->b0 = r->br[0];
	pt->b6 = r->br[6];
	pt->b7 = r->br[7];

	pt->ar_bspstore	= r->ar_bspstore;
	pt->ar_unat	= r->ar_unat;
	pt->ar_pfs	= r->ar_pfs;
	pt->ar_ccv	= r->ar_ccv;
	pt->ar_fpsr	= r->ar_fpsr;
	pt->ar_csd	= r->ar_csd;
	pt->ar_ssd	= r->ar_ssd;
	pt->ar_rsc	= r->ar_rsc;

	pt->cr_iip	= r->cr_iip;
	pt->cr_ipsr	= r->cr_ipsr;

	pt->pr = r->pr;

	pt->cr_ifs = r->cfm;

	/* fpregs 6..9,10..11 are in pt_regs */
	memcpy(&pt->f6,  &r->fr[2*6],  16*(10-6));
	memcpy(&pt->f10, &r->fr[2*10], 16*(12-10));
	/* fpreg 12..15 are on switch stack */
	memcpy(&sw->f12, &r->fr[2*12], 16*(16-12));
	/* fpregs 32...127 */
	tsk->thread.flags |= IA64_THREAD_FPH_VALID;
	memcpy(tsk->thread.fph, &r->fr[32*2], 16*(128-32));
	ia64_drop_fpu(tsk);
	psr->dfh = 1;

	memcpy(&sw->r4, &r->gr[4], 8*(8-4));
	memcpy(&sw->b1, &r->br[1], 8*(6-1));
	sw->ar_lc = r->ar_lc;

	memcpy(&sw->f2, &r->fr[2*2], 16*(6-2));
	memcpy(&sw->f16, &r->fr[2*16], 16*(32-16));

	sw->caller_unat = 0;
	sw->ar_fpsr = pt->ar_fpsr;
	sw->ar_unat = 0;
	if (r->nat[0] & 0xFFFFFF0FUL)
		sw->caller_unat = ia64_put_scratch_nat_bits(pt, r->nat[0]);
	if (r->nat[0] & 0xF0)
		sw->ar_unat = ia64_put_saved_nat_bits(sw, r->nat[0]);

	sw->ar_bspstore = (unsigned long)ia64_rse_skip_regs(krbs, r->num_regs);
	memset(krbs, 0, (void*)sw->ar_bspstore - krbs);
	sw->ar_rnat = 0;
	sw->ar_pfs = 0;

	/* This is tricky. When we are in syscall, we have frame
	 * of output register (sometimes, plus one input reg sometimes).
	 * It is not so easy to restore such frame, RSE optimizes
	 * and does not fetch those regs from backstore. So, we restore
	 * the whole frame as local registers, and then repartition it
	 * in ia64_ret_from_resume().
	 */
	if ((long)pt->cr_ifs >= 0) {
		unsigned long out = (r->cfm&0x7F) - ((r->cfm>>7)&0x7F);
		sw->ar_pfs = out | (out<<7);
	}
	if (r->ar_ec)
		sw->ar_pfs |= (r->ar_ec & 0x3F) << 52;

	for (reg = 0; reg < r->num_regs; reg++) {
		unsigned long *ptr = ia64_rse_skip_regs(krbs, reg);
		unsigned long *rnatp;
		unsigned long set_rnat = 0;

		*ptr = r->gr[32+reg];

		if (reg < 32)
			set_rnat = (r->nat[0] & (1UL<<(reg+32)));
		else
			set_rnat = (r->nat[1] & (1UL<<(reg-32)));

		if (set_rnat) {
			rnatp = ia64_rse_rnat_addr(ptr);
			if ((unsigned long)rnatp >= sw->ar_bspstore)
				rnatp = &sw->ar_rnat;
			*rnatp |= (1UL<<ia64_rse_slot_num(ptr));
		}
	}
	
	sw->b0 = (unsigned long) &ia64_ret_from_resume;
	tsk->thread.ksp = (unsigned long) sw - 16;

#define PRED_LEAVE_SYSCALL	1 /* TRUE iff leave from syscall */
#define PRED_KERNEL_STACK	2 /* returning to kernel-stacks? */
#define PRED_USER_STACK		3 /* returning to user-stacks? */
#define PRED_SYSCALL		4 /* inside a system call? */
#define PRED_NON_SYSCALL	5 /* complement of PRED_SYSCALL */

	pt->loadrs = r->loadrs;
	sw->pr = 0;
	sw->pr &= ~(1UL << PRED_LEAVE_SYSCALL);
	sw->pr &= ~((1UL << PRED_SYSCALL) | (1UL << PRED_NON_SYSCALL));
	sw->pr &= ~(1UL << PRED_KERNEL_STACK);
	sw->pr |= (1UL << PRED_USER_STACK);
	if ((long)pt->cr_ifs < 0) {
		sw->pr |= (1UL << PRED_NON_SYSCALL);
	} else {
		sw->pr |= ((1UL << PRED_SYSCALL) | (1UL << PRED_LEAVE_SYSCALL));
	}

	return 0;
}
#endif

asmlinkage void rst_resume_work(struct resume_info *ri)
{
	if (ri->hooks & (1<<HOOK_TID))
		rst_child_tid(ri->tid_ptrs);
	if (ri->hooks & (1<<HOOK_CONT))
		rst_finish_stop();
	if (ri->hooks & (1<<HOOK_LSI))
		rst_last_siginfo();
	if (ri->hooks & (1<<HOOK_RESTART))
		rst_restart_sys();
	module_put(THIS_MODULE);
}

static void rst_apply_mxcsr_mask(struct task_struct *tsk)
{
#ifdef CONFIG_X86_32
	unsigned int flags;

	flags = test_cpu_caps();

	/* if cpu does not support sse2 mask 6 bit (DAZ flag) and 16-31 bits
	   in MXCSR to avoid general protection fault */
	if (!(flags & (1 << CPT_CPU_X86_SSE2)))
		tsk->thread.xstate->fxsave.mxcsr &= 0x0000ffbf;
#endif
}

#ifdef CONFIG_X86
#include <asm/i387.h>
#endif

int rst_restore_process(struct cpt_context *ctx)
{
	cpt_object_t *obj;

	for_each_object(obj, CPT_OBJ_TASK) {
		struct task_struct *tsk = obj->o_obj;
		struct cpt_task_image *ti = obj->o_image;
		struct pt_regs * regs;
		struct cpt_object_hdr *b;
		struct cpt_siginfo_image *lsi = NULL;
		struct resume_info *ri = NULL;
		int i;
		int err = 0;
#ifdef CONFIG_BEANCOUNTERS
		struct task_beancounter *tbc;
		struct user_beancounter *new_bc, *old_bc;
#endif

		if (tsk == NULL) {
			eprintk_ctx("oops, task %d/%s is missing\n", ti->cpt_pid, ti->cpt_comm);
			return -EFAULT;
		}

		wait_task_inactive(tsk, 0);
#ifdef CONFIG_BEANCOUNTERS
		tbc = &tsk->task_bc;
		new_bc = rst_lookup_ubc(ti->cpt_exec_ub, ctx);
		err = virtinfo_notifier_call(VITYPE_SCP,
				VIRTINFO_SCP_RSTTSK, new_bc);
		if (err & NOTIFY_FAIL) {
			put_beancounter(new_bc);
			return -ECHRNG; 
		}
		old_bc = tbc->exec_ub;
		if ((err & VIRTNOTIFY_CHANGE) && old_bc != new_bc) {
			dprintk(" *** replacing ub %p by %p for %p (%d %s)\n",
					old_bc, new_bc, tsk,
					tsk->pid, tsk->comm);
			tbc->exec_ub = new_bc;
			new_bc = old_bc;
		}
		put_beancounter(new_bc);
#endif
		regs = task_pt_regs(tsk);

		if (!tsk->exit_state) {
			tsk->lock_depth = -1;
#ifdef CONFIG_PREEMPT
			task_thread_info(tsk)->preempt_count--;
#endif
		}

		if (tsk->static_prio != ti->cpt_static_prio)
			set_user_nice(tsk, PRIO_TO_NICE((s32)ti->cpt_static_prio));

		cpt_sigset_import(&tsk->blocked, ti->cpt_sigblocked);
		cpt_sigset_import(&tsk->real_blocked, ti->cpt_sigrblocked);
		cpt_sigset_import(&tsk->saved_sigmask, ti->cpt_sigsuspend_blocked);
		cpt_sigset_import(&tsk->pending.signal, ti->cpt_sigpending);

#ifdef CONFIG_IA64
		SET_UNALIGN_CTL(tsk, ti->cpt_prctl_uac);
		SET_FPEMU_CTL(tsk, ti->cpt_prctl_fpemu);
#endif
		tsk->did_exec = (ti->cpt_did_exec != 0);
		tsk->utime = ti->cpt_utime;
		tsk->stime = ti->cpt_stime;
		if (ctx->image_version == CPT_VERSION_8)
			tsk->start_time = _ns_to_timespec(ti->cpt_starttime*TICK_NSEC);
		else
			cpt_timespec_import(&tsk->start_time, ti->cpt_starttime);
		_set_normalized_timespec(&tsk->start_time,
					tsk->start_time.tv_sec +
					VE_TASK_INFO(tsk)->owner_env->start_timespec.tv_sec,
					tsk->start_time.tv_nsec +
					VE_TASK_INFO(tsk)->owner_env->start_timespec.tv_nsec);

		tsk->nvcsw = ti->cpt_nvcsw;
		tsk->nivcsw = ti->cpt_nivcsw;
		tsk->min_flt = ti->cpt_min_flt;
		tsk->maj_flt = ti->cpt_maj_flt;

#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,8)
		tsk->cutime = ti->cpt_cutime;
		tsk->cstime = ti->cpt_cstime;
		tsk->cnvcsw = ti->cpt_cnvcsw;
		tsk->cnivcsw = ti->cpt_cnivcsw;
		tsk->cmin_flt = ti->cpt_cmin_flt;
		tsk->cmaj_flt = ti->cpt_cmaj_flt;

		BUILD_BUG_ON(RLIM_NLIMITS > CPT_RLIM_NLIMITS);

		for (i=0; i<RLIM_NLIMITS; i++) {
			tsk->rlim[i].rlim_cur = ti->cpt_rlim_cur[i];
			tsk->rlim[i].rlim_max = ti->cpt_rlim_max[i];
		}
#else
		if (thread_group_leader(tsk) && tsk->signal) {
			tsk->signal->utime = ti->cpt_utime;
			tsk->signal->stime = ti->cpt_stime;
			tsk->signal->cutime = ti->cpt_cutime;
			tsk->signal->cstime = ti->cpt_cstime;
			tsk->signal->nvcsw = ti->cpt_nvcsw;
			tsk->signal->nivcsw = ti->cpt_nivcsw;
			tsk->signal->cnvcsw = ti->cpt_cnvcsw;
			tsk->signal->cnivcsw = ti->cpt_cnivcsw;
			tsk->signal->min_flt = ti->cpt_min_flt;
			tsk->signal->maj_flt = ti->cpt_maj_flt;
			tsk->signal->cmin_flt = ti->cpt_cmin_flt;
			tsk->signal->cmaj_flt = ti->cpt_cmaj_flt;

			for (i=0; i<RLIM_NLIMITS; i++) {
				tsk->signal->rlim[i].rlim_cur = ti->cpt_rlim_cur[i];
				tsk->signal->rlim[i].rlim_max = ti->cpt_rlim_max[i];
			}
		}
#endif

#ifdef CONFIG_X86
		for (i=0; i<3; i++) {
			if (i >= GDT_ENTRY_TLS_ENTRIES) {
				eprintk_ctx("too many tls descs\n");
			} else {
				tsk->thread.tls_array[i].a = ti->cpt_tls[i]&0xFFFFFFFF;
				tsk->thread.tls_array[i].b = ti->cpt_tls[i]>>32;
			}
		}
#endif

		clear_stopped_child_used_math(tsk);

		b = (void *)(ti+1);
		while ((void*)b < ((void*)ti) + ti->cpt_next) {
			/* Siginfo objects are at the end of obj array */
			if (b->cpt_object == CPT_OBJ_SIGINFO) {
				struct ve_struct *env = set_exec_env(VE_TASK_INFO(tsk)->owner_env);
				restore_sigqueue(tsk, &tsk->pending, (unsigned long)b, (unsigned long)ti + ti->cpt_next);
				set_exec_env(env);
				break;
			}

			switch (b->cpt_object) {
#ifdef CONFIG_X86
			case CPT_OBJ_BITS:
				if (b->cpt_content == CPT_CONTENT_X86_FPUSTATE &&
				    cpu_has_fxsr) {
					if (init_fpu(tsk))
						return -ENOMEM;
					memcpy(tsk->thread.xstate,
					       (void*)b + b->cpt_hdrlen,
					       sizeof(struct i387_fxsave_struct));
					rst_apply_mxcsr_mask(tsk);
					if (ti->cpt_used_math)
						set_stopped_child_used_math(tsk);
				}
#ifndef CONFIG_X86_64
				else if (b->cpt_content == CPT_CONTENT_X86_FPUSTATE_OLD &&
					 !cpu_has_fxsr) {		
					if (init_fpu(tsk))
						return -ENOMEM;
					memcpy(tsk->thread.xstate,
					       (void*)b + b->cpt_hdrlen,
					       sizeof(struct i387_fsave_struct));
					if (ti->cpt_used_math)
						set_stopped_child_used_math(tsk);
				}
#endif
				break;
#endif
			case CPT_OBJ_LASTSIGINFO:
				lsi = (void*)b;
				break;
			case CPT_OBJ_X86_REGS:
			case CPT_OBJ_X86_64_REGS:
			case CPT_OBJ_IA64_REGS:
				if (restore_registers(tsk, regs, ti, (void*)b, &ri, ctx)) {
					eprintk_ctx("cannot restore registers: image is corrupted\n");
					return -EINVAL;
				}
				break;
			case CPT_OBJ_SIGALTSTACK: {
				struct cpt_sigaltstack_image *sas;
				sas = (struct cpt_sigaltstack_image *)b;
				tsk->sas_ss_sp = sas->cpt_stack;
				tsk->sas_ss_size = sas->cpt_stacksize;
				break;
			    }
			case CPT_OBJ_TASK_AUX: {
				struct cpt_task_aux_image *ai;
				ai = (struct cpt_task_aux_image *)b;
				tsk->robust_list = cpt_ptr_import(ai->cpt_robust_list);
#ifdef CONFIG_X86_64
#ifdef CONFIG_COMPAT
				if (task_thread_info(tsk)->flags&_TIF_IA32) {
					tsk->robust_list = (void __user *)NULL;
					tsk->compat_robust_list = cpt_ptr_import(ai->cpt_robust_list);
				}
#endif
#endif
				break;
			    }
			}
			b = ((void*)b) + b->cpt_next;
		}

		if (ri == NULL && !(ti->cpt_state & (EXIT_ZOMBIE|EXIT_DEAD))) {
			eprintk_ctx("missing register info\n");
			return -EINVAL;
		}

		if (ti->cpt_ppid != ti->cpt_rppid) {
			struct task_struct *parent;
			struct ve_struct *env = set_exec_env(VE_TASK_INFO(tsk)->owner_env);
			write_lock_irq(&tasklist_lock);
			parent = find_task_by_vpid(ti->cpt_ppid);
			if (parent && parent != tsk->parent) {
				list_add(&tsk->ptrace_entry, &tsk->parent->ptraced);
				/*
				 * Ptraced kids are no longer in the parent children
				 *  remove_parent(tsk);
				 *  tsk->parent = parent;
				 *  add_parent(tsk);
				 */
			}
			write_unlock_irq(&tasklist_lock);
			set_exec_env(env);
		}

		tsk->ptrace_message = ti->cpt_ptrace_message;
		tsk->pn_state = ti->cpt_pn_state;
		tsk->stopped_state = ti->cpt_stopped_state;
		task_thread_info(tsk)->flags = ti->cpt_thrflags;

		/* The image was created with kernel < 2.6.16, while
		 * task hanged in sigsuspend -> do_signal.
		 *
		 * FIXME! This needs more brain efforts...
		 */
		if (ti->cpt_sigsuspend_state) {
			set_restore_sigmask();
		}

#ifdef CONFIG_X86_64
		task_thread_info(tsk)->flags |= _TIF_FORK | _TIF_RESUME;
		if (!ti->cpt_64bit)
			task_thread_info(tsk)->flags |= _TIF_IA32;
#endif

#ifdef CONFIG_X86_32
		do {
			if (regs->orig_ax == __NR__newselect && regs->di) {
				struct timeval tv;
				if (access_process_vm(tsk, regs->di, &tv, 
						sizeof(tv), 0) != sizeof(tv)) {
					wprintk_ctx("task %d/%d(%s): Error 1 in access_process_vm: edi %ld\n",
						task_pid_vnr(tsk), tsk->pid, tsk->comm,
					       regs->di);
					break;
				}
				dprintk_ctx("task %d/%d(%s): Old timeval in newselect: %ld.%ld\n",
				       task_pid_vnr(tsk), tsk->pid, tsk->comm,
				       tv.tv_sec, tv.tv_usec);
				tv.tv_sec -= ctx->delta_time.tv_sec;
				if (tv.tv_usec < ctx->delta_time.tv_nsec / 1000) {
					tv.tv_usec += 1000000 - ctx->delta_time.tv_nsec / 1000;
					tv.tv_sec--;
				} else {
					tv.tv_usec -= ctx->delta_time.tv_nsec / 1000;
				}
				if (tv.tv_sec < 0) {
					tv.tv_sec = 0;
					tv.tv_usec = 0;
				}
				dprintk_ctx("task %d/%d(%s): New timeval in newselect: %ld.%ld\n",
					task_pid_vnr(tsk), tsk->pid, tsk->comm,
				       tv.tv_sec, tv.tv_usec);
				if (access_process_vm(tsk, regs->di, &tv, 
						sizeof(tv), 1) != sizeof(tv)) {
					wprintk_ctx("task %d/%d(%s): Error 1 in access_process_vm write: edi %ld\n",
						task_pid_vnr(tsk), tsk->pid, tsk->comm, regs->di);
				}
				
			} else if (regs->orig_ax == __NR_select && regs->di) {
				struct {
					unsigned long n;
					fd_set __user *inp, *outp, *exp;
					struct timeval __user *tvp;
				} a;
				struct timeval tv;
				if (access_process_vm(tsk, regs->bx, &a, 
						sizeof(a), 0) != sizeof(a)) {
					wprintk_ctx("task %d: Error 2 in access_process_vm\n", tsk->pid);
					break;
				}
				if (access_process_vm(tsk, (unsigned long)a.tvp,
						&tv, sizeof(tv), 0) != sizeof(tv)) {
					wprintk_ctx("task %d: Error 3 in access_process_vm\n", tsk->pid);
					break;
				}
				dprintk_ctx("task %d: Old timeval in select: %ld.%ld\n",
					tsk->pid, tv.tv_sec, tv.tv_usec);
				tv.tv_sec -= ctx->delta_time.tv_sec;
				if (tv.tv_usec < ctx->delta_time.tv_nsec / 1000) {
					tv.tv_usec += 1000000 - ctx->delta_time.tv_nsec / 1000;
					tv.tv_sec--;
				} else {
					tv.tv_usec -= ctx->delta_time.tv_nsec / 1000;
				}
				if (tv.tv_sec < 0) {
					tv.tv_sec = 0;
					tv.tv_usec = 0;
				}
				dprintk_ctx("task %d: New timeval in select: %ld.%ld\n",
					tsk->pid, tv.tv_sec, tv.tv_usec);
				if (access_process_vm(tsk, (unsigned long)a.tvp,
						&tv, sizeof(tv), 1) != sizeof(tv)) {
					wprintk_ctx("task %d: Error 3 in access_process_vm write\n", tsk->pid);
				}
			}
		} while (0);
#endif

		if (ri && IN_SYSCALL(regs) && IN_ERROR(regs)) {
			switch (SYSCALL_ERRNO(regs)) {
			case ERESTARTSYS:
			case ERESTARTNOINTR:
			case ERESTARTNOHAND:
			case ERESTART_RESTARTBLOCK:
			case EAGAIN:
			case EINTR:
				ri->hooks |= (1<<HOOK_RESTART);
			}
		}

		if (ri && (lsi || tsk->pn_state)) {
			/* ... -> ptrace_notify()
			 * or
			 * ... -> do_signal() -> get_signal_to_deliver() ->
			 *   ptrace stop
			 */
			tsk->last_siginfo = &ri->last_siginfo;
			ri->hooks |= (1<<HOOK_LSI);
			if (lsi)
				decode_siginfo(tsk->last_siginfo, lsi);
		}

		tsk->ptrace = ti->cpt_ptrace;
		tsk->flags = ti->cpt_flags & ~PF_FROZEN;
		clear_tsk_thread_flag(tsk, TIF_FREEZE);
		tsk->exit_signal = ti->cpt_exit_signal;

		if (ri && tsk->stopped_state) {
			dprintk_ctx("finish_stop\n");
			if (ti->cpt_state != TASK_STOPPED)
				eprintk_ctx("Hellooo, state is %u\n", (unsigned)ti->cpt_state);
			ri->hooks |= (1<<HOOK_CONT);
		}

		if (ri && (ti->cpt_set_tid || ti->cpt_clear_tid)) {
			ri->hooks |= (1<<HOOK_TID);
			ri->tid_ptrs[0] = ti->cpt_clear_tid;
			ri->tid_ptrs[1] = ti->cpt_set_tid;
			dprintk_ctx("settids\n");
		}

		if (ri && ri->hooks &&
		    !(ti->cpt_state & (EXIT_ZOMBIE|EXIT_DEAD))) {
			if (try_module_get(THIS_MODULE))
				ri->hook = rst_resume_work;
		}

		if (ti->cpt_state == TASK_TRACED)
			tsk->state = TASK_TRACED;
		else if (ti->cpt_state & (EXIT_ZOMBIE|EXIT_DEAD)) {
			tsk->signal->it[CPUCLOCK_VIRT].expires = 0;
			tsk->signal->it[CPUCLOCK_PROF].expires = 0;
			if (tsk->state != TASK_DEAD)
				eprintk_ctx("oops, schedule() did not make us dead\n");
		}

		if (thread_group_leader(tsk) &&
		    ti->cpt_it_real_value &&
		    !(ti->cpt_state & (EXIT_ZOMBIE|EXIT_DEAD))) {
			ktime_t val;
			s64 nsec;

			nsec = ti->cpt_it_real_value;
			val.tv64 = 0;

			if (ctx->image_version < CPT_VERSION_9)
				nsec *= TICK_NSEC;

			val = ktime_add_ns(val, nsec - ctx->delta_nsec);
			if (val.tv64 <= 0)
				val.tv64 = NSEC_PER_USEC;
			dprintk("rst itimer " CPT_FID " +%Ld %Lu\n", CPT_TID(tsk),
				(long long)val.tv64,
				(unsigned long long)ti->cpt_it_real_value);

			spin_lock_irq(&tsk->sighand->siglock);
			if (hrtimer_try_to_cancel(&tsk->signal->real_timer) >= 0) {
				/* FIXME. Check!!!! */
				hrtimer_start(&tsk->signal->real_timer, val, HRTIMER_MODE_REL);
			} else {
				wprintk_ctx("Timer clash. Impossible?\n");
			}
			spin_unlock_irq(&tsk->sighand->siglock);

			dprintk_ctx("itimer " CPT_FID " +%Lu\n", CPT_TID(tsk),
				    (unsigned long long)val.tv64);
		}

		module_put(THIS_MODULE);
	}
	return 0;
}
