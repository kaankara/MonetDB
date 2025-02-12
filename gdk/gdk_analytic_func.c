/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0.  If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Copyright 1997 - July 2008 CWI, August 2008 - 2019 MonetDB B.V.
 */

#include "monetdb_config.h"
#include "gdk.h"
#include "gdk_analytic.h"
#include "gdk_calc_private.h"

#define ANALYTICAL_DIFF_IMP(TPE)				\
	do {							\
		TPE *bp = (TPE*)Tloc(b, 0);			\
		TPE prev = *bp, *end = bp + cnt;		\
		if (np) {					\
			for (; bp < end; bp++, rb++, np++) {	\
				*rb = *np;			\
				if (*bp != prev) {		\
					*rb = TRUE;		\
					prev = *bp;		\
				}				\
			}					\
		} else {					\
			for (; bp < end; bp++, rb++) {		\
				if (*bp == prev) { \
					*rb = FALSE;		\
				} else {		\
					*rb = TRUE;	\
					prev = *bp;	\
				} \
			}					\
		}						\
	} while (0)

/* We use NaN for floating point null values, which always output false on equality tests */
#define ANALYTICAL_DIFF_FLOAT_IMP(TPE)				\
	do {							\
		TPE *bp = (TPE*)Tloc(b, 0);			\
		TPE prev = *bp, *end = bp + cnt;		\
		if (np) {					\
			for (; bp < end; bp++, rb++, np++) {	\
				*rb = *np;			\
				if (*bp != prev && (!is_##TPE##_nil(*bp) || !is_##TPE##_nil(prev))) { \
					*rb = TRUE;		\
					prev = *bp;		\
				}				\
			}					\
		} else {					\
			for (; bp < end; bp++, rb++) {		\
				if (*bp == prev || (is_##TPE##_nil(*bp) && is_##TPE##_nil(prev))) { \
					*rb = FALSE; \
				} else {		\
					*rb = TRUE;	\
					prev = *bp;	\
				} \
			}					\
		}						\
	} while (0)

gdk_return
GDKanalyticaldiff(BAT *r, BAT *b, BAT *p, int tpe)
{
	BUN i, cnt = BATcount(b);
	bit *restrict rb = (bit *) Tloc(r, 0), *restrict np = p ? (bit *) Tloc(p, 0) : NULL;

	switch (tpe) {
	case TYPE_bit:
		ANALYTICAL_DIFF_IMP(bit);
		break;
	case TYPE_bte:
		ANALYTICAL_DIFF_IMP(bte);
		break;
	case TYPE_sht:
		ANALYTICAL_DIFF_IMP(sht);
		break;
	case TYPE_int:
		ANALYTICAL_DIFF_IMP(int);
		break;
	case TYPE_lng:
		ANALYTICAL_DIFF_IMP(lng);
		break;
#ifdef HAVE_HGE
	case TYPE_hge:
		ANALYTICAL_DIFF_IMP(hge);
		break;
#endif
	case TYPE_flt: {
		if (b->tnonil) {
			ANALYTICAL_DIFF_IMP(flt);
		} else { /* Because of NaN values, use this path */
			ANALYTICAL_DIFF_FLOAT_IMP(flt);
		}
	} break;
	case TYPE_dbl: {
		if (b->tnonil) {
			ANALYTICAL_DIFF_IMP(dbl);
		} else { /* Because of NaN values, use this path */
			ANALYTICAL_DIFF_FLOAT_IMP(dbl);
		}
	} break;
	default:{
		BATiter it = bat_iterator(b);
		ptr v = BUNtail(it, 0), next;
		int (*atomcmp) (const void *, const void *) = ATOMcompare(tpe);
		if (np) {
			for (i = 0; i < cnt; i++, rb++, np++) {
				*rb = *np;
				next = BUNtail(it, i);
				if (atomcmp(v, next) != 0) {
					*rb = TRUE;
					v = next;
				}
			}
		} else {
			for (i = 0; i < cnt; i++, rb++) {
				next = BUNtail(it, i);
				if (atomcmp(v, next) != 0) {
					*rb = TRUE;
					v = next;
				} else {
					*rb = FALSE;
				}
			}
		}
	}
	}
	BATsetcount(r, cnt);
	r->tnonil = true;
	r->tnil = false;
	return GDK_SUCCEED;
}

#define NTILE_CALC					\
	do {						\
		if (bval >= ncnt) {			\
			j = 1;				\
			for (; rb < rp; j++, rb++)	\
				*rb = j;		\
		} else if (ncnt % bval == 0) {		\
			buckets = ncnt / bval;		\
			for (; rb < rp; i++, rb++) {	\
				if (i == buckets) {	\
					j++;		\
					i = 0;		\
				}			\
				*rb = j;		\
			}				\
		} else {				\
			buckets = ncnt / bval;		\
			for (; rb < rp; i++, rb++) {	\
				*rb = j;		\
				if (i == buckets) {	\
					j++;		\
					i = 0;		\
				}			\
			}				\
		}					\
	} while (0)

#define ANALYTICAL_NTILE_IMP(TPE)				\
	do {							\
		TPE j = 1, *rp, *rb, val = *(TPE*) ntile;	\
		BUN bval = (BUN) val;				\
		rb = rp = (TPE*)Tloc(r, 0);			\
		if (is_##TPE##_nil(val)) {			\
			TPE *end = rp + cnt;			\
			has_nils = true;			\
			for (; rp < end; rp++)			\
				*rp = TPE##_nil;		\
		} else if (p) {					\
			pnp = np = (bit*)Tloc(p, 0);		\
			end = np + cnt;				\
			for (; np < end; np++) {		\
				if (*np) {			\
					i = 0;			\
					j = 1;			\
					ncnt = np - pnp;	\
					rp += ncnt;		\
					NTILE_CALC;		\
					pnp = np;		\
				}				\
			}					\
			i = 0;					\
			j = 1;					\
			ncnt = np - pnp;			\
			rp += ncnt;				\
			NTILE_CALC;				\
		} else {					\
			rp += cnt;				\
			NTILE_CALC;				\
		}						\
	} while (0)

gdk_return
GDKanalyticalntile(BAT *r, BAT *b, BAT *p, int tpe, const void *restrict ntile)
{
	BUN cnt = BATcount(b), ncnt = cnt, buckets, i = 0;
	bit *np, *pnp, *end;
	bool has_nils = false;

	assert(ntile);

	switch (tpe) {
	case TYPE_bte:
		ANALYTICAL_NTILE_IMP(bte);
		break;
	case TYPE_sht:
		ANALYTICAL_NTILE_IMP(sht);
		break;
	case TYPE_int:
		ANALYTICAL_NTILE_IMP(int);
		break;
	case TYPE_lng:
		ANALYTICAL_NTILE_IMP(lng);
		break;
#ifdef HAVE_HGE
	case TYPE_hge:
		ANALYTICAL_NTILE_IMP(hge);
		break;
#endif
	default:
		GDKerror("GDKanalyticalntile: type %s not supported.\n", ATOMname(tpe));
		return GDK_FAIL;
	}
	BATsetcount(r, cnt);
	r->tnonil = !has_nils;
	r->tnil = has_nils;
	return GDK_SUCCEED;
}

#define ANALYTICAL_FIRST_IMP(TPE)				\
	do {							\
		TPE *bp, *bs, *be, curval, *restrict rb;	\
		bp = (TPE*)Tloc(b, 0);				\
		rb = (TPE*)Tloc(r, 0);				\
		for (; i < cnt; i++, rb++) {			\
			bs = bp + start[i];			\
			be = bp + end[i];			\
			curval = (be > bs) ? *bs : TPE##_nil;	\
			*rb = curval;				\
			if (is_##TPE##_nil(curval))		\
				has_nils = true;		\
		}						\
	} while (0)

gdk_return
GDKanalyticalfirst(BAT *r, BAT *b, BAT *s, BAT *e, int tpe)
{
	BUN i = 0, cnt = BATcount(b);
	lng *restrict start, *restrict end;
	bool has_nils = false;

	assert(s && e);
	start = (lng *) Tloc(s, 0);
	end = (lng *) Tloc(e, 0);

	switch (tpe) {
	case TYPE_bit:
		ANALYTICAL_FIRST_IMP(bit);
		break;
	case TYPE_bte:
		ANALYTICAL_FIRST_IMP(bte);
		break;
	case TYPE_sht:
		ANALYTICAL_FIRST_IMP(sht);
		break;
	case TYPE_int:
		ANALYTICAL_FIRST_IMP(int);
		break;
	case TYPE_lng:
		ANALYTICAL_FIRST_IMP(lng);
		break;
#ifdef HAVE_HGE
	case TYPE_hge:
		ANALYTICAL_FIRST_IMP(hge);
		break;
#endif
	case TYPE_flt:
		ANALYTICAL_FIRST_IMP(flt);
		break;
	case TYPE_dbl:
		ANALYTICAL_FIRST_IMP(dbl);
		break;
	default:{
		const void *restrict nil = ATOMnilptr(tpe);
		int (*atomcmp) (const void *, const void *) = ATOMcompare(tpe);
		BATiter bpi = bat_iterator(b);
		void *curval;

		for (; i < cnt; i++) {
			curval = (end[i] > start[i]) ? BUNtail(bpi, (BUN) start[i]) : (void *) nil;
			if (BUNappend(r, curval, false) != GDK_SUCCEED)
				goto allocation_error;
			if (atomcmp(curval, nil) == 0)
				has_nils = true;
		}
	}
	}
	BATsetcount(r, cnt);
	r->tnonil = !has_nils;
	r->tnil = has_nils;
	return GDK_SUCCEED;
      allocation_error:
	GDKerror("GDKanalyticalfirst: malloc failure\n");
	return GDK_FAIL;
}

#define ANALYTICAL_LAST_IMP(TPE)					\
	do {								\
		TPE *bp, *bs, *be, curval, *restrict rb;		\
		bp = (TPE*)Tloc(b, 0);					\
		rb = (TPE*)Tloc(r, 0);					\
		for (; i<cnt; i++, rb++) {				\
			bs = bp + start[i];				\
			be = bp + end[i];				\
			curval = (be > bs) ? *(be - 1) : TPE##_nil;	\
			*rb = curval;					\
			if (is_##TPE##_nil(curval))			\
				has_nils = true;			\
		}							\
	} while (0)

gdk_return
GDKanalyticallast(BAT *r, BAT *b, BAT *s, BAT *e, int tpe)
{
	BUN i = 0, cnt = BATcount(b);
	lng *restrict start, *restrict end;
	bool has_nils = false;

	assert(s && e);
	start = (lng *) Tloc(s, 0);
	end = (lng *) Tloc(e, 0);

	switch (tpe) {
	case TYPE_bit:
		ANALYTICAL_LAST_IMP(bit);
		break;
	case TYPE_bte:
		ANALYTICAL_LAST_IMP(bte);
		break;
	case TYPE_sht:
		ANALYTICAL_LAST_IMP(sht);
		break;
	case TYPE_int:
		ANALYTICAL_LAST_IMP(int);
		break;
	case TYPE_lng:
		ANALYTICAL_LAST_IMP(lng);
		break;
#ifdef HAVE_HGE
	case TYPE_hge:
		ANALYTICAL_LAST_IMP(hge);
		break;
#endif
	case TYPE_flt:
		ANALYTICAL_LAST_IMP(flt);
		break;
	case TYPE_dbl:
		ANALYTICAL_LAST_IMP(dbl);
		break;
	default:{
		const void *restrict nil = ATOMnilptr(tpe);
		int (*atomcmp) (const void *, const void *) = ATOMcompare(tpe);
		BATiter bpi = bat_iterator(b);
		void *curval;

		for (; i < cnt; i++) {
			curval = (end[i] > start[i]) ? BUNtail(bpi, (BUN) (end[i] - 1)) : (void *) nil;
			if (BUNappend(r, curval, false) != GDK_SUCCEED)
				goto allocation_error;
			if (atomcmp(curval, nil) == 0)
				has_nils = true;
		}
	}
	}
	BATsetcount(r, cnt);
	r->tnonil = !has_nils;
	r->tnil = has_nils;
	return GDK_SUCCEED;
      allocation_error:
	GDKerror("GDKanalyticallast: malloc failure\n");
	return GDK_FAIL;
}

#define ANALYTICAL_NTHVALUE_IMP_SINGLE_FIXED(TPE1)			\
	do {								\
		TPE1 *bp = (TPE1*)Tloc(b, 0), *bs, *be, curval, *restrict rb = (TPE1*)Tloc(r, 0); \
		if (is_lng_nil(nth)) {					\
			has_nils = true;				\
			for (; i < cnt; i++, rb++)			\
				*rb = TPE1##_nil;			\
		} else {						\
			nth--;						\
			for (; i < cnt; i++, rb++) {			\
				bs = bp + start[i];			\
				be = bp + end[i];			\
				curval = (be > bs && nth < (end[i] - start[i])) ? *(bs + nth) : TPE1##_nil; \
				*rb = curval;				\
				if (is_##TPE1##_nil(curval))		\
					has_nils = true;		\
			}						\
		}							\
	} while (0)

#define ANALYTICAL_NTHVALUE_IMP_MULTI_FIXED(TPE1, TPE2, TPE3)			\
	do {								\
		TPE2 *restrict lp = (TPE2*)Tloc(l, 0);			\
		for (; i < cnt; i++, rb++) {				\
			TPE2 lnth = lp[i];				\
			bs = bp + start[i];				\
			be = bp + end[i];				\
			if (is_##TPE2##_nil(lnth) || be <= bs || (TPE3)(lnth - 1) > (TPE3)(end[i] - start[i])) \
				curval = TPE1##_nil;			\
			else						\
				curval = *(bs + lnth - 1);		\
			*rb = curval;					\
			if (is_##TPE1##_nil(curval))			\
				has_nils = true;			\
		}							\
	} while (0)

#ifdef HAVE_HGE
#define ANALYTICAL_NTHVALUE_CALC_FIXED_HGE(TPE1)			\
	case TYPE_hge:							\
		ANALYTICAL_NTHVALUE_IMP_MULTI_FIXED(TPE1, hge, hge); \
		break;
#else
#define ANALYTICAL_NTHVALUE_CALC_FIXED_HGE(TPE1)
#endif

#define ANALYTICAL_NTHVALUE_CALC_FIXED(TPE1)				\
	do {								\
		TPE1 *bp, *bs, *be, curval, *restrict rb;		\
		bp = (TPE1*)Tloc(b, 0);					\
		rb = (TPE1*)Tloc(r, 0);					\
		switch (tp2) {						\
		case TYPE_bte:						\
			ANALYTICAL_NTHVALUE_IMP_MULTI_FIXED(TPE1, bte, lng); \
			break;						\
		case TYPE_sht:						\
			ANALYTICAL_NTHVALUE_IMP_MULTI_FIXED(TPE1, sht, lng); \
			break;						\
		case TYPE_int:						\
			ANALYTICAL_NTHVALUE_IMP_MULTI_FIXED(TPE1, int, lng); \
			break;						\
		case TYPE_lng:						\
			ANALYTICAL_NTHVALUE_IMP_MULTI_FIXED(TPE1, lng, lng); \
			break;						\
		ANALYTICAL_NTHVALUE_CALC_FIXED_HGE(TPE1) \
		default:						\
			goto nosupport;					\
		}							\
	} while (0)

#define ANALYTICAL_NTHVALUE_IMP_MULTI_VARSIZED(TPE1, TPE2)			\
	do {								\
		TPE1 *restrict lp = (TPE1*)Tloc(l, 0);			\
		for (; i < cnt; i++) {					\
			TPE1 lnth = lp[i];				\
			if (is_##TPE1##_nil(lnth) || end[i] <= start[i] || (TPE2)(lnth - 1) > (TPE2)(end[i] - start[i])) \
				curval = (void *) nil;			\
			else						\
				curval = BUNtail(bpi, (BUN) (start[i] + lnth - 1)); \
			if (BUNappend(r, curval, false) != GDK_SUCCEED) \
				goto allocation_error;			\
			if (atomcmp(curval, nil) == 0)			\
				has_nils = true;			\
		}							\
	} while (0)

gdk_return
GDKanalyticalnthvalue(BAT *r, BAT *b, BAT *s, BAT *e, BAT *l, const void *restrict bound, int tp1, int tp2)
{
	BUN i = 0, cnt = BATcount(b);
	lng *restrict start, *restrict end, nth = 0;
	bool has_nils = false;
	const void *restrict nil = ATOMnilptr(tp1);
	int (*atomcmp) (const void *, const void *) = ATOMcompare(tp1);
	void *curval;

	assert(s && e && ((l && !bound) || (!l && bound)));
	start = (lng *) Tloc(s, 0);
	end = (lng *) Tloc(e, 0);

	if (bound) {
		switch (tp2) {
		case TYPE_bte:{
			bte val = *(bte *) bound;
			nth = !is_bte_nil(val) ? (lng) val : lng_nil;
		} break;
		case TYPE_sht:{
			sht val = *(sht *) bound;
			nth = !is_sht_nil(val) ? (lng) val : lng_nil;
		} break;
		case TYPE_int:{
			int val = *(int *) bound;
			nth = !is_int_nil(val) ? (lng) val : lng_nil;
		} break;
		case TYPE_lng:{
			nth = *(lng *) bound;
		} break;
#ifdef HAVE_HGE
		case TYPE_hge:{
			hge nval = *(hge *) bound;
			nth = is_hge_nil(nval) ? lng_nil : (nval > (hge) GDK_lng_max) ? GDK_lng_max : (lng) nval;
		} break;
#endif
		default:
			goto nosupport;
		}
		switch (tp1) {
		case TYPE_bit:
			ANALYTICAL_NTHVALUE_IMP_SINGLE_FIXED(bit);
			break;
		case TYPE_bte:
			ANALYTICAL_NTHVALUE_IMP_SINGLE_FIXED(bte);
			break;
		case TYPE_sht:
			ANALYTICAL_NTHVALUE_IMP_SINGLE_FIXED(sht);
			break;
		case TYPE_int:
			ANALYTICAL_NTHVALUE_IMP_SINGLE_FIXED(int);
			break;
		case TYPE_lng:
			ANALYTICAL_NTHVALUE_IMP_SINGLE_FIXED(lng);
			break;
#ifdef HAVE_HGE
		case TYPE_hge:
			ANALYTICAL_NTHVALUE_IMP_SINGLE_FIXED(hge);
			break;
#endif
		case TYPE_flt:
			ANALYTICAL_NTHVALUE_IMP_SINGLE_FIXED(flt);
			break;
		case TYPE_dbl:
			ANALYTICAL_NTHVALUE_IMP_SINGLE_FIXED(dbl);
			break;
		default:{
			BATiter bpi = bat_iterator(b);
			if (is_lng_nil(nth)) {
				has_nils = true;
				for (; i < cnt; i++)
					if (BUNappend(r, nil, false) != GDK_SUCCEED)
						goto allocation_error;
			} else {
				nth--;
				for (; i < cnt; i++) {
					curval = (end[i] > start[i] && nth < (end[i] - start[i])) ? BUNtail(bpi, (BUN) (start[i] + nth)) : (void *) nil;
					if (BUNappend(r, curval, false) != GDK_SUCCEED)
						goto allocation_error;
					if (atomcmp(curval, nil) == 0)
						has_nils = true;
				}
			}
		}
		}
	} else {
		switch (tp1) {
		case TYPE_bit:
			ANALYTICAL_NTHVALUE_CALC_FIXED(bit);
			break;
		case TYPE_bte:
			ANALYTICAL_NTHVALUE_CALC_FIXED(bte);
			break;
		case TYPE_sht:
			ANALYTICAL_NTHVALUE_CALC_FIXED(sht);
			break;
		case TYPE_int:
			ANALYTICAL_NTHVALUE_CALC_FIXED(int);
			break;
		case TYPE_lng:
			ANALYTICAL_NTHVALUE_CALC_FIXED(lng);
			break;
#ifdef HAVE_HGE
		case TYPE_hge:
			ANALYTICAL_NTHVALUE_CALC_FIXED(hge);
			break;
#endif
		case TYPE_flt:
			ANALYTICAL_NTHVALUE_CALC_FIXED(flt);
			break;
		case TYPE_dbl:
			ANALYTICAL_NTHVALUE_CALC_FIXED(dbl);
			break;
		default:{
			BATiter bpi = bat_iterator(b);
			switch (tp2) {
			case TYPE_bte:
				ANALYTICAL_NTHVALUE_IMP_MULTI_VARSIZED(bte, lng);
				break;
			case TYPE_sht:
				ANALYTICAL_NTHVALUE_IMP_MULTI_VARSIZED(sht, lng);
				break;
			case TYPE_int:
				ANALYTICAL_NTHVALUE_IMP_MULTI_VARSIZED(int, lng);
				break;
			case TYPE_lng:
				ANALYTICAL_NTHVALUE_IMP_MULTI_VARSIZED(lng, lng);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				ANALYTICAL_NTHVALUE_IMP_MULTI_VARSIZED(hge, hge);
				break;
#endif
			default:
				goto nosupport;
			}
		}
		}
	}
	BATsetcount(r, cnt);
	r->tnonil = !has_nils;
	r->tnil = has_nils;
	return GDK_SUCCEED;
      allocation_error:
	GDKerror("GDKanalyticalnthvalue: malloc failure\n");
	return GDK_FAIL;
      nosupport:
	GDKerror("GDKanalyticalnthvalue: type %s not supported for the nth_value.\n", ATOMname(tp2));
	return GDK_FAIL;
}

#define ANALYTICAL_LAG_CALC(TPE)				\
	do {							\
		for (i = 0; i < lag && rb < rp; i++, rb++)	\
			*rb = def;				\
		if (lag > 0 && is_##TPE##_nil(def))		\
			has_nils = true;			\
		for (; rb < rp; rb++, bp++) {			\
			next = *bp;				\
			*rb = next;				\
			if (is_##TPE##_nil(next))		\
				has_nils = true;		\
		}						\
	} while (0)

#define ANALYTICAL_LAG_IMP(TPE)						\
	do {								\
		TPE *rp, *rb, *bp, *nbp, *rend,				\
			def = *((TPE *) default_value), next;		\
		bp = (TPE*)Tloc(b, 0);					\
		rb = rp = (TPE*)Tloc(r, 0);				\
		rend = rb + cnt;					\
		if (lag == BUN_NONE) {					\
			has_nils = true;				\
			for (; rb < rend; rb++)				\
				*rb = TPE##_nil;			\
		} else if (p) {						\
			pnp = np = (bit*)Tloc(p, 0);			\
			end = np + cnt;					\
			for (; np < end; np++) {			\
				if (*np) {				\
					ncnt = (np - pnp);		\
					rp += ncnt;			\
					nbp = bp + ncnt; \
					ANALYTICAL_LAG_CALC(TPE);	\
					bp = nbp; \
					pnp = np;			\
				}					\
			}						\
			rp += (np - pnp);				\
			ANALYTICAL_LAG_CALC(TPE);			\
		} else {						\
			rp += cnt;					\
			ANALYTICAL_LAG_CALC(TPE);			\
		}							\
	} while (0)

#define ANALYTICAL_LAG_OTHERS						\
	do {								\
		for (i = 0; i < lag && k < j; i++, k++) {		\
			if (BUNappend(r, default_value, false) != GDK_SUCCEED) \
				goto allocation_error;			\
		}							\
		if (lag > 0 && atomcmp(default_value, nil) == 0)	\
			has_nils = true;				\
		for (l = k - lag; k < j; k++, l++) {			\
			curval = BUNtail(bpi, l);			\
			if (BUNappend(r, curval, false) != GDK_SUCCEED)	\
				goto allocation_error;			\
			if (atomcmp(curval, nil) == 0)			\
				has_nils = true;			\
		}							\
	} while (0)

gdk_return
GDKanalyticallag(BAT *r, BAT *b, BAT *p, BUN lag, const void *restrict default_value, int tpe)
{
	int (*atomcmp) (const void *, const void *);
	const void *restrict nil;
	BUN i = 0, j = 0, k = 0, l = 0, ncnt, cnt = BATcount(b);
	bit *np, *pnp, *end;
	bool has_nils = false;

	assert(default_value);

	switch (tpe) {
	case TYPE_bit:
		ANALYTICAL_LAG_IMP(bit);
		break;
	case TYPE_bte:
		ANALYTICAL_LAG_IMP(bte);
		break;
	case TYPE_sht:
		ANALYTICAL_LAG_IMP(sht);
		break;
	case TYPE_int:
		ANALYTICAL_LAG_IMP(int);
		break;
	case TYPE_lng:
		ANALYTICAL_LAG_IMP(lng);
		break;
#ifdef HAVE_HGE
	case TYPE_hge:
		ANALYTICAL_LAG_IMP(hge);
		break;
#endif
	case TYPE_flt:
		ANALYTICAL_LAG_IMP(flt);
		break;
	case TYPE_dbl:
		ANALYTICAL_LAG_IMP(dbl);
		break;
	default:{
		BATiter bpi = bat_iterator(b);
		const void *restrict curval;
		nil = ATOMnilptr(tpe);
		atomcmp = ATOMcompare(tpe);
		if (lag == BUN_NONE) {
			has_nils = true;
			for (j = 0; j < cnt; j++) {
				if (BUNappend(r, nil, false) != GDK_SUCCEED)
					goto allocation_error;
			}
		} else if (p) {
			pnp = np = (bit *) Tloc(p, 0);
			end = np + cnt;
			for (; np < end; np++) {
				if (*np) {
					j += (np - pnp);
					ANALYTICAL_LAG_OTHERS;
					pnp = np;
				}
			}
			j += (np - pnp);
			ANALYTICAL_LAG_OTHERS;
		} else {
			j += cnt;
			ANALYTICAL_LAG_OTHERS;
		}
	}
	}
	BATsetcount(r, cnt);
	r->tnonil = !has_nils;
	r->tnil = has_nils;
	return GDK_SUCCEED;
      allocation_error:
	GDKerror("GDKanalyticallag: malloc failure\n");
	return GDK_FAIL;
}

#define LEAD_CALC(TPE)						\
	do {							\
		if (lead < ncnt) {				\
			bp += lead;				\
			l = ncnt - lead;			\
			for (i = 0; i < l; i++, rb++, bp++) {	\
				next = *bp;			\
				*rb = next;			\
				if (is_##TPE##_nil(next))	\
					has_nils = true;	\
			}					\
		} else {					\
			bp += ncnt;				\
		}						\
		for (;rb < rp; rb++)				\
			*rb = def;				\
		if (lead > 0 && is_##TPE##_nil(def))		\
			has_nils = true;			\
	} while (0)

#define ANALYTICAL_LEAD_IMP(TPE)				\
	do {							\
		TPE *rp, *rb, *bp, *rend,			\
			def = *((TPE *) default_value), next;	\
		bp = (TPE*)Tloc(b, 0);				\
		rb = rp = (TPE*)Tloc(r, 0);			\
		rend = rb + cnt;				\
		if (lead == BUN_NONE) {				\
			has_nils = true;			\
			for (; rb < rend; rb++)			\
				*rb = TPE##_nil;		\
		} else if (p) {					\
			pnp = np = (bit*)Tloc(p, 0);		\
			end = np + cnt;				\
			for (; np < end; np++) {		\
				if (*np) {			\
					ncnt = (np - pnp);	\
					rp += ncnt;		\
					LEAD_CALC(TPE);		\
					pnp = np;		\
				}				\
			}					\
			ncnt = (np - pnp);			\
			rp += ncnt;				\
			LEAD_CALC(TPE);				\
		} else {					\
			ncnt = cnt;				\
			rp += ncnt;				\
			LEAD_CALC(TPE);				\
		}						\
	} while (0)

#define ANALYTICAL_LEAD_OTHERS						\
	do {								\
		j += ncnt;						\
		if (lead < ncnt) {					\
			m = ncnt - lead;				\
			for (i = 0,n = k + lead; i < m; i++, n++) {	\
				curval = BUNtail(bpi, n);		\
				if (BUNappend(r, curval, false) != GDK_SUCCEED)	\
					goto allocation_error;		\
				if (atomcmp(curval, nil) == 0)		\
					has_nils = true;		\
			}						\
			k += i;						\
		}							\
		for (; k < j; k++) {					\
			if (BUNappend(r, default_value, false) != GDK_SUCCEED) \
				goto allocation_error;			\
		}							\
		if (lead > 0 && atomcmp(default_value, nil) == 0)	\
			has_nils = true;				\
	} while (0)

gdk_return
GDKanalyticallead(BAT *r, BAT *b, BAT *p, BUN lead, const void *restrict default_value, int tpe)
{
	int (*atomcmp) (const void *, const void *);
	const void *restrict nil;
	BUN i = 0, j = 0, k = 0, l = 0, ncnt, cnt = BATcount(b);
	bit *np, *pnp, *end;
	bool has_nils = false;

	assert(default_value);

	switch (tpe) {
	case TYPE_bit:
		ANALYTICAL_LEAD_IMP(bit);
		break;
	case TYPE_bte:
		ANALYTICAL_LEAD_IMP(bte);
		break;
	case TYPE_sht:
		ANALYTICAL_LEAD_IMP(sht);
		break;
	case TYPE_int:
		ANALYTICAL_LEAD_IMP(int);
		break;
	case TYPE_lng:
		ANALYTICAL_LEAD_IMP(lng);
		break;
#ifdef HAVE_HGE
	case TYPE_hge:
		ANALYTICAL_LEAD_IMP(hge);
		break;
#endif
	case TYPE_flt:
		ANALYTICAL_LEAD_IMP(flt);
		break;
	case TYPE_dbl:
		ANALYTICAL_LEAD_IMP(dbl);
		break;
	default:{
		BUN m = 0, n = 0;
		BATiter bpi = bat_iterator(b);
		const void *restrict curval;
		nil = ATOMnilptr(tpe);
		atomcmp = ATOMcompare(tpe);
		if (lead == BUN_NONE) {
			has_nils = true;
			for (j = 0; j < cnt; j++) {
				if (BUNappend(r, nil, false) != GDK_SUCCEED)
					goto allocation_error;
			}
		} else if (p) {
			pnp = np = (bit *) Tloc(p, 0);
			end = np + cnt;
			for (; np < end; np++) {
				if (*np) {
					ncnt = (np - pnp);
					ANALYTICAL_LEAD_OTHERS;
					pnp = np;
				}
			}
			ncnt = (np - pnp);
			ANALYTICAL_LEAD_OTHERS;
		} else {
			ncnt = cnt;
			ANALYTICAL_LEAD_OTHERS;
		}
	}
	}
	BATsetcount(r, cnt);
	r->tnonil = !has_nils;
	r->tnil = has_nils;
	return GDK_SUCCEED;
      allocation_error:
	GDKerror("GDKanalyticallead: malloc failure\n");
	return GDK_FAIL;
}

#define ANALYTICAL_MIN_MAX_CALC(TPE, OP)				\
	do {								\
		TPE *bp = (TPE*)Tloc(b, 0), *bs, *be, v,		\
			curval = TPE##_nil, *restrict rb = (TPE*)Tloc(r, 0); \
		for (; i < cnt; i++, rb++) {				\
			bs = bp + start[i];				\
			be = bp + end[i];				\
			for (; bs < be; bs++) {				\
				v = *bs;				\
				if (!is_##TPE##_nil(v)) {		\
					if (is_##TPE##_nil(curval))	\
						curval = v;             \
					else				\
						curval = OP(v, curval); \
				}					\
			}						\
			*rb = curval;					\
			if (is_##TPE##_nil(curval))			\
				has_nils = true;			\
			else						\
				curval = TPE##_nil;			\
		}							\
	} while (0)

#ifdef HAVE_HGE
#define ANALYTICAL_MIN_MAX_LIMIT(OP)			\
	case TYPE_hge:					\
		ANALYTICAL_MIN_MAX_CALC(hge, OP);	\
	break;
#else
#define ANALYTICAL_MIN_MAX_LIMIT(OP)
#endif

#define ANALYTICAL_MIN_MAX(OP, IMP, SIGN_OP)				\
gdk_return								\
GDKanalytical##OP(BAT *r, BAT *b, BAT *s, BAT *e, int tpe)		\
{									\
	bool has_nils = false;						\
	BUN i = 0, cnt = BATcount(b);					\
	lng *restrict start, *restrict end, j = 0, l = 0;		\
									\
	assert(s && e);							\
	start = (lng*)Tloc(s, 0);					\
	end = (lng*)Tloc(e, 0);						\
									\
	switch (tpe) {							\
	case TYPE_bit:							\
		ANALYTICAL_MIN_MAX_CALC(bit, IMP);			\
		break;							\
	case TYPE_bte:							\
		ANALYTICAL_MIN_MAX_CALC(bte, IMP);			\
		break;							\
	case TYPE_sht:							\
		ANALYTICAL_MIN_MAX_CALC(sht, IMP);			\
		break;							\
	case TYPE_int:							\
		ANALYTICAL_MIN_MAX_CALC(int, IMP);			\
		break;							\
	case TYPE_lng:							\
		ANALYTICAL_MIN_MAX_CALC(lng, IMP);			\
		break;							\
		ANALYTICAL_MIN_MAX_LIMIT(IMP)				\
	case TYPE_flt:							\
		ANALYTICAL_MIN_MAX_CALC(flt, IMP);			\
		break;							\
	case TYPE_dbl:							\
		ANALYTICAL_MIN_MAX_CALC(dbl, IMP);			\
		break;							\
	default: {							\
		BATiter bpi = bat_iterator(b);				\
		const void *nil = ATOMnilptr(tpe);			\
		int (*atomcmp)(const void *, const void *) = ATOMcompare(tpe); \
		void *curval;						\
		for (; i < cnt; i++) {					\
			j = start[i];					\
			l = end[i];					\
			curval = (void*)nil;				\
			for (;j < l; j++) {				\
				void *next = BUNtail(bpi, (BUN) j);	\
				if (atomcmp(next, nil) != 0) {		\
					if (atomcmp(curval, nil) == 0)	\
						curval = next;		\
					else				\
						curval = atomcmp(next, curval) SIGN_OP 0 ? curval : next; \
				}					\
			}						\
			if (BUNappend(r, curval, false) != GDK_SUCCEED) \
				goto allocation_error;			\
			if (atomcmp(curval, nil) == 0)			\
				has_nils = true;			\
		}							\
	}								\
	}								\
	BATsetcount(r, cnt);						\
	r->tnonil = !has_nils;						\
	r->tnil = has_nils;						\
	return GDK_SUCCEED;						\
  allocation_error:							\
	GDKerror("GDKanalytical""OP"": malloc failure\n");		\
	return GDK_FAIL;						\
}

ANALYTICAL_MIN_MAX(min, MIN, >)
ANALYTICAL_MIN_MAX(max, MAX, <)

#define ANALYTICAL_COUNT_NO_NIL_FIXED_SIZE_IMP(TPE)		\
	do {							\
		TPE *bp, *bs, *be;				\
		bp = (TPE*)Tloc(b, 0);				\
		for (; i < cnt; i++, rb++) {			\
			bs = bp + start[i];                     \
			be = bp + end[i];                       \
			for (; bs < be; bs++)			\
				curval += !is_##TPE##_nil(*bs);	\
			*rb = curval;                           \
			curval = 0;                             \
		}						\
	} while (0)

#define ANALYTICAL_COUNT_NO_NIL_STR_IMP(TPE_CAST, OFFSET)		\
	do {								\
		for (; i < cnt; i++, rb++) {				\
			j = start[i];					\
			l = end[i];					\
			for (; j < l; j++)				\
				curval += base[(var_t) ((TPE_CAST) bp) OFFSET] != '\200'; \
			*rb = curval;					\
			curval = 0;					\
		}							\
	} while (0)

gdk_return
GDKanalyticalcount(BAT *r, BAT *b, BAT *s, BAT *e, const bit *restrict ignore_nils, int tpe)
{
	BUN i = 0, cnt = BATcount(b);
	lng *restrict rb = (lng *) Tloc(r, 0), *restrict start, *restrict end, curval = 0, j = 0, l = 0;

	assert(s && e && ignore_nils);
	start = (lng *) Tloc(s, 0);
	end = (lng *) Tloc(e, 0);

	if (!*ignore_nils || b->tnonil) {
		for (; i < cnt; i++, rb++)
			*rb = (end[i] > start[i]) ? (end[i] - start[i]) : 0;
	} else {
		switch (tpe) {
		case TYPE_bit:
			ANALYTICAL_COUNT_NO_NIL_FIXED_SIZE_IMP(bit);
			break;
		case TYPE_bte:
			ANALYTICAL_COUNT_NO_NIL_FIXED_SIZE_IMP(bte);
			break;
		case TYPE_sht:
			ANALYTICAL_COUNT_NO_NIL_FIXED_SIZE_IMP(sht);
			break;
		case TYPE_int:
			ANALYTICAL_COUNT_NO_NIL_FIXED_SIZE_IMP(int);
			break;
		case TYPE_lng:
			ANALYTICAL_COUNT_NO_NIL_FIXED_SIZE_IMP(lng);
			break;
#ifdef HAVE_HGE
		case TYPE_hge:
			ANALYTICAL_COUNT_NO_NIL_FIXED_SIZE_IMP(hge);
			break;
#endif
		case TYPE_flt:
			ANALYTICAL_COUNT_NO_NIL_FIXED_SIZE_IMP(flt);
			break;
		case TYPE_dbl:
			ANALYTICAL_COUNT_NO_NIL_FIXED_SIZE_IMP(dbl);
			break;
		case TYPE_str:{
			const char *restrict base = b->tvheap->base;
			const void *restrict bp = Tloc(b, 0);
			switch (b->twidth) {
			case 1:
				ANALYTICAL_COUNT_NO_NIL_STR_IMP(const unsigned char *,[j] + GDK_VAROFFSET);
				break;
			case 2:
				ANALYTICAL_COUNT_NO_NIL_STR_IMP(const unsigned short *,[j] + GDK_VAROFFSET);
				break;
#if SIZEOF_VAR_T != SIZEOF_INT
			case 4:
				ANALYTICAL_COUNT_NO_NIL_STR_IMP(const unsigned int *,[j]);
				break;
#endif
			default:
				ANALYTICAL_COUNT_NO_NIL_STR_IMP(const var_t *,[j]);
				break;
			}
			break;
		}
		default:{
			const void *restrict nil = ATOMnilptr(tpe);
			int (*cmp) (const void *, const void *) = ATOMcompare(tpe);
			if (b->tvarsized) {
				const char *restrict base = b->tvheap->base;
				const void *restrict bp = Tloc(b, 0);
				for (; i < cnt; i++, rb++) {
					j = start[i];
					l = end[i];
					for (; j < l; j++)
						curval += cmp(nil, base + ((const var_t *) bp)[j]) != 0;
					*rb = curval;
					curval = 0;
				}
			} else {
				for (; i < cnt; i++, rb++) {
					j = start[i];
					l = end[i];
					for (; j < l; j++)
						curval += cmp(Tloc(b, j), nil) != 0;
					*rb = curval;
					curval = 0;
				}
			}
		}
		}
	}
	BATsetcount(r, cnt);
	r->tnonil = true;
	r->tnil = false;
	return GDK_SUCCEED;
}

#define ANALYTICAL_SUM_IMP_NUM(TPE1, TPE2)				\
	do {								\
		TPE1 *bs, *be, v;					\
		for (; i < cnt; i++, rb++) {				\
			bs = bp + start[i];				\
			be = bp + end[i];				\
			for (; bs < be; bs++) {				\
				v = *bs;				\
				if (!is_##TPE1##_nil(v)) {		\
					if (is_##TPE2##_nil(curval))	\
						curval = (TPE2) v;      \
					else				\
						ADD_WITH_CHECK(v, curval, TPE2, curval, GDK_##TPE2##_max, goto calc_overflow); \
				}					\
			}						\
			*rb = curval;					\
			if (is_##TPE2##_nil(curval))			\
				has_nils = true;			\
			else						\
				curval = TPE2##_nil;			\
		}							\
	} while (0)

#define ANALYTICAL_SUM_IMP_FP(TPE1, TPE2)				\
	do {								\
		TPE1 *bs;						\
		BUN parcel;						\
		for (; i < cnt; i++, rb++) {				\
			if (end[i] > start[i]) {			\
				bs = bp + start[i];			\
				parcel = (BUN)(end[i] - start[i]);	\
				if (dofsum(bs, 0,			\
					   &(struct canditer){.tpe = cand_dense, .ncand = parcel,}, \
					   parcel, &curval, 1, TYPE_##TPE1, \
					   TYPE_##TPE2, NULL, 0, 0, true, \
					   false, true) == BUN_NONE) {	\
					goto bailout;			\
				}					\
			}						\
			*rb = curval;					\
			if (is_##TPE2##_nil(curval))			\
				has_nils = true;			\
			else						\
				curval = TPE2##_nil;			\
		}							\
	} while (0)

#define ANALYTICAL_SUM_CALC(TPE1, TPE2, IMP)		\
	do {						\
		TPE1 *bp = (TPE1*)Tloc(b, 0);           \
		TPE2 *restrict rb, curval = TPE2##_nil; \
		rb = (TPE2*)Tloc(r, 0);                 \
		IMP(TPE1, TPE2);                        \
	} while (0)

gdk_return
GDKanalyticalsum(BAT *r, BAT *b, BAT *s, BAT *e, int tp1, int tp2)
{
	bool has_nils = false;
	BUN i = 0, cnt = BATcount(b), nils = 0;
	int abort_on_error = 1;
	lng *restrict start, *restrict end;

	assert(s && e);
	start = (lng *) Tloc(s, 0);
	end = (lng *) Tloc(e, 0);

	switch (tp2) {
	case TYPE_bte:{
		switch (tp1) {
		case TYPE_bte:
			ANALYTICAL_SUM_CALC(bte, bte, ANALYTICAL_SUM_IMP_NUM);
			break;
		default:
			goto nosupport;
		}
		break;
	}
	case TYPE_sht:{
		switch (tp1) {
		case TYPE_bte:
			ANALYTICAL_SUM_CALC(bte, sht, ANALYTICAL_SUM_IMP_NUM);
			break;
		case TYPE_sht:
			ANALYTICAL_SUM_CALC(sht, sht, ANALYTICAL_SUM_IMP_NUM);
			break;
		default:
			goto nosupport;
		}
		break;
	}
	case TYPE_int:{
		switch (tp1) {
		case TYPE_bte:
			ANALYTICAL_SUM_CALC(bte, int, ANALYTICAL_SUM_IMP_NUM);
			break;
		case TYPE_sht:
			ANALYTICAL_SUM_CALC(sht, int, ANALYTICAL_SUM_IMP_NUM);
			break;
		case TYPE_int:
			ANALYTICAL_SUM_CALC(int, int, ANALYTICAL_SUM_IMP_NUM);
			break;
		default:
			goto nosupport;
		}
		break;
	}
	case TYPE_lng:{
		switch (tp1) {
		case TYPE_bte:
			ANALYTICAL_SUM_CALC(bte, lng, ANALYTICAL_SUM_IMP_NUM);
			break;
		case TYPE_sht:
			ANALYTICAL_SUM_CALC(sht, lng, ANALYTICAL_SUM_IMP_NUM);
			break;
		case TYPE_int:
			ANALYTICAL_SUM_CALC(int, lng, ANALYTICAL_SUM_IMP_NUM);
			break;
		case TYPE_lng:
			ANALYTICAL_SUM_CALC(lng, lng, ANALYTICAL_SUM_IMP_NUM);
			break;
		default:
			goto nosupport;
		}
		break;
	}
#ifdef HAVE_HGE
	case TYPE_hge:{
		switch (tp1) {
		case TYPE_bte:
			ANALYTICAL_SUM_CALC(bte, hge, ANALYTICAL_SUM_IMP_NUM);
			break;
		case TYPE_sht:
			ANALYTICAL_SUM_CALC(sht, hge, ANALYTICAL_SUM_IMP_NUM);
			break;
		case TYPE_int:
			ANALYTICAL_SUM_CALC(int, hge, ANALYTICAL_SUM_IMP_NUM);
			break;
		case TYPE_lng:
			ANALYTICAL_SUM_CALC(lng, hge, ANALYTICAL_SUM_IMP_NUM);
			break;
		case TYPE_hge:
			ANALYTICAL_SUM_CALC(hge, hge, ANALYTICAL_SUM_IMP_NUM);
			break;
		default:
			goto nosupport;
		}
		break;
	}
#endif
	case TYPE_flt:{
		switch (tp1) {
		case TYPE_flt:
			ANALYTICAL_SUM_CALC(flt, flt, ANALYTICAL_SUM_IMP_FP);
			break;
		default:
			goto nosupport;
		}
		break;
	}
	case TYPE_dbl:{
		switch (tp1) {
		case TYPE_flt:
			ANALYTICAL_SUM_CALC(flt, dbl, ANALYTICAL_SUM_IMP_FP);
			break;
		case TYPE_dbl:
			ANALYTICAL_SUM_CALC(dbl, dbl, ANALYTICAL_SUM_IMP_FP);
			break;
		default:
			goto nosupport;
		}
		break;
	}
	default:
		goto nosupport;
	}
	BATsetcount(r, cnt);
	r->tnonil = !has_nils;
	r->tnil = has_nils;
	return GDK_SUCCEED;
      bailout:
	GDKerror("GDKanalyticalsum: error while calculating floating-point sum\n");
	return GDK_FAIL;
      nosupport:
	GDKerror("GDKanalyticalsum: type combination (sum(%s)->%s) not supported.\n", ATOMname(tp1), ATOMname(tp2));
	return GDK_FAIL;
      calc_overflow:
	GDKerror("22003!overflow in calculation.\n");
	return GDK_FAIL;
}

#define ANALYTICAL_PROD_CALC_NUM(TPE1, TPE2, TPE3)			\
	do {								\
		TPE1 *bp = (TPE1*)Tloc(b, 0), *bs, *be, v;		\
		TPE2 *restrict rb, curval = TPE2##_nil;			\
		rb = (TPE2*)Tloc(r, 0);					\
		for (; i < cnt; i++, rb++) {				\
			bs = bp + start[i];				\
			be = bp + end[i];				\
			for (; bs < be; bs++) {				\
				v = *bs;				\
				if (!is_##TPE1##_nil(v)) {		\
					if (is_##TPE2##_nil(curval))	\
						curval = (TPE2) v;	\
					else				\
						MUL4_WITH_CHECK(v, curval, TPE2, curval, GDK_##TPE2##_max, TPE3, \
										goto calc_overflow); \
				}					\
			}						\
			*rb = curval;					\
			if (is_##TPE2##_nil(curval))			\
				has_nils = true;			\
			else						\
				curval = TPE2##_nil;			\
		}							\
	} while (0)

#define ANALYTICAL_PROD_CALC_NUM_LIMIT(TPE1, TPE2, REAL_IMP)		\
	do {								\
		TPE1 *bp = (TPE1*)Tloc(b, 0), *bs, *be, v;		\
		TPE2 *restrict rb, curval = TPE2##_nil;			\
		rb = (TPE2*)Tloc(r, 0);					\
		for (; i < cnt; i++, rb++) {				\
			bs = bp + start[i];				\
			be = bp + end[i];				\
			for (; bs < be; bs++) {				\
				v = *bs;				\
				if (!is_##TPE1##_nil(v)) {		\
					if (is_##TPE2##_nil(curval))	\
						curval = (TPE2) v;      \
					else				\
						REAL_IMP(v, curval, curval, GDK_##TPE2##_max, goto calc_overflow); \
				}					\
			}						\
			*rb = curval;					\
			if (is_##TPE2##_nil(curval))			\
				has_nils = true;			\
			else						\
				curval = TPE2##_nil;			\
		}							\
	} while (0)

#define ANALYTICAL_PROD_CALC_FP(TPE1, TPE2)				\
	do {								\
		TPE1 *bp = (TPE1*)Tloc(b, 0), *bs, *be, v;		\
		TPE2 *restrict rb, curval = TPE2##_nil;			\
		rb = (TPE2*)Tloc(r, 0);					\
		for (; i < cnt; i++, rb++) {				\
			bs = bp + start[i];				\
			be = bp + end[i];				\
			for (; bs < be; bs++) {				\
				v = *bs;				\
				if (!is_##TPE1##_nil(v)) {		\
					if (is_##TPE2##_nil(curval)) {	\
						curval = (TPE2) v;	\
					} else if (ABSOLUTE(curval) > 1 && GDK_##TPE2##_max / ABSOLUTE(v) < ABSOLUTE(curval)) { \
						if (abort_on_error)	\
							goto calc_overflow; \
						curval = TPE2##_nil;	\
						nils++;			\
					} else {			\
						curval *= v;		\
					}				\
				}					\
			}						\
			*rb = curval;					\
			if (is_##TPE2##_nil(curval))			\
				has_nils = true;			\
			else						\
				curval = TPE2##_nil;			\
		}							\
	} while (0)

gdk_return
GDKanalyticalprod(BAT *r, BAT *b, BAT *s, BAT *e, int tp1, int tp2)
{
	bool has_nils = false;
	BUN i = 0, cnt = BATcount(b), nils = 0;
	int abort_on_error = 1;
	lng *restrict start, *restrict end;

	assert(s && e);
	start = (lng *) Tloc(s, 0);
	end = (lng *) Tloc(e, 0);

	switch (tp2) {
	case TYPE_bte:{
		switch (tp1) {
		case TYPE_bte:
			ANALYTICAL_PROD_CALC_NUM(bte, bte, sht);
			break;
		default:
			goto nosupport;
		}
		break;
	}
	case TYPE_sht:{
		switch (tp1) {
		case TYPE_bte:
			ANALYTICAL_PROD_CALC_NUM(bte, sht, int);
			break;
		case TYPE_sht:
			ANALYTICAL_PROD_CALC_NUM(sht, sht, int);
			break;
		default:
			goto nosupport;
		}
		break;
	}
	case TYPE_int:{
		switch (tp1) {
		case TYPE_bte:
			ANALYTICAL_PROD_CALC_NUM(bte, int, lng);
			break;
		case TYPE_sht:
			ANALYTICAL_PROD_CALC_NUM(sht, int, lng);
			break;
		case TYPE_int:
			ANALYTICAL_PROD_CALC_NUM(int, int, lng);
			break;
		default:
			goto nosupport;
		}
		break;
	}
#ifdef HAVE_HGE
	case TYPE_lng:{
		switch (tp1) {
		case TYPE_bte:
			ANALYTICAL_PROD_CALC_NUM(bte, lng, hge);
			break;
		case TYPE_sht:
			ANALYTICAL_PROD_CALC_NUM(sht, lng, hge);
			break;
		case TYPE_int:
			ANALYTICAL_PROD_CALC_NUM(int, lng, hge);
			break;
		case TYPE_lng:
			ANALYTICAL_PROD_CALC_NUM(lng, lng, hge);
			break;
		default:
			goto nosupport;
		}
		break;
	}
	case TYPE_hge:{
		switch (tp1) {
		case TYPE_bte:
			ANALYTICAL_PROD_CALC_NUM_LIMIT(bte, hge, HGEMUL_CHECK);
			break;
		case TYPE_sht:
			ANALYTICAL_PROD_CALC_NUM_LIMIT(sht, hge, HGEMUL_CHECK);
			break;
		case TYPE_int:
			ANALYTICAL_PROD_CALC_NUM_LIMIT(int, hge, HGEMUL_CHECK);
			break;
		case TYPE_lng:
			ANALYTICAL_PROD_CALC_NUM_LIMIT(lng, hge, HGEMUL_CHECK);
			break;
		case TYPE_hge:
			ANALYTICAL_PROD_CALC_NUM_LIMIT(hge, hge, HGEMUL_CHECK);
			break;
		default:
			goto nosupport;
		}
		break;
	}
#else
	case TYPE_lng:{
		switch (tp1) {
		case TYPE_bte:
			ANALYTICAL_PROD_CALC_NUM_LIMIT(bte, lng, LNGMUL_CHECK);
			break;
		case TYPE_sht:
			ANALYTICAL_PROD_CALC_NUM_LIMIT(sht, lng, LNGMUL_CHECK);
			break;
		case TYPE_int:
			ANALYTICAL_PROD_CALC_NUM_LIMIT(int, lng, LNGMUL_CHECK);
			break;
		case TYPE_lng:
			ANALYTICAL_PROD_CALC_NUM_LIMIT(lng, lng, LNGMUL_CHECK);
			break;
		default:
			goto nosupport;
		}
		break;
	}
#endif
	case TYPE_flt:{
		switch (tp1) {
		case TYPE_flt:
			ANALYTICAL_PROD_CALC_FP(flt, flt);
			break;
		default:
			goto nosupport;
		}
		break;
	}
	case TYPE_dbl:{
		switch (tp1) {
		case TYPE_flt:
			ANALYTICAL_PROD_CALC_FP(flt, dbl);
			break;
		case TYPE_dbl:
			ANALYTICAL_PROD_CALC_FP(dbl, dbl);
			break;
		default:
			goto nosupport;
		}
		break;
	}
	default:
		goto nosupport;
	}
	BATsetcount(r, cnt);
	r->tnonil = !has_nils;
	r->tnil = has_nils;
	return GDK_SUCCEED;
      nosupport:
	GDKerror("GDKanalyticalprod: type combination (prod(%s)->%s) not supported.\n", ATOMname(tp1), ATOMname(tp2));
	return GDK_FAIL;
      calc_overflow:
	GDKerror("22003!overflow in calculation.\n");
	return GDK_FAIL;
}

#define ANALYTICAL_AVERAGE_CALC_NUM(TPE,lng_hge)			\
	do {								\
		TPE *bp = (TPE*)Tloc(b, 0), *bs, *be, v, a = 0;		\
		for (; i < cnt; i++, rb++) {				\
			bs = bp + start[i];				\
			be = bp + end[i];				\
			for (; bs < be; bs++) {				\
				v = *bs;				\
				if (!is_##TPE##_nil(v)) {		\
					ADD_WITH_CHECK(v, sum, lng_hge, sum, GDK_##lng_hge##_max, goto avg_overflow##TPE); \
					/* count only when no overflow occurs */ \
					n++;				\
				}					\
			}						\
			if (0) {					\
avg_overflow##TPE:							\
				assert(n > 0);				\
				if (sum >= 0) {				\
					a = (TPE) (sum / (lng_hge) n);	\
					rr = (BUN) (sum % (SBUN) n);	\
				} else {				\
					sum = -sum;			\
					a = - (TPE) (sum / (lng_hge) n); \
					rr = (BUN) (sum % (SBUN) n);	\
					if (r) {			\
						a--;			\
						rr = n - rr;		\
					}				\
				}					\
				for (; bs < be; bs++) {			\
					v = *bs;			\
					if (is_##TPE##_nil(v))		\
						continue;		\
					AVERAGE_ITER(TPE, v, a, rr, n);	\
				}					\
				curval = a + (dbl) rr / n;		\
				goto calc_done##TPE;			\
			}						\
			curval = n > 0 ? (dbl) sum / n : dbl_nil;	\
calc_done##TPE:								\
			*rb = curval;					\
			has_nils = has_nils || (n == 0);		\
			n = 0;						\
			sum = 0;					\
		}							\
	} while (0)

#ifdef HAVE_HGE
#define ANALYTICAL_AVERAGE_LNG_HGE(TPE) ANALYTICAL_AVERAGE_CALC_NUM(TPE,hge)
#else
#define ANALYTICAL_AVERAGE_LNG_HGE(TPE) ANALYTICAL_AVERAGE_CALC_NUM(TPE,lng)
#endif

#define ANALYTICAL_AVERAGE_CALC_FP(TPE)					\
	do {								\
		TPE *bp = (TPE*)Tloc(b, 0), *bs, *be, v;		\
		dbl a = 0;						\
		for (; i < cnt; i++, rb++) {				\
			bs = bp + start[i];				\
			be = bp + end[i];				\
			for (; bs < be; bs++) {				\
				v = *bs;				\
				if (!is_##TPE##_nil(v))			\
					AVERAGE_ITER_FLOAT(TPE, v, a, n); \
			}						\
			curval = (n > 0) ? a : dbl_nil;			\
			*rb = curval;					\
			has_nils = has_nils || (n == 0);		\
			n = 0;						\
			a = 0;						\
		}							\
	} while (0)

gdk_return
GDKanalyticalavg(BAT *r, BAT *b, BAT *s, BAT *e, int tpe)
{
	bool has_nils = false;
	BUN i = 0, cnt = BATcount(b), nils = 0, n = 0, rr = 0;
	bool abort_on_error = true;
	lng *restrict start, *restrict end;
	dbl *restrict rb = (dbl *) Tloc(r, 0), curval;
#ifdef HAVE_HGE
	hge sum = 0;
#else
	lng sum = 0;
#endif

	assert(s && e);
	start = (lng *) Tloc(s, 0);
	end = (lng *) Tloc(e, 0);

	switch (tpe) {
	case TYPE_bte:
		ANALYTICAL_AVERAGE_LNG_HGE(bte);
		break;
	case TYPE_sht:
		ANALYTICAL_AVERAGE_LNG_HGE(sht);
		break;
	case TYPE_int:
		ANALYTICAL_AVERAGE_LNG_HGE(int);
		break;
	case TYPE_lng:
		ANALYTICAL_AVERAGE_LNG_HGE(lng);
		break;
#ifdef HAVE_HGE
	case TYPE_hge:
		ANALYTICAL_AVERAGE_LNG_HGE(hge);
		break;
#endif
	case TYPE_flt:
		ANALYTICAL_AVERAGE_CALC_FP(flt);
		break;
	case TYPE_dbl:
		ANALYTICAL_AVERAGE_CALC_FP(dbl);
		break;
	default:
		GDKerror("GDKanalyticalavg: average of type %s unsupported.\n", ATOMname(tpe));
		return GDK_FAIL;
	}
	BATsetcount(r, cnt);
	r->tnonil = !has_nils;
	r->tnil = has_nils;
	return GDK_SUCCEED;
}
