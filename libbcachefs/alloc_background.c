// SPDX-License-Identifier: GPL-2.0
#include "bcachefs.h"
#include "alloc_background.h"
#include "alloc_foreground.h"
#include "backpointers.h"
#include "btree_cache.h"
#include "btree_io.h"
#include "btree_key_cache.h"
#include "btree_update.h"
#include "btree_update_interior.h"
#include "btree_gc.h"
#include "buckets.h"
#include "buckets_waiting_for_journal.h"
#include "clock.h"
#include "debug.h"
#include "ec.h"
#include "error.h"
#include "lru.h"
#include "recovery.h"
#include "varint.h"

#include <linux/kthread.h>
#include <linux/math64.h>
#include <linux/random.h>
#include <linux/rculist.h>
#include <linux/rcupdate.h>
#include <linux/sched/task.h>
#include <linux/sort.h>
#include <trace/events/bcachefs.h>

/* Persistent alloc info: */

static const unsigned BCH_ALLOC_V1_FIELD_BYTES[] = {
#define x(name, bits) [BCH_ALLOC_FIELD_V1_##name] = bits / 8,
	BCH_ALLOC_FIELDS_V1()
#undef x
};

struct bkey_alloc_unpacked {
	u64		journal_seq;
	u8		gen;
	u8		oldest_gen;
	u8		data_type;
	bool		need_discard:1;
	bool		need_inc_gen:1;
#define x(_name, _bits)	u##_bits _name;
	BCH_ALLOC_FIELDS_V2()
#undef  x
};

static inline u64 alloc_field_v1_get(const struct bch_alloc *a,
				     const void **p, unsigned field)
{
	unsigned bytes = BCH_ALLOC_V1_FIELD_BYTES[field];
	u64 v;

	if (!(a->fields & (1 << field)))
		return 0;

	switch (bytes) {
	case 1:
		v = *((const u8 *) *p);
		break;
	case 2:
		v = le16_to_cpup(*p);
		break;
	case 4:
		v = le32_to_cpup(*p);
		break;
	case 8:
		v = le64_to_cpup(*p);
		break;
	default:
		BUG();
	}

	*p += bytes;
	return v;
}

static inline void alloc_field_v1_put(struct bkey_i_alloc *a, void **p,
				      unsigned field, u64 v)
{
	unsigned bytes = BCH_ALLOC_V1_FIELD_BYTES[field];

	if (!v)
		return;

	a->v.fields |= 1 << field;

	switch (bytes) {
	case 1:
		*((u8 *) *p) = v;
		break;
	case 2:
		*((__le16 *) *p) = cpu_to_le16(v);
		break;
	case 4:
		*((__le32 *) *p) = cpu_to_le32(v);
		break;
	case 8:
		*((__le64 *) *p) = cpu_to_le64(v);
		break;
	default:
		BUG();
	}

	*p += bytes;
}

static void bch2_alloc_unpack_v1(struct bkey_alloc_unpacked *out,
				 struct bkey_s_c k)
{
	const struct bch_alloc *in = bkey_s_c_to_alloc(k).v;
	const void *d = in->data;
	unsigned idx = 0;

	out->gen = in->gen;

#define x(_name, _bits) out->_name = alloc_field_v1_get(in, &d, idx++);
	BCH_ALLOC_FIELDS_V1()
#undef  x
}

static int bch2_alloc_unpack_v2(struct bkey_alloc_unpacked *out,
				struct bkey_s_c k)
{
	struct bkey_s_c_alloc_v2 a = bkey_s_c_to_alloc_v2(k);
	const u8 *in = a.v->data;
	const u8 *end = bkey_val_end(a);
	unsigned fieldnr = 0;
	int ret;
	u64 v;

	out->gen	= a.v->gen;
	out->oldest_gen	= a.v->oldest_gen;
	out->data_type	= a.v->data_type;

#define x(_name, _bits)							\
	if (fieldnr < a.v->nr_fields) {					\
		ret = bch2_varint_decode_fast(in, end, &v);		\
		if (ret < 0)						\
			return ret;					\
		in += ret;						\
	} else {							\
		v = 0;							\
	}								\
	out->_name = v;							\
	if (v != out->_name)						\
		return -1;						\
	fieldnr++;

	BCH_ALLOC_FIELDS_V2()
#undef  x
	return 0;
}

static int bch2_alloc_unpack_v3(struct bkey_alloc_unpacked *out,
				struct bkey_s_c k)
{
	struct bkey_s_c_alloc_v3 a = bkey_s_c_to_alloc_v3(k);
	const u8 *in = a.v->data;
	const u8 *end = bkey_val_end(a);
	unsigned fieldnr = 0;
	int ret;
	u64 v;

	out->gen	= a.v->gen;
	out->oldest_gen	= a.v->oldest_gen;
	out->data_type	= a.v->data_type;
	out->need_discard = BCH_ALLOC_V3_NEED_DISCARD(a.v);
	out->need_inc_gen = BCH_ALLOC_V3_NEED_INC_GEN(a.v);
	out->journal_seq = le64_to_cpu(a.v->journal_seq);

#define x(_name, _bits)							\
	if (fieldnr < a.v->nr_fields) {					\
		ret = bch2_varint_decode_fast(in, end, &v);		\
		if (ret < 0)						\
			return ret;					\
		in += ret;						\
	} else {							\
		v = 0;							\
	}								\
	out->_name = v;							\
	if (v != out->_name)						\
		return -1;						\
	fieldnr++;

	BCH_ALLOC_FIELDS_V2()
#undef  x
	return 0;
}

static struct bkey_alloc_unpacked bch2_alloc_unpack(struct bkey_s_c k)
{
	struct bkey_alloc_unpacked ret = { .gen	= 0 };

	switch (k.k->type) {
	case KEY_TYPE_alloc:
		bch2_alloc_unpack_v1(&ret, k);
		break;
	case KEY_TYPE_alloc_v2:
		bch2_alloc_unpack_v2(&ret, k);
		break;
	case KEY_TYPE_alloc_v3:
		bch2_alloc_unpack_v3(&ret, k);
		break;
	}

	return ret;
}

struct bkey_i_alloc_v4 *
bch2_trans_start_alloc_update(struct btree_trans *trans, struct btree_iter *iter,
			      struct bpos pos)
{
	struct bkey_s_c k;
	struct bkey_i_alloc_v4 *a;
	int ret;

	bch2_trans_iter_init(trans, iter, BTREE_ID_alloc, pos,
			     BTREE_ITER_WITH_UPDATES|
			     BTREE_ITER_CACHED|
			     BTREE_ITER_INTENT);
	k = bch2_btree_iter_peek_slot(iter);
	ret = bkey_err(k);
	if (ret) {
		bch2_trans_iter_exit(trans, iter);
		return ERR_PTR(ret);
	}

	a = bch2_alloc_to_v4_mut(trans, k);
	if (IS_ERR(a))
		bch2_trans_iter_exit(trans, iter);
	return a;
}

static unsigned bch_alloc_v1_val_u64s(const struct bch_alloc *a)
{
	unsigned i, bytes = offsetof(struct bch_alloc, data);

	for (i = 0; i < ARRAY_SIZE(BCH_ALLOC_V1_FIELD_BYTES); i++)
		if (a->fields & (1 << i))
			bytes += BCH_ALLOC_V1_FIELD_BYTES[i];

	return DIV_ROUND_UP(bytes, sizeof(u64));
}

int bch2_alloc_v1_invalid(const struct bch_fs *c, struct bkey_s_c k,
			  int rw, struct printbuf *err)
{
	struct bkey_s_c_alloc a = bkey_s_c_to_alloc(k);

	/* allow for unknown fields */
	if (bkey_val_u64s(a.k) < bch_alloc_v1_val_u64s(a.v)) {
		prt_printf(err, "incorrect value size (%zu < %u)",
		       bkey_val_u64s(a.k), bch_alloc_v1_val_u64s(a.v));
		return -EINVAL;
	}

	return 0;
}

int bch2_alloc_v2_invalid(const struct bch_fs *c, struct bkey_s_c k,
			  int rw, struct printbuf *err)
{
	struct bkey_alloc_unpacked u;

	if (bch2_alloc_unpack_v2(&u, k)) {
		prt_printf(err, "unpack error");
		return -EINVAL;
	}

	return 0;
}

int bch2_alloc_v3_invalid(const struct bch_fs *c, struct bkey_s_c k,
			  int rw, struct printbuf *err)
{
	struct bkey_alloc_unpacked u;

	if (bch2_alloc_unpack_v3(&u, k)) {
		prt_printf(err, "unpack error");
		return -EINVAL;
	}

	return 0;
}

int bch2_alloc_v4_invalid(const struct bch_fs *c, struct bkey_s_c k,
			  int rw, struct printbuf *err)
{
	struct bkey_s_c_alloc_v4 a = bkey_s_c_to_alloc_v4(k);

	if (alloc_v4_u64s(a.v) != bkey_val_u64s(k.k)) {
		prt_printf(err, "bad val size (%lu != %u)",
		       bkey_val_u64s(k.k), alloc_v4_u64s(a.v));
		return -EINVAL;
	}

	if (!BCH_ALLOC_V4_BACKPOINTERS_START(a.v) &&
	    BCH_ALLOC_V4_NR_BACKPOINTERS(a.v)) {
		prt_printf(err, "invalid backpointers_start");
		return -EINVAL;
	}

	if (rw == WRITE) {
		if (alloc_data_type(*a.v, a.v->data_type) != a.v->data_type) {
			prt_printf(err, "invalid data type (got %u should be %u)",
			       a.v->data_type, alloc_data_type(*a.v, a.v->data_type));
			return -EINVAL;
		}

		switch (a.v->data_type) {
		case BCH_DATA_free:
		case BCH_DATA_need_gc_gens:
		case BCH_DATA_need_discard:
			if (a.v->dirty_sectors ||
			    a.v->cached_sectors ||
			    a.v->stripe) {
				prt_printf(err, "empty data type free but have data");
				return -EINVAL;
			}
			break;
		case BCH_DATA_sb:
		case BCH_DATA_journal:
		case BCH_DATA_btree:
		case BCH_DATA_user:
		case BCH_DATA_parity:
			if (!a.v->dirty_sectors) {
				prt_printf(err, "data_type %s but dirty_sectors==0",
				       bch2_data_types[a.v->data_type]);
				return -EINVAL;
			}
			break;
		case BCH_DATA_cached:
			if (!a.v->cached_sectors ||
			    a.v->dirty_sectors ||
			    a.v->stripe) {
				prt_printf(err, "data type inconsistency");
				return -EINVAL;
			}

			if (!a.v->io_time[READ] &&
			    test_bit(BCH_FS_CHECK_ALLOC_TO_LRU_REFS_DONE, &c->flags)) {
				prt_printf(err, "cached bucket with read_time == 0");
				return -EINVAL;
			}
			break;
		case BCH_DATA_stripe:
			if (!a.v->stripe) {
				prt_printf(err, "data_type %s but stripe==0",
				       bch2_data_types[a.v->data_type]);
				return -EINVAL;
			}
			break;
		}
	}

	return 0;
}

static inline u64 swab40(u64 x)
{
	return (((x & 0x00000000ffULL) << 32)|
		((x & 0x000000ff00ULL) << 16)|
		((x & 0x0000ff0000ULL) >>  0)|
		((x & 0x00ff000000ULL) >> 16)|
		((x & 0xff00000000ULL) >> 32));
}

void bch2_alloc_v4_swab(struct bkey_s k)
{
	struct bch_alloc_v4 *a = bkey_s_to_alloc_v4(k).v;
	struct bch_backpointer *bp, *bps;

	a->journal_seq		= swab64(a->journal_seq);
	a->flags		= swab32(a->flags);
	a->dirty_sectors	= swab32(a->dirty_sectors);
	a->cached_sectors	= swab32(a->cached_sectors);
	a->io_time[0]		= swab64(a->io_time[0]);
	a->io_time[1]		= swab64(a->io_time[1]);
	a->stripe		= swab32(a->stripe);
	a->nr_external_backpointers = swab32(a->nr_external_backpointers);

	bps = alloc_v4_backpointers(a);
	for (bp = bps; bp < bps + BCH_ALLOC_V4_NR_BACKPOINTERS(a); bp++) {
		bp->bucket_offset	= swab40(bp->bucket_offset);
		bp->bucket_len		= swab32(bp->bucket_len);
		bch2_bpos_swab(&bp->pos);
	}
}

void bch2_alloc_to_text(struct printbuf *out, struct bch_fs *c, struct bkey_s_c k)
{
	struct bch_alloc_v4 _a;
	const struct bch_alloc_v4 *a = &_a;
	const struct bch_backpointer *bps;
	unsigned i;

	if (k.k->type == KEY_TYPE_alloc_v4)
		a = bkey_s_c_to_alloc_v4(k).v;
	else
		bch2_alloc_to_v4(k, &_a);

	prt_newline(out);
	printbuf_indent_add(out, 2);

	prt_printf(out, "gen %u oldest_gen %u data_type %s",
	       a->gen, a->oldest_gen, bch2_data_types[a->data_type]);
	prt_newline(out);
	prt_printf(out, "journal_seq       %llu",	a->journal_seq);
	prt_newline(out);
	prt_printf(out, "need_discard      %llu",	BCH_ALLOC_V4_NEED_DISCARD(a));
	prt_newline(out);
	prt_printf(out, "need_inc_gen      %llu",	BCH_ALLOC_V4_NEED_INC_GEN(a));
	prt_newline(out);
	prt_printf(out, "dirty_sectors     %u",	a->dirty_sectors);
	prt_newline(out);
	prt_printf(out, "cached_sectors    %u",	a->cached_sectors);
	prt_newline(out);
	prt_printf(out, "stripe            %u",	a->stripe);
	prt_newline(out);
	prt_printf(out, "stripe_redundancy %u",	a->stripe_redundancy);
	prt_newline(out);
	prt_printf(out, "io_time[READ]     %llu",	a->io_time[READ]);
	prt_newline(out);
	prt_printf(out, "io_time[WRITE]    %llu",	a->io_time[WRITE]);
	prt_newline(out);
	prt_printf(out, "backpointers:     %llu",	BCH_ALLOC_V4_NR_BACKPOINTERS(a));
	printbuf_indent_add(out, 2);

	bps = alloc_v4_backpointers_c(a);
	for (i = 0; i < BCH_ALLOC_V4_NR_BACKPOINTERS(a); i++) {
		prt_newline(out);
		bch2_backpointer_to_text(out, &bps[i]);
	}

	printbuf_indent_sub(out, 4);
}

void bch2_alloc_to_v4(struct bkey_s_c k, struct bch_alloc_v4 *out)
{
	if (k.k->type == KEY_TYPE_alloc_v4) {
		int d;

		*out = *bkey_s_c_to_alloc_v4(k).v;

		d = (int) BCH_ALLOC_V4_U64s -
			(int) (BCH_ALLOC_V4_BACKPOINTERS_START(out) ?: BCH_ALLOC_V4_U64s_V0);
		if (unlikely(d > 0)) {
			memset((u64 *) out + BCH_ALLOC_V4_BACKPOINTERS_START(out),
			       0,
			       d * sizeof(u64));
			SET_BCH_ALLOC_V4_BACKPOINTERS_START(out, BCH_ALLOC_V4_U64s);
		}
	} else {
		struct bkey_alloc_unpacked u = bch2_alloc_unpack(k);

		*out = (struct bch_alloc_v4) {
			.journal_seq		= u.journal_seq,
			.flags			= u.need_discard,
			.gen			= u.gen,
			.oldest_gen		= u.oldest_gen,
			.data_type		= u.data_type,
			.stripe_redundancy	= u.stripe_redundancy,
			.dirty_sectors		= u.dirty_sectors,
			.cached_sectors		= u.cached_sectors,
			.io_time[READ]		= u.read_time,
			.io_time[WRITE]		= u.write_time,
			.stripe			= u.stripe,
		};

		SET_BCH_ALLOC_V4_BACKPOINTERS_START(out, BCH_ALLOC_V4_U64s);
	}
}

struct bkey_i_alloc_v4 *bch2_alloc_to_v4_mut(struct btree_trans *trans, struct bkey_s_c k)
{
	unsigned bytes = k.k->type == KEY_TYPE_alloc_v4
		? bkey_bytes(k.k)
		: sizeof(struct bkey_i_alloc_v4);
	struct bkey_i_alloc_v4 *ret;

	/*
	 * Reserve space for one more backpointer here:
	 * Not sketchy at doing it this way, nope...
	 */
	ret = bch2_trans_kmalloc(trans, bytes + sizeof(struct bch_backpointer));
	if (IS_ERR(ret))
		return ret;

	if (k.k->type == KEY_TYPE_alloc_v4) {
		bkey_reassemble(&ret->k_i, k);

		if (BCH_ALLOC_V4_BACKPOINTERS_START(&ret->v) < BCH_ALLOC_V4_U64s) {
			struct bch_backpointer *src, *dst;

			src = alloc_v4_backpointers(&ret->v);
			SET_BCH_ALLOC_V4_BACKPOINTERS_START(&ret->v, BCH_ALLOC_V4_U64s);
			dst = alloc_v4_backpointers(&ret->v);

			memmove(dst, src, BCH_ALLOC_V4_NR_BACKPOINTERS(&ret->v) *
				sizeof(struct bch_backpointer));
			memset(src, 0, dst - src);
			set_alloc_v4_u64s(ret);
		}
	} else {
		bkey_alloc_v4_init(&ret->k_i);
		ret->k.p = k.k->p;
		bch2_alloc_to_v4(k, &ret->v);
	}
	return ret;
}

int bch2_alloc_read(struct bch_fs *c)
{
	struct btree_trans trans;
	struct btree_iter iter;
	struct bkey_s_c k;
	struct bch_alloc_v4 a;
	struct bch_dev *ca;
	int ret;

	bch2_trans_init(&trans, c, 0, 0);

	for_each_btree_key(&trans, iter, BTREE_ID_alloc, POS_MIN,
			   BTREE_ITER_PREFETCH, k, ret) {
		/*
		 * Not a fsck error because this is checked/repaired by
		 * bch2_check_alloc_key() which runs later:
		 */
		if (!bch2_dev_bucket_exists(c, k.k->p))
			continue;

		ca = bch_dev_bkey_exists(c, k.k->p.inode);
		bch2_alloc_to_v4(k, &a);

		*bucket_gen(ca, k.k->p.offset) = a.gen;
	}
	bch2_trans_iter_exit(&trans, &iter);

	bch2_trans_exit(&trans);

	if (ret)
		bch_err(c, "error reading alloc info: %s", bch2_err_str(ret));

	return ret;
}

/* Free space/discard btree: */

static int bch2_bucket_do_index(struct btree_trans *trans,
				struct bkey_s_c alloc_k,
				const struct bch_alloc_v4 *a,
				bool set)
{
	struct bch_fs *c = trans->c;
	struct bch_dev *ca = bch_dev_bkey_exists(c, alloc_k.k->p.inode);
	struct btree_iter iter;
	struct bkey_s_c old;
	struct bkey_i *k;
	enum btree_id btree;
	enum bch_bkey_type old_type = !set ? KEY_TYPE_set : KEY_TYPE_deleted;
	enum bch_bkey_type new_type =  set ? KEY_TYPE_set : KEY_TYPE_deleted;
	struct printbuf buf = PRINTBUF;
	int ret;

	if (a->data_type != BCH_DATA_free &&
	    a->data_type != BCH_DATA_need_discard)
		return 0;

	k = bch2_trans_kmalloc(trans, sizeof(*k));
	if (IS_ERR(k))
		return PTR_ERR(k);

	bkey_init(&k->k);
	k->k.type = new_type;

	switch (a->data_type) {
	case BCH_DATA_free:
		btree = BTREE_ID_freespace;
		k->k.p = alloc_freespace_pos(alloc_k.k->p, *a);
		bch2_key_resize(&k->k, 1);
		break;
	case BCH_DATA_need_discard:
		btree = BTREE_ID_need_discard;
		k->k.p = alloc_k.k->p;
		break;
	default:
		return 0;
	}

	bch2_trans_iter_init(trans, &iter, btree,
			     bkey_start_pos(&k->k),
			     BTREE_ITER_INTENT);
	old = bch2_btree_iter_peek_slot(&iter);
	ret = bkey_err(old);
	if (ret)
		goto err;

	if (ca->mi.freespace_initialized &&
	    bch2_trans_inconsistent_on(old.k->type != old_type, trans,
			"incorrect key when %s %s btree (got %s should be %s)\n"
			"  for %s",
			set ? "setting" : "clearing",
			bch2_btree_ids[btree],
			bch2_bkey_types[old.k->type],
			bch2_bkey_types[old_type],
			(bch2_bkey_val_to_text(&buf, c, alloc_k), buf.buf))) {
		ret = -EIO;
		goto err;
	}

	ret = bch2_trans_update(trans, &iter, k, 0);
err:
	bch2_trans_iter_exit(trans, &iter);
	printbuf_exit(&buf);
	return ret;
}

int bch2_trans_mark_alloc(struct btree_trans *trans,
			  enum btree_id btree_id, unsigned level,
			  struct bkey_s_c old, struct bkey_i *new,
			  unsigned flags)
{
	struct bch_fs *c = trans->c;
	struct bch_alloc_v4 old_a, *new_a;
	u64 old_lru, new_lru;
	int ret = 0;

	/*
	 * Deletion only happens in the device removal path, with
	 * BTREE_TRIGGER_NORUN:
	 */
	BUG_ON(new->k.type != KEY_TYPE_alloc_v4);

	bch2_alloc_to_v4(old, &old_a);
	new_a = &bkey_i_to_alloc_v4(new)->v;

	new_a->data_type = alloc_data_type(*new_a, new_a->data_type);

	if (new_a->dirty_sectors > old_a.dirty_sectors ||
	    new_a->cached_sectors > old_a.cached_sectors) {
		new_a->io_time[READ] = max_t(u64, 1, atomic64_read(&c->io_clock[READ].now));
		new_a->io_time[WRITE]= max_t(u64, 1, atomic64_read(&c->io_clock[WRITE].now));
		SET_BCH_ALLOC_V4_NEED_INC_GEN(new_a, true);
		SET_BCH_ALLOC_V4_NEED_DISCARD(new_a, true);
	}

	if (data_type_is_empty(new_a->data_type) &&
	    BCH_ALLOC_V4_NEED_INC_GEN(new_a) &&
	    !bch2_bucket_is_open_safe(c, new->k.p.inode, new->k.p.offset)) {
		new_a->gen++;
		SET_BCH_ALLOC_V4_NEED_INC_GEN(new_a, false);
	}

	if (old_a.data_type != new_a->data_type ||
	    (new_a->data_type == BCH_DATA_free &&
	     alloc_freespace_genbits(old_a) != alloc_freespace_genbits(*new_a))) {
		ret =   bch2_bucket_do_index(trans, old, &old_a, false) ?:
			bch2_bucket_do_index(trans, bkey_i_to_s_c(new), new_a, true);
		if (ret)
			return ret;
	}

	if (new_a->data_type == BCH_DATA_cached &&
	    !new_a->io_time[READ])
		new_a->io_time[READ] = max_t(u64, 1, atomic64_read(&c->io_clock[READ].now));

	old_lru = alloc_lru_idx(old_a);
	new_lru = alloc_lru_idx(*new_a);

	if (old_lru != new_lru) {
		ret = bch2_lru_change(trans, new->k.p.inode, new->k.p.offset,
				      old_lru, &new_lru, old);
		if (ret)
			return ret;

		if (new_a->data_type == BCH_DATA_cached)
			new_a->io_time[READ] = new_lru;
	}

	return 0;
}

static int bch2_check_alloc_key(struct btree_trans *trans,
				struct btree_iter *alloc_iter,
				struct btree_iter *discard_iter,
				struct btree_iter *freespace_iter)
{
	struct bch_fs *c = trans->c;
	struct bch_dev *ca;
	struct bch_alloc_v4 a;
	unsigned discard_key_type, freespace_key_type;
	struct bkey_s_c alloc_k, k;
	struct printbuf buf = PRINTBUF;
	int ret;

	alloc_k = bch2_dev_bucket_exists(c, alloc_iter->pos)
		? bch2_btree_iter_peek_slot(alloc_iter)
		: bch2_btree_iter_peek(alloc_iter);
	if (!alloc_k.k)
		return 1;

	ret = bkey_err(alloc_k);
	if (ret)
		return ret;

	if (fsck_err_on(!bch2_dev_bucket_exists(c, alloc_k.k->p), c,
			"alloc key for invalid device:bucket %llu:%llu",
			alloc_k.k->p.inode, alloc_k.k->p.offset))
		return bch2_btree_delete_at(trans, alloc_iter, 0);

	ca = bch_dev_bkey_exists(c, alloc_k.k->p.inode);
	if (!ca->mi.freespace_initialized)
		return 0;

	bch2_alloc_to_v4(alloc_k, &a);

	discard_key_type = a.data_type == BCH_DATA_need_discard
		? KEY_TYPE_set : 0;
	freespace_key_type = a.data_type == BCH_DATA_free
		? KEY_TYPE_set : 0;

	bch2_btree_iter_set_pos(discard_iter, alloc_k.k->p);
	bch2_btree_iter_set_pos(freespace_iter, alloc_freespace_pos(alloc_k.k->p, a));

	k = bch2_btree_iter_peek_slot(discard_iter);
	ret = bkey_err(k);
	if (ret)
		goto err;

	if (k.k->type != discard_key_type &&
	    (c->opts.reconstruct_alloc ||
	     fsck_err(c, "incorrect key in need_discard btree (got %s should be %s)\n"
		      "  %s",
		      bch2_bkey_types[k.k->type],
		      bch2_bkey_types[discard_key_type],
		      (bch2_bkey_val_to_text(&buf, c, alloc_k), buf.buf)))) {
		struct bkey_i *update =
			bch2_trans_kmalloc(trans, sizeof(*update));

		ret = PTR_ERR_OR_ZERO(update);
		if (ret)
			goto err;

		bkey_init(&update->k);
		update->k.type	= discard_key_type;
		update->k.p	= discard_iter->pos;

		ret = bch2_trans_update(trans, discard_iter, update, 0);
		if (ret)
			goto err;
	}

	k = bch2_btree_iter_peek_slot(freespace_iter);
	ret = bkey_err(k);
	if (ret)
		goto err;

	if (k.k->type != freespace_key_type &&
	    (c->opts.reconstruct_alloc ||
	     fsck_err(c, "incorrect key in freespace btree (got %s should be %s)\n"
		      "  %s",
		      bch2_bkey_types[k.k->type],
		      bch2_bkey_types[freespace_key_type],
		      (printbuf_reset(&buf),
		       bch2_bkey_val_to_text(&buf, c, alloc_k), buf.buf)))) {
		struct bkey_i *update =
			bch2_trans_kmalloc(trans, sizeof(*update));

		ret = PTR_ERR_OR_ZERO(update);
		if (ret)
			goto err;

		bkey_init(&update->k);
		update->k.type	= freespace_key_type;
		update->k.p	= freespace_iter->pos;
		bch2_key_resize(&update->k, 1);

		ret = bch2_trans_update(trans, freespace_iter, update, 0);
		if (ret)
			goto err;
	}
err:
fsck_err:
	printbuf_exit(&buf);
	return ret;
}

static int bch2_check_discard_freespace_key(struct btree_trans *trans,
					    struct btree_iter *iter)
{
	struct bch_fs *c = trans->c;
	struct btree_iter alloc_iter;
	struct bkey_s_c alloc_k;
	struct bch_alloc_v4 a;
	u64 genbits;
	struct bpos pos;
	enum bch_data_type state = iter->btree_id == BTREE_ID_need_discard
		? BCH_DATA_need_discard
		: BCH_DATA_free;
	struct printbuf buf = PRINTBUF;
	int ret;

	pos = iter->pos;
	pos.offset &= ~(~0ULL << 56);
	genbits = iter->pos.offset & (~0ULL << 56);

	bch2_trans_iter_init(trans, &alloc_iter, BTREE_ID_alloc, pos, 0);

	if (fsck_err_on(!bch2_dev_bucket_exists(c, pos), c,
			"entry in %s btree for nonexistant dev:bucket %llu:%llu",
			bch2_btree_ids[iter->btree_id], pos.inode, pos.offset))
		goto delete;

	alloc_k = bch2_btree_iter_peek_slot(&alloc_iter);
	ret = bkey_err(alloc_k);
	if (ret)
		goto err;

	bch2_alloc_to_v4(alloc_k, &a);

	if (fsck_err_on(a.data_type != state ||
			(state == BCH_DATA_free &&
			 genbits != alloc_freespace_genbits(a)), c,
			"%s\n  incorrectly set in %s index (free %u, genbits %llu should be %llu)",
			(bch2_bkey_val_to_text(&buf, c, alloc_k), buf.buf),
			bch2_btree_ids[iter->btree_id],
			a.data_type == state,
			genbits >> 56, alloc_freespace_genbits(a) >> 56))
		goto delete;
out:
err:
fsck_err:
	bch2_trans_iter_exit(trans, &alloc_iter);
	printbuf_exit(&buf);
	return ret;
delete:
	ret = bch2_btree_delete_extent_at(trans, iter,
			iter->btree_id == BTREE_ID_freespace ? 1 : 0, 0);
	goto out;
}

int bch2_check_alloc_info(struct bch_fs *c)
{
	struct btree_trans trans;
	struct btree_iter iter, discard_iter, freespace_iter;
	struct bkey_s_c k;
	int ret = 0;

	bch2_trans_init(&trans, c, 0, 0);

	bch2_trans_iter_init(&trans, &iter, BTREE_ID_alloc, POS_MIN,
			     BTREE_ITER_PREFETCH);
	bch2_trans_iter_init(&trans, &discard_iter, BTREE_ID_need_discard, POS_MIN,
			     BTREE_ITER_PREFETCH);
	bch2_trans_iter_init(&trans, &freespace_iter, BTREE_ID_freespace, POS_MIN,
			     BTREE_ITER_PREFETCH);
	while (1) {
		ret = commit_do(&trans, NULL, NULL,
				      BTREE_INSERT_NOFAIL|
				      BTREE_INSERT_LAZY_RW,
			bch2_check_alloc_key(&trans, &iter,
					     &discard_iter,
					     &freespace_iter));
		if (ret)
			break;

		bch2_btree_iter_advance(&iter);
	}
	bch2_trans_iter_exit(&trans, &freespace_iter);
	bch2_trans_iter_exit(&trans, &discard_iter);
	bch2_trans_iter_exit(&trans, &iter);

	if (ret < 0)
		goto err;

	ret = for_each_btree_key_commit(&trans, iter,
			BTREE_ID_need_discard, POS_MIN,
			BTREE_ITER_PREFETCH, k,
			NULL, NULL, BTREE_INSERT_NOFAIL|BTREE_INSERT_LAZY_RW,
		bch2_check_discard_freespace_key(&trans, &iter)) ?:
	      for_each_btree_key_commit(&trans, iter,
			BTREE_ID_freespace, POS_MIN,
			BTREE_ITER_PREFETCH, k,
			NULL, NULL, BTREE_INSERT_NOFAIL|BTREE_INSERT_LAZY_RW,
		bch2_check_discard_freespace_key(&trans, &iter));
err:
	bch2_trans_exit(&trans);
	return ret < 0 ? ret : 0;
}

static int bch2_check_alloc_to_lru_ref(struct btree_trans *trans,
				       struct btree_iter *alloc_iter)
{
	struct bch_fs *c = trans->c;
	struct btree_iter lru_iter;
	struct bch_alloc_v4 a;
	struct bkey_s_c alloc_k, k;
	struct printbuf buf = PRINTBUF;
	struct printbuf buf2 = PRINTBUF;
	int ret;

	alloc_k = bch2_btree_iter_peek(alloc_iter);
	if (!alloc_k.k)
		return 0;

	ret = bkey_err(alloc_k);
	if (ret)
		return ret;

	bch2_alloc_to_v4(alloc_k, &a);

	if (a.data_type != BCH_DATA_cached)
		return 0;

	bch2_trans_iter_init(trans, &lru_iter, BTREE_ID_lru,
			     POS(alloc_k.k->p.inode, a.io_time[READ]), 0);

	k = bch2_btree_iter_peek_slot(&lru_iter);
	ret = bkey_err(k);
	if (ret)
		goto err;

	if (fsck_err_on(!a.io_time[READ], c,
			"cached bucket with read_time 0\n"
			"  %s",
		(printbuf_reset(&buf),
		 bch2_bkey_val_to_text(&buf, c, alloc_k), buf.buf)) ||
	    fsck_err_on(k.k->type != KEY_TYPE_lru ||
			le64_to_cpu(bkey_s_c_to_lru(k).v->idx) != alloc_k.k->p.offset, c,
			"incorrect/missing lru entry\n"
			"  %s\n"
			"  %s",
			(printbuf_reset(&buf),
			 bch2_bkey_val_to_text(&buf, c, alloc_k), buf.buf),
			(bch2_bkey_val_to_text(&buf2, c, k), buf2.buf))) {
		u64 read_time = a.io_time[READ];

		if (!a.io_time[READ])
			a.io_time[READ] = atomic64_read(&c->io_clock[READ].now);

		ret = bch2_lru_set(trans,
				   alloc_k.k->p.inode,
				   alloc_k.k->p.offset,
				   &a.io_time[READ]);
		if (ret)
			goto err;

		if (a.io_time[READ] != read_time) {
			struct bkey_i_alloc_v4 *a_mut =
				bch2_alloc_to_v4_mut(trans, alloc_k);
			ret = PTR_ERR_OR_ZERO(a_mut);
			if (ret)
				goto err;

			a_mut->v.io_time[READ] = a.io_time[READ];
			ret = bch2_trans_update(trans, alloc_iter,
						&a_mut->k_i, BTREE_TRIGGER_NORUN);
			if (ret)
				goto err;
		}
	}
err:
fsck_err:
	bch2_trans_iter_exit(trans, &lru_iter);
	printbuf_exit(&buf2);
	printbuf_exit(&buf);
	return ret;
}

int bch2_check_alloc_to_lru_refs(struct bch_fs *c)
{
	struct btree_trans trans;
	struct btree_iter iter;
	struct bkey_s_c k;
	int ret = 0;

	bch2_trans_init(&trans, c, 0, 0);

	for_each_btree_key_commit(&trans, iter, BTREE_ID_alloc,
			POS_MIN, BTREE_ITER_PREFETCH, k,
			NULL, NULL, BTREE_INSERT_NOFAIL|BTREE_INSERT_LAZY_RW,
		bch2_check_alloc_to_lru_ref(&trans, &iter));

	bch2_trans_exit(&trans);
	return ret < 0 ? ret : 0;
}

static int bch2_discard_one_bucket(struct btree_trans *trans,
				   struct btree_iter *need_discard_iter,
				   struct bpos *discard_pos_done,
				   u64 *seen,
				   u64 *open,
				   u64 *need_journal_commit,
				   u64 *discarded)
{
	struct bch_fs *c = trans->c;
	struct bpos pos = need_discard_iter->pos;
	struct btree_iter iter = { NULL };
	struct bkey_s_c k;
	struct bch_dev *ca;
	struct bkey_i_alloc_v4 *a;
	struct printbuf buf = PRINTBUF;
	bool did_discard = false;
	int ret = 0;

	ca = bch_dev_bkey_exists(c, pos.inode);
	if (!percpu_ref_tryget(&ca->io_ref)) {
		bch2_btree_iter_set_pos(need_discard_iter, POS(pos.inode + 1, 0));
		return 0;
	}

	if (bch2_bucket_is_open_safe(c, pos.inode, pos.offset)) {
		(*open)++;
		goto out;
	}

	if (bch2_bucket_needs_journal_commit(&c->buckets_waiting_for_journal,
			c->journal.flushed_seq_ondisk,
			pos.inode, pos.offset)) {
		(*need_journal_commit)++;
		goto out;
	}

	bch2_trans_iter_init(trans, &iter, BTREE_ID_alloc,
			     need_discard_iter->pos,
			     BTREE_ITER_CACHED);
	k = bch2_btree_iter_peek_slot(&iter);
	ret = bkey_err(k);
	if (ret)
		goto out;

	a = bch2_alloc_to_v4_mut(trans, k);
	ret = PTR_ERR_OR_ZERO(a);
	if (ret)
		goto out;

	if (BCH_ALLOC_V4_NEED_INC_GEN(&a->v)) {
		a->v.gen++;
		SET_BCH_ALLOC_V4_NEED_INC_GEN(&a->v, false);
		goto write;
	}

	if (bch2_trans_inconsistent_on(a->v.journal_seq > c->journal.flushed_seq_ondisk, trans,
			"clearing need_discard but journal_seq %llu > flushed_seq %llu\n"
			"%s",
			a->v.journal_seq,
			c->journal.flushed_seq_ondisk,
			(bch2_bkey_val_to_text(&buf, c, k), buf.buf))) {
		ret = -EIO;
		goto out;
	}

	if (bch2_trans_inconsistent_on(a->v.data_type != BCH_DATA_need_discard, trans,
			"bucket incorrectly set in need_discard btree\n"
			"%s",
			(bch2_bkey_val_to_text(&buf, c, k), buf.buf))) {
		ret = -EIO;
		goto out;
	}

	if (bkey_cmp(*discard_pos_done, iter.pos) &&
	    ca->mi.discard && !c->opts.nochanges) {
		/*
		 * This works without any other locks because this is the only
		 * thread that removes items from the need_discard tree
		 */
		bch2_trans_unlock(trans);
		blkdev_issue_discard(ca->disk_sb.bdev,
				     k.k->p.offset * ca->mi.bucket_size,
				     ca->mi.bucket_size,
				     GFP_KERNEL);

		ret = bch2_trans_relock(trans);
		if (ret)
			goto out;
	}

	*discard_pos_done = iter.pos;
	did_discard = true;

	SET_BCH_ALLOC_V4_NEED_DISCARD(&a->v, false);
	a->v.data_type = alloc_data_type(a->v, a->v.data_type);
write:
	ret =   bch2_trans_update(trans, &iter, &a->k_i, 0) ?:
		bch2_trans_commit(trans, NULL, NULL,
				  BTREE_INSERT_USE_RESERVE|BTREE_INSERT_NOFAIL);
	if (ret)
		goto out;

	if (did_discard) {
		this_cpu_inc(c->counters[BCH_COUNTER_bucket_discard]);
		(*discarded)++;
	}
out:
	bch2_trans_iter_exit(trans, &iter);
	percpu_ref_put(&ca->io_ref);
	printbuf_exit(&buf);
	return ret;
}

static void bch2_do_discards_work(struct work_struct *work)
{
	struct bch_fs *c = container_of(work, struct bch_fs, discard_work);
	struct btree_trans trans;
	struct btree_iter iter;
	struct bkey_s_c k;
	u64 seen = 0, open = 0, need_journal_commit = 0, discarded = 0;
	struct bpos discard_pos_done = POS_MAX;
	int ret;

	bch2_trans_init(&trans, c, 0, 0);

	/*
	 * We're doing the commit in bch2_discard_one_bucket instead of using
	 * for_each_btree_key_commit() so that we can increment counters after
	 * successful commit:
	 */
	ret = for_each_btree_key2(&trans, iter,
			BTREE_ID_need_discard, POS_MIN, 0, k,
		bch2_discard_one_bucket(&trans, &iter, &discard_pos_done,
					&seen,
					&open,
					&need_journal_commit,
					&discarded));

	bch2_trans_exit(&trans);

	if (need_journal_commit * 2 > seen)
		bch2_journal_flush_async(&c->journal, NULL);

	percpu_ref_put(&c->writes);

	trace_discard_buckets(c, seen, open, need_journal_commit, discarded,
			      bch2_err_str(ret));
}

void bch2_do_discards(struct bch_fs *c)
{
	if (percpu_ref_tryget_live(&c->writes) &&
	    !queue_work(system_long_wq, &c->discard_work))
		percpu_ref_put(&c->writes);
}

static int invalidate_one_bucket(struct btree_trans *trans,
				 struct btree_iter *lru_iter, struct bkey_s_c k,
				 unsigned dev_idx, s64 *nr_to_invalidate)
{
	struct bch_fs *c = trans->c;
	struct btree_iter alloc_iter = { NULL };
	struct bkey_i_alloc_v4 *a;
	struct bpos bucket;
	struct printbuf buf = PRINTBUF;
	unsigned cached_sectors;
	int ret = 0;

	if (*nr_to_invalidate <= 0 || k.k->p.inode != dev_idx)
		return 1;

	if (k.k->type != KEY_TYPE_lru) {
		prt_printf(&buf, "non lru key in lru btree:\n  ");
		bch2_bkey_val_to_text(&buf, c, k);

		if (!test_bit(BCH_FS_CHECK_LRUS_DONE, &c->flags)) {
			bch_err(c, "%s", buf.buf);
		} else {
			bch2_trans_inconsistent(trans, "%s", buf.buf);
			ret = -EINVAL;
		}

		goto out;
	}

	bucket = POS(dev_idx, le64_to_cpu(bkey_s_c_to_lru(k).v->idx));

	a = bch2_trans_start_alloc_update(trans, &alloc_iter, bucket);
	ret = PTR_ERR_OR_ZERO(a);
	if (ret)
		goto out;

	if (k.k->p.offset != alloc_lru_idx(a->v)) {
		prt_printf(&buf, "alloc key does not point back to lru entry when invalidating bucket:\n  ");
		bch2_bkey_val_to_text(&buf, c, bkey_i_to_s_c(&a->k_i));
		prt_printf(&buf, "\n  ");
		bch2_bkey_val_to_text(&buf, c, k);

		if (!test_bit(BCH_FS_CHECK_LRUS_DONE, &c->flags)) {
			bch_err(c, "%s", buf.buf);
		} else {
			bch2_trans_inconsistent(trans, "%s", buf.buf);
			ret = -EINVAL;
		}

		goto out;
	}

	if (!a->v.cached_sectors)
		bch_err(c, "invalidating empty bucket, confused");

	cached_sectors = a->v.cached_sectors;

	SET_BCH_ALLOC_V4_NEED_INC_GEN(&a->v, false);
	a->v.gen++;
	a->v.data_type		= 0;
	a->v.dirty_sectors	= 0;
	a->v.cached_sectors	= 0;
	a->v.io_time[READ]	= atomic64_read(&c->io_clock[READ].now);
	a->v.io_time[WRITE]	= atomic64_read(&c->io_clock[WRITE].now);

	ret =   bch2_trans_update(trans, &alloc_iter, &a->k_i,
				BTREE_TRIGGER_BUCKET_INVALIDATE) ?:
		bch2_trans_commit(trans, NULL, NULL,
				  BTREE_INSERT_USE_RESERVE|BTREE_INSERT_NOFAIL);
	if (ret)
		goto out;

	trace_and_count(c, bucket_invalidate, c, bucket.inode, bucket.offset, cached_sectors);
	--*nr_to_invalidate;
out:
	bch2_trans_iter_exit(trans, &alloc_iter);
	printbuf_exit(&buf);
	return ret;
}

static void bch2_do_invalidates_work(struct work_struct *work)
{
	struct bch_fs *c = container_of(work, struct bch_fs, invalidate_work);
	struct bch_dev *ca;
	struct btree_trans trans;
	struct btree_iter iter;
	struct bkey_s_c k;
	unsigned i;
	int ret = 0;

	bch2_trans_init(&trans, c, 0, 0);

	for_each_member_device(ca, c, i) {
		s64 nr_to_invalidate =
			should_invalidate_buckets(ca, bch2_dev_usage_read(ca));

		ret = for_each_btree_key2(&trans, iter, BTREE_ID_lru,
				POS(ca->dev_idx, 0), BTREE_ITER_INTENT, k,
			invalidate_one_bucket(&trans, &iter, k, ca->dev_idx, &nr_to_invalidate));

		if (ret < 0) {
			percpu_ref_put(&ca->ref);
			break;
		}
	}

	bch2_trans_exit(&trans);
	percpu_ref_put(&c->writes);
}

void bch2_do_invalidates(struct bch_fs *c)
{
	if (percpu_ref_tryget_live(&c->writes) &&
	    !queue_work(system_long_wq, &c->invalidate_work))
		percpu_ref_put(&c->writes);
}

static int bucket_freespace_init(struct btree_trans *trans, struct btree_iter *iter,
				 struct bkey_s_c k, struct bch_dev *ca)
{
	struct bch_alloc_v4 a;

	if (iter->pos.offset >= ca->mi.nbuckets)
		return 1;

	bch2_alloc_to_v4(k, &a);
	return bch2_bucket_do_index(trans, k, &a, true);
}

static int bch2_dev_freespace_init(struct bch_fs *c, struct bch_dev *ca)
{
	struct btree_trans trans;
	struct btree_iter iter;
	struct bkey_s_c k;
	struct bch_member *m;
	int ret;

	bch2_trans_init(&trans, c, 0, 0);

	ret = for_each_btree_key_commit(&trans, iter, BTREE_ID_alloc,
			POS(ca->dev_idx, ca->mi.first_bucket),
			BTREE_ITER_SLOTS|BTREE_ITER_PREFETCH, k,
			NULL, NULL, BTREE_INSERT_LAZY_RW,
		bucket_freespace_init(&trans, &iter, k, ca));

	bch2_trans_exit(&trans);

	if (ret < 0) {
		bch_err(ca, "error initializing free space: %s", bch2_err_str(ret));
		return ret;
	}

	mutex_lock(&c->sb_lock);
	m = bch2_sb_get_members(c->disk_sb.sb)->members + ca->dev_idx;
	SET_BCH_MEMBER_FREESPACE_INITIALIZED(m, true);
	mutex_unlock(&c->sb_lock);

	return 0;
}

int bch2_fs_freespace_init(struct bch_fs *c)
{
	struct bch_dev *ca;
	unsigned i;
	int ret = 0;
	bool doing_init = false;

	/*
	 * We can crash during the device add path, so we need to check this on
	 * every mount:
	 */

	for_each_member_device(ca, c, i) {
		if (ca->mi.freespace_initialized)
			continue;

		if (!doing_init) {
			bch_info(c, "initializing freespace");
			doing_init = true;
		}

		ret = bch2_dev_freespace_init(c, ca);
		if (ret) {
			percpu_ref_put(&ca->ref);
			return ret;
		}
	}

	if (doing_init) {
		mutex_lock(&c->sb_lock);
		bch2_write_super(c);
		mutex_unlock(&c->sb_lock);

		bch_verbose(c, "done initializing freespace");
	}

	return ret;
}

/* Bucket IO clocks: */

int bch2_bucket_io_time_reset(struct btree_trans *trans, unsigned dev,
			      size_t bucket_nr, int rw)
{
	struct bch_fs *c = trans->c;
	struct btree_iter iter;
	struct bkey_i_alloc_v4 *a;
	u64 now;
	int ret = 0;

	a = bch2_trans_start_alloc_update(trans, &iter,  POS(dev, bucket_nr));
	ret = PTR_ERR_OR_ZERO(a);
	if (ret)
		return ret;

	now = atomic64_read(&c->io_clock[rw].now);
	if (a->v.io_time[rw] == now)
		goto out;

	a->v.io_time[rw] = now;

	ret   = bch2_trans_update(trans, &iter, &a->k_i, 0) ?:
		bch2_trans_commit(trans, NULL, NULL, 0);
out:
	bch2_trans_iter_exit(trans, &iter);
	return ret;
}

/* Startup/shutdown (ro/rw): */

void bch2_recalc_capacity(struct bch_fs *c)
{
	struct bch_dev *ca;
	u64 capacity = 0, reserved_sectors = 0, gc_reserve;
	unsigned bucket_size_max = 0;
	unsigned long ra_pages = 0;
	unsigned i;

	lockdep_assert_held(&c->state_lock);

	for_each_online_member(ca, c, i) {
		struct backing_dev_info *bdi = ca->disk_sb.bdev->bd_disk->bdi;

		ra_pages += bdi->ra_pages;
	}

	bch2_set_ra_pages(c, ra_pages);

	for_each_rw_member(ca, c, i) {
		u64 dev_reserve = 0;

		/*
		 * We need to reserve buckets (from the number
		 * of currently available buckets) against
		 * foreground writes so that mainly copygc can
		 * make forward progress.
		 *
		 * We need enough to refill the various reserves
		 * from scratch - copygc will use its entire
		 * reserve all at once, then run against when
		 * its reserve is refilled (from the formerly
		 * available buckets).
		 *
		 * This reserve is just used when considering if
		 * allocations for foreground writes must wait -
		 * not -ENOSPC calculations.
		 */

		dev_reserve += ca->nr_btree_reserve * 2;
		dev_reserve += ca->mi.nbuckets >> 6; /* copygc reserve */

		dev_reserve += 1;	/* btree write point */
		dev_reserve += 1;	/* copygc write point */
		dev_reserve += 1;	/* rebalance write point */

		dev_reserve *= ca->mi.bucket_size;

		capacity += bucket_to_sector(ca, ca->mi.nbuckets -
					     ca->mi.first_bucket);

		reserved_sectors += dev_reserve * 2;

		bucket_size_max = max_t(unsigned, bucket_size_max,
					ca->mi.bucket_size);
	}

	gc_reserve = c->opts.gc_reserve_bytes
		? c->opts.gc_reserve_bytes >> 9
		: div64_u64(capacity * c->opts.gc_reserve_percent, 100);

	reserved_sectors = max(gc_reserve, reserved_sectors);

	reserved_sectors = min(reserved_sectors, capacity);

	c->capacity = capacity - reserved_sectors;

	c->bucket_size_max = bucket_size_max;

	/* Wake up case someone was waiting for buckets */
	closure_wake_up(&c->freelist_wait);
}

static bool bch2_dev_has_open_write_point(struct bch_fs *c, struct bch_dev *ca)
{
	struct open_bucket *ob;
	bool ret = false;

	for (ob = c->open_buckets;
	     ob < c->open_buckets + ARRAY_SIZE(c->open_buckets);
	     ob++) {
		spin_lock(&ob->lock);
		if (ob->valid && !ob->on_partial_list &&
		    ob->dev == ca->dev_idx)
			ret = true;
		spin_unlock(&ob->lock);
	}

	return ret;
}

/* device goes ro: */
void bch2_dev_allocator_remove(struct bch_fs *c, struct bch_dev *ca)
{
	unsigned i;

	/* First, remove device from allocation groups: */

	for (i = 0; i < ARRAY_SIZE(c->rw_devs); i++)
		clear_bit(ca->dev_idx, c->rw_devs[i].d);

	/*
	 * Capacity is calculated based off of devices in allocation groups:
	 */
	bch2_recalc_capacity(c);

	/* Next, close write points that point to this device... */
	for (i = 0; i < ARRAY_SIZE(c->write_points); i++)
		bch2_writepoint_stop(c, ca, &c->write_points[i]);

	bch2_writepoint_stop(c, ca, &c->copygc_write_point);
	bch2_writepoint_stop(c, ca, &c->rebalance_write_point);
	bch2_writepoint_stop(c, ca, &c->btree_write_point);

	mutex_lock(&c->btree_reserve_cache_lock);
	while (c->btree_reserve_cache_nr) {
		struct btree_alloc *a =
			&c->btree_reserve_cache[--c->btree_reserve_cache_nr];

		bch2_open_buckets_put(c, &a->ob);
	}
	mutex_unlock(&c->btree_reserve_cache_lock);

	while (1) {
		struct open_bucket *ob;

		spin_lock(&c->freelist_lock);
		if (!ca->open_buckets_partial_nr) {
			spin_unlock(&c->freelist_lock);
			break;
		}
		ob = c->open_buckets +
			ca->open_buckets_partial[--ca->open_buckets_partial_nr];
		ob->on_partial_list = false;
		spin_unlock(&c->freelist_lock);

		bch2_open_bucket_put(c, ob);
	}

	bch2_ec_stop_dev(c, ca);

	/*
	 * Wake up threads that were blocked on allocation, so they can notice
	 * the device can no longer be removed and the capacity has changed:
	 */
	closure_wake_up(&c->freelist_wait);

	/*
	 * journal_res_get() can block waiting for free space in the journal -
	 * it needs to notice there may not be devices to allocate from anymore:
	 */
	wake_up(&c->journal.wait);

	/* Now wait for any in flight writes: */

	closure_wait_event(&c->open_buckets_wait,
			   !bch2_dev_has_open_write_point(c, ca));
}

/* device goes rw: */
void bch2_dev_allocator_add(struct bch_fs *c, struct bch_dev *ca)
{
	unsigned i;

	for (i = 0; i < ARRAY_SIZE(c->rw_devs); i++)
		if (ca->mi.data_allowed & (1 << i))
			set_bit(ca->dev_idx, c->rw_devs[i].d);
}

void bch2_fs_allocator_background_init(struct bch_fs *c)
{
	spin_lock_init(&c->freelist_lock);
	INIT_WORK(&c->discard_work, bch2_do_discards_work);
	INIT_WORK(&c->invalidate_work, bch2_do_invalidates_work);
}
