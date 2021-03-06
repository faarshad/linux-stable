/*
 *
 *  kernel/cpt/cpt_net.c
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
#include <linux/slab.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/nsproxy.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/socket.h>
#include <linux/netdevice.h>
#include <linux/inetdevice.h>
#include <net/addrconf.h>
#include <linux/rtnetlink.h>
#include <linux/ve.h>
#include <linux/ve_proto.h>
#include <linux/vzcalluser.h>
#include <linux/cpt_image.h>
#include <linux/if_tun.h>
#include <linux/veth.h>
#include <linux/fdtable.h>

#include <linux/cpt_export.h>

#include "cpt_obj.h"
#include "cpt_context.h"
#include "cpt_kernel.h"
#include "cpt_syscalls.h"

static void cpt_dump_netstats(struct net_device *dev, struct cpt_context * ctx)
{
	struct cpt_netstats_image *n;
	struct net_device_stats *stats;

	if (!dev->netdev_ops->ndo_get_stats)
		return;

	n = cpt_get_buf(ctx);
	stats = dev->netdev_ops->ndo_get_stats(dev);
	cpt_open_object(NULL, ctx);

	n->cpt_next = CPT_NULL;
	n->cpt_object = CPT_OBJ_NET_STATS;
	n->cpt_hdrlen = sizeof(*n);
	n->cpt_content = CPT_CONTENT_VOID;

	n->cpt_rx_packets = stats->rx_packets;
	n->cpt_tx_packets = stats->tx_packets;
	n->cpt_rx_bytes = stats->rx_bytes;
	n->cpt_tx_bytes = stats->tx_bytes;
	n->cpt_rx_errors = stats->rx_errors;
	n->cpt_tx_errors = stats->tx_errors;
	n->cpt_rx_dropped = stats->rx_dropped;
	n->cpt_tx_dropped = stats->tx_dropped;
	n->cpt_multicast = stats->multicast;
	n->cpt_collisions = stats->collisions;
	n->cpt_rx_length_errors = stats->rx_length_errors;
	n->cpt_rx_over_errors = stats->rx_over_errors;
	n->cpt_rx_crc_errors = stats->rx_crc_errors;
	n->cpt_rx_frame_errors = stats->rx_frame_errors;
	n->cpt_rx_fifo_errors = stats->rx_fifo_errors;
	n->cpt_rx_missed_errors = stats->rx_missed_errors;
	n->cpt_tx_aborted_errors = stats->tx_aborted_errors;
	n->cpt_tx_carrier_errors = stats->tx_carrier_errors;
	n->cpt_tx_fifo_errors = stats->tx_fifo_errors;
	n->cpt_tx_heartbeat_errors = stats->tx_heartbeat_errors;
	n->cpt_tx_window_errors = stats->tx_window_errors;
	n->cpt_rx_compressed = stats->rx_compressed;
	n->cpt_tx_compressed = stats->tx_compressed;

	ctx->write(n, sizeof(*n), ctx);
	cpt_close_object(ctx);
	cpt_release_buf(ctx);
	return;
}

int cpt_dump_link(struct cpt_context * ctx)
{
	struct net *net = get_exec_env()->ve_netns;
	struct net_device *dev;

	cpt_open_section(ctx, CPT_SECT_NET_DEVICE);
	for_each_netdev(net, dev) {
		struct cpt_netdev_image v;
		struct cpt_hwaddr_image hw;
		loff_t saved_obj;

		if (dev->netdev_ops->ndo_cpt == NULL) {
			eprintk_ctx("unsupported netdev %s\n", dev->name);
			cpt_close_section(ctx);
			return -EBUSY;
		}

		cpt_open_object(NULL, ctx);

		v.cpt_next = CPT_NULL;
		v.cpt_object = CPT_OBJ_NET_DEVICE;
		v.cpt_hdrlen = sizeof(v);
		v.cpt_content = CPT_CONTENT_ARRAY;

		v.cpt_index = dev->ifindex;
		v.cpt_flags = dev->flags;
		memcpy(v.cpt_name, dev->name, IFNAMSIZ);
		ctx->write(&v, sizeof(v), ctx);

		cpt_push_object(&saved_obj, ctx);

		cpt_open_object(NULL, ctx);
		dev->netdev_ops->ndo_cpt(dev, &cpt_ops, ctx);

		/* Dump hardware address */
		cpt_open_object(NULL, ctx);
		hw.cpt_next = CPT_NULL;
		hw.cpt_object = CPT_OBJ_NET_HWADDR;
		hw.cpt_hdrlen = sizeof(hw);
		hw.cpt_content = CPT_CONTENT_VOID;

		if (dev->dev_addrs.count != 1) {
			eprintk_ctx("multiple hwaddrs on %s\n", dev->name);
			return -EINVAL;
		}

		BUILD_BUG_ON(sizeof(hw.cpt_dev_addr) != MAX_ADDR_LEN);
		memcpy(hw.cpt_dev_addr, dev->dev_addr, sizeof(hw.cpt_dev_addr));
		ctx->write(&hw, sizeof(hw), ctx);
		cpt_close_object(ctx);
		
		cpt_dump_netstats(dev, ctx);

		cpt_pop_object(&saved_obj, ctx);

		cpt_close_object(ctx);
	}
	cpt_close_section(ctx);
	return 0;
}

int cpt_suspend_network(struct cpt_context *ctx)
{
	get_exec_env()->disable_net = 1;
	synchronize_net();
	return 0;
}

int cpt_resume_network(struct cpt_context *ctx)
{
	struct ve_struct *env;
	env = get_ve_by_id(ctx->ve_id);
	if (!env)
		return -ESRCH;
	env->disable_net = 0;
	put_ve(env);
	return 0;
}

int cpt_dump_ifaddr(struct cpt_context * ctx)
{
	struct net *net = get_exec_env()->ve_netns;
	struct net_device *dev;

	cpt_open_section(ctx, CPT_SECT_NET_IFADDR);
	for_each_netdev(net, dev) {
		struct in_device *idev = in_dev_get(dev);
		struct in_ifaddr *ifa;

		if (!idev)
			continue;

		for (ifa = idev->ifa_list; ifa; ifa = ifa->ifa_next) {
			struct cpt_ifaddr_image v;
			cpt_open_object(NULL, ctx);

			v.cpt_next = CPT_NULL;
			v.cpt_object = CPT_OBJ_NET_IFADDR;
			v.cpt_hdrlen = sizeof(v);
			v.cpt_content = CPT_CONTENT_VOID;

			v.cpt_index = dev->ifindex;
			v.cpt_family = AF_INET;
			v.cpt_masklen = ifa->ifa_prefixlen;
			v.cpt_flags = ifa->ifa_flags;
			v.cpt_scope = ifa->ifa_scope;
			memset(&v.cpt_address, 0, sizeof(v.cpt_address));
			memset(&v.cpt_peer, 0, sizeof(v.cpt_peer));
			memset(&v.cpt_broadcast, 0, sizeof(v.cpt_broadcast));
			v.cpt_address[0] = ifa->ifa_local;
			v.cpt_peer[0] = ifa->ifa_address;
			v.cpt_broadcast[0] = ifa->ifa_broadcast;
			memcpy(v.cpt_label, ifa->ifa_label, IFNAMSIZ);
			ctx->write(&v, sizeof(v), ctx);
			cpt_close_object(ctx);
		}
		in_dev_put(idev);
	}
#if defined(CONFIG_IPV6) || defined (CONFIG_IPV6_MODULE)
	for_each_netdev(net, dev) {
		struct inet6_dev *idev = in6_dev_get(dev);
		struct inet6_ifaddr *ifa;

		if (!idev)
			continue;

		for (ifa = idev->addr_list; ifa; ifa = ifa->if_next) {
			struct cpt_ifaddr_image v;

			if (dev == net->loopback_dev &&
			    ifa->prefix_len == 128 &&
			    ifa->addr.s6_addr32[0] == 0 &&
			    ifa->addr.s6_addr32[1] == 0 &&
			    ifa->addr.s6_addr32[2] == 0 &&
			    ifa->addr.s6_addr32[3] == htonl(1))
				continue;

			cpt_open_object(NULL, ctx);

			v.cpt_next = CPT_NULL;
			v.cpt_object = CPT_OBJ_NET_IFADDR;
			v.cpt_hdrlen = sizeof(v);
			v.cpt_content = CPT_CONTENT_VOID;

			v.cpt_index = dev->ifindex;
			v.cpt_family = AF_INET6;
			v.cpt_masklen = ifa->prefix_len;
			v.cpt_flags = ifa->flags;
			v.cpt_scope = ifa->scope;
			v.cpt_valid_lft = ifa->valid_lft;
			v.cpt_prefered_lft = ifa->prefered_lft;
			memcpy(&v.cpt_address, &ifa->addr, 16);
			memcpy(&v.cpt_peer, &ifa->addr, 16);
			memset(&v.cpt_broadcast, 0, sizeof(v.cpt_broadcast));
			memcpy(v.cpt_label, dev->name, IFNAMSIZ);
			ctx->write(&v, sizeof(v), ctx);
			cpt_close_object(ctx);
		}
		in6_dev_put(idev);
	}
#endif
	cpt_close_section(ctx);
	return 0;
}

#ifdef CONFIG_IP_FIB_TRIE
#error "Trie fib rules are known not to be restored proprly yet"
#endif

static int cpt_dump_route(struct cpt_context * ctx)
{
	int err;
	struct socket *sock;
	struct msghdr msg;
	struct iovec iov;
	struct {
		struct nlmsghdr nlh;
		struct rtgenmsg g;
	} req;
	struct sockaddr_nl nladdr;
	struct cpt_object_hdr v;
	mm_segment_t oldfs;
	char *pg;

	err = sock_create(AF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE, &sock);
	if (err)
		return err;

	memset(&nladdr, 0, sizeof(nladdr));
	nladdr.nl_family = AF_NETLINK;

	req.nlh.nlmsg_len = sizeof(req);
	req.nlh.nlmsg_type = RTM_GETROUTE;
	req.nlh.nlmsg_flags = NLM_F_ROOT|NLM_F_MATCH|NLM_F_REQUEST;
	req.nlh.nlmsg_pid = 0;
	req.g.rtgen_family = AF_INET;

	iov.iov_base=&req;
	iov.iov_len=sizeof(req);
	msg.msg_name=&nladdr;
	msg.msg_namelen=sizeof(nladdr);
	msg.msg_iov=&iov;
	msg.msg_iovlen=1;
	msg.msg_control=NULL;
	msg.msg_controllen=0;
	msg.msg_flags=MSG_DONTWAIT;

	oldfs = get_fs(); set_fs(KERNEL_DS);
	err = sock_sendmsg(sock, &msg, sizeof(req));
	set_fs(oldfs);

	if (err < 0)
		goto out_sock;

	pg = (char*)__get_free_page(GFP_KERNEL);
	if (pg == NULL) {
		err = -ENOMEM;
		goto out_sock;
	}

	cpt_open_section(ctx, CPT_SECT_NET_ROUTE);
	cpt_open_object(NULL, ctx);
	v.cpt_next = CPT_NULL;
	v.cpt_object = CPT_OBJ_NET_ROUTE;
	v.cpt_hdrlen = sizeof(v);
	v.cpt_content = CPT_CONTENT_NLMARRAY;

	ctx->write(&v, sizeof(v), ctx);

#if defined(CONFIG_IPV6) || defined (CONFIG_IPV6_MODULE)
restart:
#endif
	for (;;) {
		struct nlmsghdr *h;

		iov.iov_base = pg;
		iov.iov_len = PAGE_SIZE;

		oldfs = get_fs(); set_fs(KERNEL_DS);
		err = sock_recvmsg(sock, &msg, PAGE_SIZE, MSG_DONTWAIT);
		set_fs(oldfs);

		if (err < 0)
			goto out_sock_pg;
		if (msg.msg_flags & MSG_TRUNC) {
			err = -ENOBUFS;
			goto out_sock_pg;
		}

		h = (struct nlmsghdr*)pg;
		while (NLMSG_OK(h, err)) {
			if (h->nlmsg_type == NLMSG_DONE) {
				err = 0;
				goto done;
			}
			if (h->nlmsg_type == NLMSG_ERROR) {
				struct nlmsgerr *errm = (struct nlmsgerr*)NLMSG_DATA(h);
				err = errm->error;
				eprintk_ctx("NLMSG error: %d\n", errm->error);
				goto done;
			}
			if (h->nlmsg_type != RTM_NEWROUTE) {
				eprintk_ctx("NLMSG: %d\n", h->nlmsg_type);
				err = -EINVAL;
				goto done;
			}
			ctx->write(h, NLMSG_ALIGN(h->nlmsg_len), ctx);
			h = NLMSG_NEXT(h, err);
		}
		if (err) {
			eprintk_ctx("!!!Remnant of size %d %d %d\n", err, h->nlmsg_len, h->nlmsg_type);
			err = -EINVAL;
			break;
		}
	}
done:
#if defined(CONFIG_IPV6) || defined (CONFIG_IPV6_MODULE)
	if (!err && req.g.rtgen_family == AF_INET) {
		req.g.rtgen_family = AF_INET6;
		iov.iov_base=&req;
		iov.iov_len=sizeof(req);
		msg.msg_name=&nladdr;
		msg.msg_namelen=sizeof(nladdr);
		msg.msg_iov=&iov;
		msg.msg_iovlen=1;
		msg.msg_control=NULL;
		msg.msg_controllen=0;
		msg.msg_flags=MSG_DONTWAIT;

		oldfs = get_fs(); set_fs(KERNEL_DS);
		err = sock_sendmsg(sock, &msg, sizeof(req));
		set_fs(oldfs);

		if (err > 0)
			goto restart;
	}
#endif
	ctx->align(ctx);
	cpt_close_object(ctx);
	cpt_close_section(ctx);

out_sock_pg:
	free_page((unsigned long)pg);
out_sock:
	sock_release(sock);
	return err;
}

static int dumpfn(void *arg)
{
	int i;
	int *pfd = arg;
	char *argv[] = { "iptables-save", "-c", NULL };

	i = real_env_create(VEID(get_exec_env()), VE_ENTER|VE_SKIPLOCK, 2, NULL, 0);
	if (i < 0) {
		eprintk("cannot enter ve to dump iptables\n");
		module_put(THIS_MODULE);
		return 255 << 8;
	}

	if (pfd[1] != 1)
		sc_dup2(pfd[1], 1);

	for (i=0; i<current->files->fdt->max_fds; i++) {
		if (i != 1)
			sc_close(i);
	}

	module_put(THIS_MODULE);

	set_fs(KERNEL_DS);
	i = sc_execve("/sbin/iptables-save", argv, NULL);
	if (i == -ENOENT)
		i = sc_execve("/usr/sbin/iptables-save", argv, NULL);
	eprintk("failed to exec iptables-save: %d\n", i);
	return 255 << 8;
}


static int cpt_dump_iptables(struct cpt_context * ctx)
{
	int err = 0;
#ifdef CONFIG_VE_IPTABLES
	int pid;
	int pfd[2];
	struct file *f;
	struct cpt_object_hdr v;
	char buf[16];
	loff_t pos;
	int n;
	int status;
	mm_segment_t oldfs;
	sigset_t ignore, blocked;

	if (!(get_exec_env()->_iptables_modules & VE_IP_IPTABLES_MOD))
		return 0;

	err = sc_pipe(pfd);
	if (err < 0) {
		eprintk_ctx("sc_pipe: %d\n", err);
		return err;
	}
	ignore.sig[0] = CPT_SIG_IGNORE_MASK;
	sigprocmask(SIG_BLOCK, &ignore, &blocked);
	err = pid = local_kernel_thread(dumpfn, (void*)pfd, SIGCHLD, 0);
	if (err < 0) {
		eprintk_ctx("local_kernel_thread: %d\n", err);
		goto out;
	}

	f = fget(pfd[0]);
	sc_close(pfd[1]);
	sc_close(pfd[0]);

	cpt_open_section(ctx, CPT_SECT_NET_IPTABLES);

	cpt_open_object(NULL, ctx);
	v.cpt_next = CPT_NULL;
	v.cpt_object = CPT_OBJ_NAME;
	v.cpt_hdrlen = sizeof(v);
	v.cpt_content = CPT_CONTENT_NAME;

	ctx->write(&v, sizeof(v), ctx);

	pos = ctx->file->f_pos;
	do {
		oldfs = get_fs(); set_fs(KERNEL_DS);
		n = f->f_op->read(f, buf, sizeof(buf), &f->f_pos);
		set_fs(oldfs);
		if (n > 0)
			ctx->write(buf, n, ctx);
	} while (n > 0);

	if (n < 0)
		eprintk_ctx("read: %d\n", n);

	fput(f);

	oldfs = get_fs(); set_fs(KERNEL_DS);
	if ((err = sc_waitx(pid, 0, &status)) < 0)
		eprintk_ctx("wait4: %d\n", err);
	else if ((status & 0x7f) == 0) {
		err = (status & 0xff00) >> 8;
		if (err != 0) {
			eprintk_ctx("iptables-save exited with %d\n", err);
			err = -EINVAL;
		}
	} else {
		eprintk_ctx("iptables-save terminated\n");
		err = -EINVAL;
	}
	set_fs(oldfs);
	sigprocmask(SIG_SETMASK, &blocked, NULL);

	if (ctx->file->f_pos != pos) {
		buf[0] = 0;
		ctx->write(buf, 1, ctx);
		ctx->align(ctx);
		cpt_close_object(ctx);
		cpt_close_section(ctx);
	} else {
		pos = ctx->current_section;
		cpt_close_object(ctx);
		cpt_close_section(ctx);
		ctx->sections[CPT_SECT_NET_IPTABLES] = CPT_NULL;
		ctx->file->f_pos = pos;
	}
	return n ? : err;

out:
	if (pfd[1] >= 0)
		sc_close(pfd[1]);
	if (pfd[0] >= 0)
		sc_close(pfd[0]);
	sigprocmask(SIG_SETMASK, &blocked, NULL);
#endif
	return err;
}

int cpt_dump_ifinfo(struct cpt_context * ctx)
{
	int err;

	rtnl_lock();
	err = cpt_dump_link(ctx);
	if (!err)
		err = cpt_dump_ifaddr(ctx);
	rtnl_unlock();
	if (!err)
		err = cpt_dump_route(ctx);
	if (!err)
		err = cpt_dump_iptables(ctx);
	return err;
}
