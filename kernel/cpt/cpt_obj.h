#ifndef __CPT_OBJ_H_
#define __CPT_OBJ_H_ 1

#include <linux/list.h>
#include <linux/cpt_image.h>

typedef struct _cpt_object
{
	struct list_head	o_list;
	struct list_head	o_hash;
	int			o_count;
	int			o_index;
	int			o_lock;
	loff_t			o_pos;
	loff_t			o_ppos;
	void			*o_obj;
	void			*o_image;
	void			*o_parent;
	struct list_head	o_alist;
} cpt_object_t;

struct cpt_context;

#define for_each_object(obj, type) list_for_each_entry(obj, &ctx->object_array[type], o_list)


extern cpt_object_t *alloc_cpt_object(int gfp, struct cpt_context *ctx);
extern void free_cpt_object(cpt_object_t *obj, struct cpt_context *ctx);

cpt_object_t *lookup_cpt_object(enum _cpt_object_type type, void *p, struct cpt_context *ctx);
cpt_object_t *lookup_cpt_obj_bypos(enum _cpt_object_type type, loff_t pos, struct cpt_context *ctx);
cpt_object_t *lookup_cpt_obj_byindex(enum _cpt_object_type type, __u32 index, struct cpt_context *ctx);

static inline void cpt_obj_setpos(cpt_object_t *cpt, loff_t pos, struct cpt_context *ctx)
{
	cpt->o_pos = pos;
	/* Add to pos hash table */
}

static inline void cpt_obj_setobj(cpt_object_t *cpt, void *ptr, struct cpt_context *ctx)
{
	cpt->o_obj = ptr;
	/* Add to hash table */
}

static inline void cpt_obj_setindex(cpt_object_t *cpt, __u32 index, struct cpt_context *ctx)
{
	cpt->o_index = index;
	/* Add to index hash table */
}


extern void intern_cpt_object(enum _cpt_object_type type, cpt_object_t *obj, struct cpt_context *ctx);
extern void insert_cpt_object(enum _cpt_object_type type, cpt_object_t *obj, cpt_object_t *head, struct cpt_context *ctx);
extern cpt_object_t *cpt_object_add(enum _cpt_object_type type, void *p, struct cpt_context *ctx);
extern cpt_object_t *__cpt_object_add(enum _cpt_object_type type, void *p, unsigned int gfp_mask, struct cpt_context *ctx);
extern cpt_object_t *cpt_object_get(enum _cpt_object_type type, void *p, struct cpt_context *ctx);

extern int cpt_object_init(struct cpt_context *ctx);
extern int cpt_object_destroy(struct cpt_context *ctx);

#endif /* __CPT_OBJ_H_ */
