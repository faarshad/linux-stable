/*
 *
 *  include/linux/cpt_exports.h
 *
 *  Copyright (C) 2008  Parallels
 *  All rights reserved.
 *
 *  Licensing governed by "linux/COPYING.SWsoft" file.
 *
 */

#ifndef __CPT_EXPORTS_H__
#define __CPT_EXPORTS_H__

struct cpt_context;

struct cpt_ops {
	void (*write)(const void *addr, size_t count, struct cpt_context *ctx);
	void (*push_object)(loff_t *, struct cpt_context *);
	void (*pop_object)(loff_t *, struct cpt_context *);
	loff_t (*lookup_object)(int type, void *p, struct cpt_context *ctx);

};

extern struct cpt_ops cpt_ops;

struct rst_ops {
	int (*get_object)(int type, loff_t pos, void *tmp,
			int size, struct cpt_context *ctx);
	struct file *(*rst_file)(loff_t pos, int fd, struct cpt_context *ctx);
};

extern struct rst_ops rst_ops;

#endif

