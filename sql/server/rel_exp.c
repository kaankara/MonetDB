/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0.  If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Copyright 1997 - July 2008 CWI, August 2008 - 2019 MonetDB B.V.
 */

#include "monetdb_config.h"
#include "sql_relation.h"
#include "sql_semantic.h"
#include "rel_exp.h"
#include "rel_prop.h" /* for prop_copy() */
#include "rel_unnest.h"
#include "rel_optimizer.h"
#include "rel_distribute.h"
#ifdef HAVE_HGE
#include "mal.h"		/* for have_hge */
#endif
#include "mtime.h"
#include "blob.h"

comp_type 
swap_compare( comp_type t )
{
	switch(t) {
	case cmp_equal:
		return cmp_equal;
	case cmp_lt:
		return cmp_gt;
	case cmp_lte:
		return cmp_gte;
	case cmp_gte:
		return cmp_lte;
	case cmp_gt:
		return cmp_lt;
	case cmp_notequal:
		return cmp_notequal;
	default:
		return cmp_equal;
	}
}

comp_type 
range2lcompare( int r )
{
	if (r&1) {
		return cmp_gte;
	} else {
		return cmp_gt;
	}
}

comp_type 
range2rcompare( int r )
{
	if (r&2) {
		return cmp_lte;
	} else {
		return cmp_lt;
	}
}

int 
compare2range( int l, int r )
{
	if (l == cmp_gt) {
		if (r == cmp_lt)
			return 0;
		else if (r == cmp_lte)
			return 2;
	} else if (l == cmp_gte) {
		if (r == cmp_lt)
			return 1;
		else if (r == cmp_lte)
			return 3;
	} 
	return -1;
}

static sql_exp * 
exp_create(sql_allocator *sa, int type ) 
{
	sql_exp *e = SA_NEW(sa, sql_exp);

	if (e == NULL)
		return NULL;
	e->type = (expression_type)type;
	e->alias.label = 0;
	e->alias.name = NULL;
	e->alias.rname = NULL;
	e->f = e->l = e->r = NULL;
	e->flag = 0;
	e->card = 0;
	e->freevar = 0;
	e->intern = 0;
	e->anti = 0;
	e->base = 0;
	e->used = 0;
	e->tpe.type = NULL;
	e->tpe.digits = e->tpe.scale = 0;
	e->p = NULL;
	return e;
}

sql_exp * 
exp_compare(sql_allocator *sa, sql_exp *l, sql_exp *r, int cmptype) 
{
	sql_exp *e = exp_create(sa, e_cmp);
	if (e == NULL)
		return NULL;
	e->card = MAX(l->card,r->card);
	if (e->card == CARD_ATOM && !exp_is_atom(l))
		e->card = CARD_AGGR;
	e->l = l;
	e->r = r;
	e->flag = cmptype;
	return e;
}

sql_exp * 
exp_compare2(sql_allocator *sa, sql_exp *l, sql_exp *r, sql_exp *h, int cmptype) 
{
	sql_exp *e = exp_create(sa, e_cmp);
	if (e == NULL)
		return NULL;
	e->card = l->card;
	if (e->card == CARD_ATOM && !exp_is_atom(l))
		e->card = CARD_AGGR;
	e->l = l;
	e->r = r;
	if (h)
		e->f = h;
	e->flag = cmptype;
	return e;
}

sql_exp *
exp_filter(sql_allocator *sa, list *l, list *r, sql_subfunc *f, int anti) 
{
	sql_exp *e = exp_create(sa, e_cmp);

	if (e == NULL)
		return NULL;
	e->card = exps_card(l);
	e->l = l;
	e->r = r;
	e->f = f;
	e->flag = cmp_filter;
	if (anti)
		set_anti(e);
	return e;
}

sql_exp *
exp_or(sql_allocator *sa, list *l, list *r, int anti)
{
	sql_exp *f = NULL;
	sql_exp *e = exp_create(sa, e_cmp);

	if (e == NULL)
		return NULL;
	f = l->h?l->h->data:r->h?r->h->data:NULL;
	e->card = l->h?exps_card(l):exps_card(r);
	e->l = l;
	e->r = r;
	assert(f);
	e->f = f;
	e->flag = cmp_or;
	if (anti)
		set_anti(e);
	return e;
}

sql_exp *
exp_in(sql_allocator *sa, sql_exp *l, list *r, int cmptype)
{
	sql_exp *e = exp_create(sa, e_cmp);

	if (e == NULL)
		return NULL;
	e->card = l->card;
	e->l = l;
	e->r = r;
	assert( cmptype == cmp_in || cmptype == cmp_notin);
	e->flag = cmptype;
	return e;
}

static sql_subtype*
dup_subtype(sql_allocator *sa, sql_subtype *st)
{
	sql_subtype *res = SA_NEW(sa, sql_subtype);

	if (res == NULL)
		return NULL;
	*res = *st;
	return res;
}

sql_exp * 
exp_convert(sql_allocator *sa, sql_exp *exp, sql_subtype *fromtype, sql_subtype *totype )
{
	sql_exp *e = exp_create(sa, e_convert);
	if (e == NULL)
		return NULL;
	e->card = exp->card;
	e->l = exp;
	totype = dup_subtype(sa, totype);
	e->r = append(append(sa_list(sa), dup_subtype(sa, fromtype)),totype);
	e->tpe = *totype; 
	e->alias = exp->alias;
	return e;
}

sql_exp * 
exp_op( sql_allocator *sa, list *l, sql_subfunc *f )
{
	sql_exp *e = exp_create(sa, e_func);
	if (e == NULL)
		return NULL;
	e->card = exps_card(l);
	if (!l || list_length(l) == 0) 
		e->card = CARD_ATOM; /* unop returns a single atom */
	if (f->func->side_effect)
		e->card = CARD_MULTI;
	e->l = l;
	e->f = f; 
	return e;
}

sql_exp * 
exp_aggr( sql_allocator *sa, list *l, sql_subaggr *a, int distinct, int no_nils, unsigned int card, int has_nils )
{
	sql_exp *e = exp_create(sa, e_aggr);
	if (e == NULL)
		return NULL;
	e->card = card;
	e->l = l;
	e->f = a; 
	if (distinct)
		set_distinct(e);
	if (no_nils)
		set_no_nil(e);
	if (!has_nils)
		set_has_no_nil(e);
	return e;
}

sql_exp * 
exp_atom(sql_allocator *sa, atom *a) 
{
	sql_exp *e = exp_create(sa, e_atom);
	if (e == NULL)
		return NULL;
	e->card = CARD_ATOM;
	e->tpe = a->tpe;
	e->l = a;
	return e;
}

sql_exp *
exp_atom_max(sql_allocator *sa, sql_subtype *tpe) 
{

	if (tpe->type->localtype == TYPE_bte) {
		return exp_atom_bte(sa, GDK_bte_max);
	} else if (tpe->type->localtype == TYPE_sht) {
		return exp_atom_sht(sa, GDK_sht_max);
	} else if (tpe->type->localtype == TYPE_int) {
		return exp_atom_int(sa, GDK_int_max);
	} else if (tpe->type->localtype == TYPE_lng) {
		return exp_atom_lng(sa, GDK_lng_max);
#ifdef HAVE_HGE
	} else if (tpe->type->localtype == TYPE_hge) {
		return exp_atom_hge(sa, GDK_hge_max);
#endif
	}
	return NULL;
}

sql_exp *
exp_atom_bool(sql_allocator *sa, int b) 
{
	sql_subtype bt; 

	sql_find_subtype(&bt, "boolean", 0, 0);
	if (b) 
		return exp_atom(sa, atom_bool(sa, &bt, TRUE ));
	else
		return exp_atom(sa, atom_bool(sa, &bt, FALSE ));
}

sql_exp *
exp_atom_bte(sql_allocator *sa, bte i) 
{
	sql_subtype it; 

	sql_find_subtype(&it, "tinyint", 3, 0);
	return exp_atom(sa, atom_int(sa, &it, i ));
}

sql_exp *
exp_atom_sht(sql_allocator *sa, sht i) 
{
	sql_subtype it; 

	sql_find_subtype(&it, "smallint", 5, 0);
	return exp_atom(sa, atom_int(sa, &it, i ));
}

sql_exp *
exp_atom_int(sql_allocator *sa, int i) 
{
	sql_subtype it; 

	sql_find_subtype(&it, "int", 9, 0);
	return exp_atom(sa, atom_int(sa, &it, i ));
}

sql_exp *
exp_atom_lng(sql_allocator *sa, lng i) 
{
	sql_subtype it; 

#ifdef HAVE_HGE
	sql_find_subtype(&it, "bigint", have_hge ? 18 : 19, 0);
#else
	sql_find_subtype(&it, "bigint", 19, 0);
#endif
	return exp_atom(sa, atom_int(sa, &it, i ));
}

#ifdef HAVE_HGE
sql_exp *
exp_atom_hge(sql_allocator *sa, hge i) 
{
	sql_subtype it; 

	sql_find_subtype(&it, "hugeint", 39, 0);
	return exp_atom(sa, atom_int(sa, &it, i ));
}
#endif

sql_exp *
exp_atom_flt(sql_allocator *sa, flt f) 
{
	sql_subtype it; 

	sql_find_subtype(&it, "real", 24, 0);
	return exp_atom(sa, atom_float(sa, &it, (dbl)f ));
}

sql_exp *
exp_atom_dbl(sql_allocator *sa, dbl f) 
{
	sql_subtype it; 

	sql_find_subtype(&it, "double", 53, 0);
	return exp_atom(sa, atom_float(sa, &it, (dbl)f ));
}

sql_exp *
exp_atom_str(sql_allocator *sa, const char *s, sql_subtype *st) 
{
	return exp_atom(sa, atom_string(sa, st, s?sa_strdup(sa, s):NULL));
}

sql_exp *
exp_atom_clob(sql_allocator *sa, const char *s) 
{
	sql_subtype clob;

	sql_find_subtype(&clob, "clob", 0, 0);
	return exp_atom(sa, atom_string(sa, &clob, s?sa_strdup(sa, s):NULL));
}

sql_exp *
exp_atom_ptr(sql_allocator *sa, void *s) 
{
	sql_subtype *t = sql_bind_localtype("ptr");
	return exp_atom(sa, atom_ptr(sa, t, s));
}

sql_exp * 
exp_atom_ref(sql_allocator *sa, int i, sql_subtype *tpe) 
{
	sql_exp *e = exp_create(sa, e_atom);
	if (e == NULL)
		return NULL;
	e->card = CARD_ATOM;
	e->flag = i;
	if (tpe)
		e->tpe = *tpe;
	return e;
}

sql_exp *
exp_null(sql_allocator *sa, sql_subtype *tpe)
{
	atom *a = atom_general(sa, tpe, NULL);
	return exp_atom(sa, a);
}

atom *
exp_value(mvc *sql, sql_exp *e, atom **args, int maxarg)
{
	if (!e || e->type != e_atom)
		return NULL; 
	if (e->l) {	   /* literal */
		return e->l;
	} else if (e->r) { /* param (ie not set) */
		if (e->flag <= 1) /* global variable */
			return stack_get_var(sql, e->r); 
		return NULL; 
	} else if (sql->emode == m_normal && e->flag < (unsigned) maxarg) { /* do not get the value in the prepared case */
		return args[e->flag]; 
	}
	return NULL; 
}

sql_exp * 
exp_param(sql_allocator *sa, const char *name, sql_subtype *tpe, int frame) 
{
	sql_exp *e = exp_create(sa, e_atom);
	if (e == NULL)
		return NULL;
	e->r = (char*)name;
	e->card = CARD_ATOM;
	e->flag = frame;
	if (tpe)
		e->tpe = *tpe;
	return e;
}

sql_exp * 
exp_values(sql_allocator *sa, list *exps) 
{
	sql_exp *e = exp_create(sa, e_atom);
	if (e == NULL)
		return NULL;
	e->card = CARD_MULTI;
	e->f = exps;
	return e;
}

list * 
exp_types(sql_allocator *sa, list *exps) 
{
	list *l = sa_list(sa);
	node *n;

	for ( n = exps->h; n; n = n->next)
		append(l, exp_subtype(n->data));
	return l;
}

int
have_nil(list *exps)
{
	int has_nil = 0;
	node *n;

	for ( n = exps->h; n && !has_nil; n = n->next) {
		sql_exp *e = n->data;
		has_nil |= has_nil(e);
	}
	return has_nil;
}

sql_exp * 
exp_column(sql_allocator *sa, const char *rname, const char *cname, sql_subtype *t, unsigned int card, int has_nils, int intern) 
{
	sql_exp *e = exp_create(sa, e_column);

	if (e == NULL)
		return NULL;
	assert(cname);
	e->card = card;
	e->alias.name = cname;
	e->alias.rname = rname;
	e->r = (char*)e->alias.name;
	e->l = (char*)e->alias.rname;
	if (t)
		e->tpe = *t;
	if (!has_nils)
		set_has_no_nil(e);
	if (intern)
		set_intern(e);
	return e;
}

sql_exp *
exp_propagate(sql_allocator *sa, sql_exp *ne, sql_exp *oe)
{
	if (is_intern(oe))
		set_intern(ne);
	if (is_anti(oe))
		set_anti(ne);
	if (is_basecol(oe))
		set_basecol(ne);
	ne->p = prop_copy(sa, oe->p);
	return ne;
}

sql_exp * 
exp_alias(sql_allocator *sa, const char *arname, const char *acname, const char *org_rname, const char *org_cname, sql_subtype *t, unsigned int card, int has_nils, int intern) 
{
	sql_exp *e = exp_column(sa, org_rname, org_cname, t, card, has_nils, intern);

	if (e == NULL)
		return NULL;
	assert(acname && org_cname);
	exp_setname(sa, e, (arname)?arname:org_rname, acname);
	return e;
}

sql_exp *
exp_alias_or_copy(mvc *sql, const char *tname, const char *cname, sql_rel *orel, sql_exp *old)
{
	sql_exp *ne = NULL;

	if (!tname)
		tname = old->alias.rname;

	if (!tname && old->type == e_column)
		tname = old->l;

	if (!cname && exp_name(old) && has_label(old)) {
		ne = exp_column(sql->sa, exp_relname(old), exp_name(old), exp_subtype(old), orel?orel->card:CARD_ATOM, has_nil(old), is_intern(old));
		return exp_propagate(sql->sa, ne, old);
	} else if (!cname) {
		char name[16], *nme;
		nme = number2name(name, sizeof(name), ++sql->label);

		exp_setname(sql->sa, old, nme, nme);
		ne = exp_column(sql->sa, exp_relname(old), exp_name(old), exp_subtype(old), orel?orel->card:CARD_ATOM, has_nil(old), is_intern(old));
		return exp_propagate(sql->sa, ne, old);
	} else if (cname && !old->alias.name) {
		exp_setname(sql->sa, old, tname, cname);
	}
	ne = exp_column(sql->sa, tname, cname, exp_subtype(old), orel?orel->card:CARD_ATOM, has_nil(old), is_intern(old));
	return exp_propagate(sql->sa, ne, old);
}

sql_exp *
exp_set(sql_allocator *sa, const char *name, sql_exp *val, int level)
{
	sql_exp *e = exp_create(sa, e_psm);

	if (e == NULL)
		return NULL;
	e->alias.name = name;
	e->l = val;
	e->flag = PSM_SET + SET_PSM_LEVEL(level);
	return e;
}

sql_exp * 
exp_var(sql_allocator *sa, const char *name, sql_subtype *type, int level)
{
	sql_exp *e = exp_create(sa, e_psm);

	if (e == NULL)
		return NULL;
	e->alias.name = name;
	e->tpe = *type;
	e->flag = PSM_VAR + SET_PSM_LEVEL(level);
	return e;
}

sql_exp * 
exp_table(sql_allocator *sa, const char *name, sql_table *t, int level)
{
	sql_exp *e = exp_create(sa, e_psm);

	if (e == NULL)
		return NULL;
	e->alias.name = name;
	e->f = t;
	e->flag = PSM_VAR + SET_PSM_LEVEL(level);
	return e;
}

sql_exp *
exp_return(sql_allocator *sa, sql_exp *val, int level)
{
	sql_exp *e = exp_create(sa, e_psm);

	if (e == NULL)
		return NULL;
	e->l = val;
	e->flag = PSM_RETURN + SET_PSM_LEVEL(level);
	return e;
}

sql_exp * 
exp_while(sql_allocator *sa, sql_exp *cond, list *stmts)
{
	sql_exp *e = exp_create(sa, e_psm);

	if (e == NULL)
		return NULL;
	e->l = cond;
	e->r = stmts;
	e->flag = PSM_WHILE;
	return e;
}

sql_exp * 
exp_if(sql_allocator *sa, sql_exp *cond, list *if_stmts, list *else_stmts)
{
	sql_exp *e = exp_create(sa, e_psm);

	if (e == NULL)
		return NULL;
	e->l = cond;
	e->r = if_stmts;
	e->f = else_stmts;
	e->flag = PSM_IF;
	return e;
}

sql_exp * 
exp_rel(mvc *sql, sql_rel *rel)
{
	sql_exp *e = exp_create(sql->sa, e_psm);

	if (e == NULL)
		return NULL;
	/*
	rel = rel_unnest(sql, rel);
	rel = rel_optimizer(sql, rel, 0);
	rel = rel_distribute(sql, rel);
	*/
	e->l = rel;
	e->flag = PSM_REL;
	return e;
}

sql_exp *
exp_exception(sql_allocator *sa, sql_exp *cond, char* error_message)
{
	sql_exp *e = exp_create(sa, e_psm);

	if (e == NULL)
		return NULL;
	e->l = cond;
	e->r = sa_strdup(sa, error_message);
	e->flag = PSM_EXCEPTION;
	return e;
}

/* Set a name (alias) for the expression, such that we can refer 
   to this expression by this simple name.
 */
void 
exp_setname(sql_allocator *sa, sql_exp *e, const char *rname, const char *name )
{
	e->alias.label = 0;
	if (name) 
		e->alias.name = sa_strdup(sa, name);
	e->alias.rname = (rname)?sa_strdup(sa, rname):NULL;
}

void 
noninternexp_setname(sql_allocator *sa, sql_exp *e, const char *rname, const char *name )
{
	if (!is_intern(e))
		exp_setname(sa, e, rname, name);
}

void 
exp_setalias(sql_exp *e, const char *rname, const char *name )
{
	e->alias.label = 0;
	e->alias.name = name;
	e->alias.rname = rname;
}

void 
exp_prop_alias(sql_exp *e, sql_exp *oe )
{
	e->alias = oe->alias;
}

str
number2name(str s, int len, int i)
{
	s[--len] = 0;
	while(i>0) {
		s[--len] = '0' + (i & 7);
		i >>= 3;
	}
	s[--len] = 'L';
	return s + len;
}

void 
exp_setrelname(sql_allocator *sa, sql_exp *e, int nr)
{
	char name[16], *nme;

	nme = number2name(name, sizeof(name), nr);
	e->alias.label = 0;
	e->alias.rname = sa_strdup(sa, nme);
}

char *
make_label(sql_allocator *sa, int nr)
{
	char name[16], *nme;

	nme = number2name(name, sizeof(name), nr);
	return sa_strdup(sa, nme);
}

sql_exp*
exp_label(sql_allocator *sa, sql_exp *e, int nr)
{
	assert(nr > 0);
	e->alias.label = nr;
	e->alias.rname = e->alias.name = make_label(sa, nr);
	return e;
}

sql_exp*
exp_label_table(sql_allocator *sa, sql_exp *e, int nr)
{
	e->alias.rname = make_label(sa, nr);
	return e;
}

list*
exps_label(sql_allocator *sa, list *exps, int nr)
{
	node *n;

	if (!exps)
		return NULL;
	for (n = exps->h; n; n = n->next)
		n->data = exp_label(sa, n->data, nr++);
	return exps;
}

void
exp_swap( sql_exp *e ) 
{
	sql_exp *s = e->l;

	e->l = e->r;
	e->r = s;
	e->flag = swap_compare((comp_type)e->flag);
}

sql_subtype *
exp_subtype( sql_exp *e )
{
	switch(e->type) {
	case e_atom: {
		if (e->l) {
			atom *a = e->l;
			return atom_type(a);
		} else if (e->tpe.type) { /* atom reference */
			return &e->tpe;
		}
		break;
	}
	case e_convert:
	case e_column:
		if (e->tpe.type)
			return &e->tpe;
		break;
	case e_aggr: {
		sql_subaggr *a = e->f;
		if (a->res && list_length(a->res) == 1) 
			return a->res->h->data;
		return NULL;
	}
	case e_func: {
		if (e->f) {
			sql_subfunc *f = e->f;
			if (f->res && list_length(f->res) == 1) 
				return f->res->h->data;
		}
		return NULL;
	}
	case e_cmp:
		/* return bit */
	default:
		return NULL;
	}
	return NULL;
}

const char *
exp_name( sql_exp *e )
{
	if (e->alias.name)
		return e->alias.name;
	if (e->type == e_convert && e->l)
		return exp_name(e->l);
	return NULL;
}

const char *
exp_relname( sql_exp *e )
{
	if (e->alias.rname)
		return e->alias.rname;
	/*
	if (e->type == e_column && e->l)
		return e->l;
	*/
	return NULL;
}

const char *
exp_find_rel_name(sql_exp *e)
{
	if (e->alias.rname)
		return e->alias.rname;
	switch(e->type) {
	case e_column:
		if (e->l)
			return e->l;
		break;
	case e_convert:
		return exp_find_rel_name(e->l);
	default:
		return NULL;
	}
	return NULL;
}

unsigned int
exp_card( sql_exp *e )
{
	return e->card;
}

const char *
exp_func_name( sql_exp *e )
{
	if (e->type == e_func && e->f) {
		sql_subfunc *f = e->f;
		return f->func->base.name;
	}
	if (e->alias.name)
		return e->alias.name;
	if (e->type == e_convert && e->l)
		return exp_name(e->l);
	return NULL;
}

int 
exp_cmp( sql_exp *e1, sql_exp *e2)
{
	return (e1 == e2)?0:-1;
}

int 
exp_equal( sql_exp *e1, sql_exp *e2)
{
	if (e1 == e2)
		return 0;
	if (e1->alias.rname && e2->alias.rname && strcmp(e1->alias.rname, e2->alias.rname) == 0)
		return strcmp(e1->alias.name, e2->alias.name);
	return -1;
}

int 
exp_match( sql_exp *e1, sql_exp *e2)
{
	if (exp_cmp(e1, e2) == 0)
		return 1;
	if (e1->type == e2->type && e1->type == e_column) {
		if (e1->l != e2->l && (!e1->l || !e2->l || strcmp(e1->l, e2->l) != 0)) 
			return 0;
		if (!e1->r || !e2->r || strcmp(e1->r, e2->r) != 0)
			return 0;
		return 1;
	}
	if (e1->type == e2->type && e1->type == e_func) {
		if (is_identity(e1, NULL) && is_identity(e2, NULL)) {
			list *args1 = e1->l;
			list *args2 = e2->l;
			
			if (list_length(args1) == list_length(args2) && list_length(args1) == 1) {
				sql_exp *ne1 = args1->h->data;
				sql_exp *ne2 = args2->h->data;

				if (exp_match(ne1,ne2))
					return 1;
			}
		}
	}
	return 0;
}

/* list already contains matching expression */
sql_exp*
exps_find_exp( list *l, sql_exp *e) 
{
	node *n;

	if (!l || !l->h)
		return NULL;

	for(n=l->h; n; n = n->next) {
		if (exp_match(n->data, e) || exp_refers(n->data, e))
			return n->data;
	}
	return NULL;
}


/* c refers to the parent p */
int 
exp_refers( sql_exp *p, sql_exp *c)
{
	if (c->type == e_column) {
		if (!p->alias.name || !c->r || strcmp(p->alias.name, c->r) != 0)
			return 0;
		if (c->l && ((p->alias.rname && strcmp(p->alias.rname, c->l) != 0) || (!p->alias.rname && strcmp(p->l, c->l) != 0)))
			return 0;
		return 1;
	}
	return 0;
}

int
exp_match_col_exps( sql_exp *e, list *l)
{
	node *n;

	for(n=l->h; n; n = n->next) {
		sql_exp *re = n->data;
		sql_exp *re_r = re->r;
	
		if (re->type == e_cmp && re->flag == cmp_or)
			return exp_match_col_exps(e, re->l) &&
			       exp_match_col_exps(e, re->r); 

		if (re->type != e_cmp || /*re->flag != cmp_equal ||*/ !re_r || re_r->card != 1 || !exp_match_exp(e, re->l)) 
			return 0;
	} 
	return 1;
}

int
exps_match_col_exps( sql_exp *e1, sql_exp *e2)
{
	sql_exp *e1_r = e1->r;
	sql_exp *e2_r = e2->r;

	if (e1->type != e_cmp || e2->type != e_cmp)
		return 0;

	if (!is_complex_exp(e1->flag) && e1_r && e1_r->card == CARD_ATOM &&
	    !is_complex_exp(e2->flag) && e2_r && e2_r->card == CARD_ATOM)
		return exp_match_exp(e1->l, e2->l);

	if (!is_complex_exp(e1->flag) && e1_r && e1_r->card == CARD_ATOM &&
	    (e2->flag == cmp_in || e2->flag == cmp_notin))
 		return exp_match_exp(e1->l, e2->l); 
	if ((e1->flag == cmp_in || e1->flag == cmp_notin) &&
	    !is_complex_exp(e2->flag) && e2_r && e2_r->card == CARD_ATOM)
 		return exp_match_exp(e1->l, e2->l); 

	if ((e1->flag == cmp_in || e1->flag == cmp_notin) &&
	    (e2->flag == cmp_in || e2->flag == cmp_notin))
 		return exp_match_exp(e1->l, e2->l); 

	if (!is_complex_exp(e1->flag) && e1_r && e1_r->card == CARD_ATOM &&
	    e2->flag == cmp_or)
 		return exp_match_col_exps(e1->l, e2->l) &&
 		       exp_match_col_exps(e1->l, e2->r); 

	if (e1->flag == cmp_or &&
	    !is_complex_exp(e2->flag) && e2_r && e2_r->card == CARD_ATOM)
 		return exp_match_col_exps(e2->l, e1->l) &&
 		       exp_match_col_exps(e2->l, e1->r); 

	if (e1->flag == cmp_or && e2->flag == cmp_or) {
		list *l = e1->l, *r = e1->r;	
		sql_exp *el = l->h->data;
		sql_exp *er = r->h->data;

		return list_length(l) == 1 && list_length(r) == 1 &&
		       exps_match_col_exps(el, e2) &&
		       exps_match_col_exps(er, e2);
	}
	return 0;
}

int 
exp_match_list( list *l, list *r)
{
	node *n, *m;
	char *lu, *ru;
	int lc = 0, rc = 0, match = 0;

	if (!l || !r)
		return l == r;
	if (list_length(l) != list_length(r))
		return 0;
	lu = calloc(list_length(l), sizeof(char));
	ru = calloc(list_length(r), sizeof(char));
	for (n = l->h, lc = 0; n; n = n->next, lc++) {
		sql_exp *le = n->data;

		for ( m = r->h, rc = 0; m; m = m->next, rc++) {
			sql_exp *re = m->data;

			if (!ru[rc] && exp_match_exp(le,re)) {
				lu[lc] = 1;
				ru[rc] = 1;
				match = 1;
			}
		}
	}
	for (n = l->h, lc = 0; n && match; n = n->next, lc++) 
		if (!lu[lc])
			match = 0;
	for (n = r->h, rc = 0; n && match; n = n->next, rc++) 
		if (!ru[rc])
			match = 0;
	free(lu);
	free(ru);
	return match;
}

static int 
exps_equal( list *l, list *r)
{
	node *n, *m;

	if (!l || !r)
		return l == r;
	if (list_length(l) != list_length(r))
		return 0;
	for (n = l->h, m = r->h; n && m; n = n->next, m = m->next) {
		sql_exp *le = n->data, *re = m->data;

		if (!exp_match_exp(le,re))
			return 0;
	}
	return 1;
}

int 
exp_match_exp( sql_exp *e1, sql_exp *e2)
{
	if (exp_match(e1, e2))
		return 1;
	if (e1->type == e2->type) { 
		switch(e1->type) {
		case e_cmp:
			if (e1->flag == e2->flag && !is_complex_exp(e1->flag) &&
		            exp_match_exp(e1->l, e2->l) && 
			    exp_match_exp(e1->r, e2->r) && 
			    ((!e1->f && !e2->f) || exp_match_exp(e1->f, e2->f)))
				return 1;
			else if (e1->flag == e2->flag && get_cmp(e1) == cmp_or &&
		            exp_match_list(e1->l, e2->l) && 
			    exp_match_list(e1->r, e2->r))
				return 1;
			else if (e1->flag == e2->flag && is_anti(e1) == is_anti(e2) &&
				(e1->flag == cmp_in || e1->flag == cmp_notin) &&
		            exp_match_exp(e1->l, e2->l) && 
			    exp_match_list(e1->r, e2->r))
				return 1;
			else if (e1->flag == e2->flag && (e1->flag == cmp_equal || e1->flag == cmp_notequal) &&
				exp_match_exp(e1->l, e2->r) && exp_match_exp(e1->r, e2->l))
				return 1; /* = and <> operations are reflective, so exp_match_exp can be called crossed */
			break;
		case e_convert:
			if (!subtype_cmp(exp_totype(e1), exp_totype(e2)) &&
			    !subtype_cmp(exp_fromtype(e1), exp_fromtype(e2)) &&
			    exp_match_exp(e1->l, e2->l))
				return 1;
			break;
		case e_aggr:
			if (!subaggr_cmp(e1->f, e2->f) && /* equal aggregation*/
			    exps_equal(e1->l, e2->l) && 
			    e1->flag == e2->flag)
				return 1;
			break;
		case e_func:
			if (!subfunc_cmp(e1->f, e2->f) && /* equal functions */
			    exps_equal(e1->l, e2->l) &&
			    /* optional order by expressions */
			    exps_equal(e1->r, e2->r)) {
				sql_subfunc *f = e1->f;
				if (!f->func->side_effect)
					return 1;
			}
			break;
		case e_atom:
			if (e1->l && e2->l && !atom_cmp(e1->l, e2->l))
				return 1;
			break;
		default:
			break;
		}
	}
	return 0;
}

static int
exps_are_joins( list *l )
{
	node *n;

	for (n = l->h; n; n = n->next) {
		sql_exp *e = n->data;

		if (exp_is_join_exp(e))
			return -1;
	}
	return 0;
}

int
exp_is_join_exp(sql_exp *e)
{
	if (exp_is_join(e, NULL) == 0)
		return 0;
	if (e->type == e_cmp && e->flag == cmp_or && e->card >= CARD_AGGR)
		if (exps_are_joins(e->l) == 0 && exps_are_joins(e->r) == 0)
			return 0;
	return -1;
}

static int
exp_is_complex_select( sql_exp *e )
{
	switch (e->type) {
	case e_atom:
		return 0;
	case e_convert:
		return exp_is_complex_select(e->l);
	case e_func:
	case e_aggr:
	{	
		int r = (e->card == CARD_ATOM);
		node *n;
		list *l = e->l;

		if (r && l)
			for (n = l->h; n && !r; n = n->next) 
				r |= exp_is_complex_select(n->data);
		return r;
	}
	case e_column:
	case e_cmp:
	default:
		return 0;
	case e_psm:
		return 1;
	}
}

static int
complex_select(sql_exp *e)
{
	sql_exp *l = e->l, *r = e->r;

	if (exp_is_complex_select(l) || exp_is_complex_select(r))
		return 1;
	return 0;
}

static int
distinct_rel(sql_exp *e, const char **rname)
{
	const char *e_rname = NULL;

	switch(e->type) {
	case e_column:
		e_rname = exp_relname(e);

		if (*rname && e_rname && strcmp(*rname, e_rname) == 0)
			return 1;
		if (!*rname) {
			*rname = e_rname;
			return 1;
		}
		break;
	case e_aggr:
	case e_func: 
		if (e->l) {
			int m = 1;
			list *l = e->l;
			node *n;
	
			for(n=l->h; n && m; n = n->next) {
				sql_exp *ae = n->data; 

				m = distinct_rel(ae, rname);
			}
			return m;
		}
		return 0;
	case e_atom:
		return 1;
	case e_convert:
		return distinct_rel(e->l, rname);
	default:
		return 0;
	}
	return 0;
}

int
rel_has_exp(sql_rel *rel, sql_exp *e) 
{
	if (rel_find_exp(rel, e) != NULL) 
		return 0;
	return -1;
}

int
rel_has_exps(sql_rel *rel, list *exps)
{
	node *n;

	if (!exps)
		return -1;
	for (n = exps->h; n; n = n->next)
		if (rel_has_exp(rel, n->data) >= 0)
			return 0;
	return -1;
}

int
rel_has_all_exps(sql_rel *rel, list *exps)
{
	node *n;

	if (!exps)
		return -1;
	for (n = exps->h; n; n = n->next)
		if (rel_has_exp(rel, n->data) < 0)
			return 0;
	return 1;
}


sql_rel *
find_rel(list *rels, sql_exp *e)
{
	node *n = list_find(rels, e, (fcmp)&rel_has_exp);
	if (n) 
		return n->data;
	return NULL;
}

sql_rel *
find_one_rel(list *rels, sql_exp *e)
{
	node *n;
	sql_rel *fnd = NULL;

	for(n = rels->h; n; n = n->next) {
		if (rel_has_exp(n->data, e) == 0) {
			if (fnd)
				return NULL;
			fnd = n->data;
		}
	}
	return fnd;
}

static int 
exp_is_rangejoin(sql_exp *e, list *rels)
{
	/* assume e is a e_cmp with 3 args 
	 * Need to check e->r and e->f only touch one table.
	 */
	const char *rname = 0;

	if (distinct_rel(e->r, &rname) && distinct_rel(e->f, &rname))
		return 0;
	if (rels) { 
		sql_rel *r = find_rel(rels, e->r);
		sql_rel *f = find_rel(rels, e->f);
		if (r && f && r == f)
			return 0;
	}
	return -1;
}

int
exp_is_join(sql_exp *e, list *rels)
{
	/* only simple compare expressions, ie not or lists
		or range expressions (e->f)
	 */
	if (e->type == e_cmp && !is_complex_exp(e->flag) && e->l && e->r && !e->f && e->card >= CARD_AGGR && !complex_select(e))
		return 0;
	if (e->type == e_cmp && get_cmp(e) == cmp_filter && e->l && e->r && e->card >= CARD_AGGR)
		return 0;
	/* range expression */
	if (e->type == e_cmp && !is_complex_exp(e->flag) && e->l && e->r && e->f && e->card >= CARD_AGGR && !complex_select(e)) 
		return exp_is_rangejoin(e, rels);
	return -1;
}

int
exp_is_eqjoin(sql_exp *e)
{
	if (e->flag == cmp_equal) {
		sql_exp *l = e->l;
		sql_exp *r = e->r;

		if (!is_func(l->type) && !is_func(r->type)) 
			return 0;
	}
	return -1; 
}

static sql_exp *
rel_find_exp_( sql_rel *rel, sql_exp *e) 
{
	sql_exp *ne = NULL;

	if (!rel)
		return NULL;
	switch(e->type) {
	case e_column:
		if (rel->exps && (is_project(rel->op) || is_base(rel->op))) {
			if (e->l) {
				ne = exps_bind_column2(rel->exps, e->l, e->r);
			} else {
				ne = exps_bind_column(rel->exps, e->r, NULL);
			}
		}
		return ne;
	case e_convert:
		return rel_find_exp_(rel, e->l);
	case e_aggr:
	case e_func: 
		if (e->l) {
			list *l = e->l;
			node *n = l->h;
	
			ne = n->data;
			while (ne != NULL && n != NULL) {
				ne = rel_find_exp_(rel, n->data);
				n = n->next;
			}
			return ne;
		}
		/* fall through */
	case e_cmp:	
	case e_psm:	
		return NULL;
	case e_atom:
		return e;
	}
	return ne;
}

sql_exp *
rel_find_exp( sql_rel *rel, sql_exp *e)
{
	sql_exp *ne = rel_find_exp_(rel, e);

	if (rel && !ne) {
		switch(rel->op) {
		case op_left:
		case op_right:
		case op_full:
		case op_join:
			ne = rel_find_exp(rel->l, e);
			if (!ne) 
				ne = rel_find_exp(rel->r, e);
			break;
		case op_table:
			if (rel->exps && e->type == e_column && e->l && exps_bind_column2(rel->exps, e->l, e->r)) 
				ne = e;
			break;
		case op_union:
		case op_except:
		case op_inter:
		{
			if (rel->l)
				ne = rel_find_exp(rel->l, e);
			else if (rel->exps && e->l)
				ne = exps_bind_column2(rel->exps, e->l, e->r);
			else if (rel->exps)
				ne = exps_bind_column(rel->exps, e->r, NULL);
		}
		break;
		case op_basetable: 
			if (rel->exps && e->type == e_column && e->l) 
				ne = exps_bind_column2(rel->exps, e->l, e->r);
			break;
		default:
			if (!is_project(rel->op) && rel->l)
				ne = rel_find_exp(rel->l, e);
		}
	}
	return ne;
}

int 
exp_is_correlation(sql_exp *e, sql_rel *r )
{
	if (e->type == e_cmp && !is_complex_exp(e->flag)) {
		sql_exp *le = rel_find_exp(r->l, e->l);
		sql_exp *re = rel_find_exp(r->r, e->r);

		if (le && re)
			return 0;
		le = rel_find_exp(r->r, e->l);
		re = rel_find_exp(r->l, e->r);
		if (le && re) {
			/* for future processing we depend on 
			   the correct order of the expression, ie swap here */
			exp_swap(e);
			return 0;
		}
	}
	return -1;
}

int
exp_is_true(mvc *sql, sql_exp *e) 
{
	if (e->type == e_atom) {
		if (e->l) {
			return atom_is_true(e->l);
		} else if(sql->emode == m_normal && (unsigned) sql->argc > e->flag && EC_BOOLEAN(exp_subtype(e)->type->eclass)) {
			return atom_is_true(sql->args[e->flag]);
		}
	}
	return 0;
}

int
exp_is_zero(mvc *sql, sql_exp *e) 
{
	if (e->type == e_atom) {
		if (e->l) {
			return atom_is_zero(e->l);
		} else if(sql->emode == m_normal && (unsigned) sql->argc > e->flag && EC_COMPUTE(exp_subtype(e)->type->eclass)) {
			return atom_is_zero(sql->args[e->flag]);
		}
	}
	return 0;
}

int
exp_is_not_null(mvc *sql, sql_exp *e) 
{
	if (e->type == e_atom) {
		if (e->l) {
			return !(atom_null(e->l));
		} else if(sql->emode == m_normal && (unsigned) sql->argc > e->flag && EC_COMPUTE(exp_subtype(e)->type->eclass)) {
			return !atom_null(sql->args[e->flag]);
		}
	}
	return 0;
}

int
exp_is_null(mvc *sql, sql_exp *e )
{
	switch (e->type) {
	case e_atom:
		if (e->f) /* values list */
			return 0;
		if (e->l) {
			return (atom_null(e->l));
		} else if (sql->emode == m_normal && (unsigned) sql->argc > e->flag) {
			return atom_null(sql->args[e->flag]);
		}
		return 0;
	case e_convert:
		return exp_is_null(sql, e->l);
	case e_func:
	case e_aggr:
	{	
		int r = 0;
		node *n;
		list *l = e->l;

		if (!r && l && list_length(l) == 2) {
			for (n = l->h; n && !r; n = n->next) 
				r |= exp_is_null(sql, n->data);
		}
		return r;
	}
	case e_column:
	case e_cmp:
	case e_psm:
		return 0;
	}
	return 0;
}

int
exp_is_atom( sql_exp *e )
{
	switch (e->type) {
	case e_atom:
		if (e->f) /* values list */
			return 0;
		return 1;
	case e_convert:
		return exp_is_atom(e->l);
	case e_func:
	case e_aggr:
	{	
		int r = (e->card == CARD_ATOM);
		node *n;
		list *l = e->l;

		if (r && l)
			for (n = l->h; n && r; n = n->next) 
				r &= exp_is_atom(n->data);
		return r;
	}
	case e_column:
	case e_cmp:
	case e_psm:
		return 0;
	}
	return 0;
}

int
exps_are_atoms( list *exps)
{
	node *n;
	int atoms = 1;

	for(n=exps->h; n && atoms; n=n->next) 
		atoms &= exp_is_atom(n->data);
	return atoms;
}

static int
exps_has_func( list *exps)
{
	node *n;
	int has_func = 0;

	for(n=exps->h; n && !has_func; n=n->next) 
		has_func |= exp_has_func(n->data);
	return has_func;
}

int
exp_has_func( sql_exp *e )
{
	switch (e->type) {
	case e_atom:
		return 0;
	case e_convert:
		return exp_has_func(e->l);
	case e_func:
		return 1;
	case e_aggr:
		if (e->l)
			return exps_has_func(e->l);
		return 0;
	case e_cmp:
		if (get_cmp(e) == cmp_or) {
			return (exps_has_func(e->l) || exps_has_func(e->r));
		} else if (e->flag == cmp_in || e->flag == cmp_notin || get_cmp(e) == cmp_filter) {
			return (exp_has_func(e->l) || exps_has_func(e->r));
		} else {
			return (exp_has_func(e->l) || exp_has_func(e->r) || 
					(e->f && exp_has_func(e->f)));
		}
	case e_column:
	case e_psm:
		return 0;
	}
	return 0;
}

static int
exps_has_sideeffect( list *exps)
{
	node *n;
	int has_sideeffect = 0;

	for(n=exps->h; n && !has_sideeffect; n=n->next) 
		has_sideeffect |= exp_has_sideeffect(n->data);
	return has_sideeffect;
}

int
exp_has_sideeffect( sql_exp *e )
{
	switch (e->type) {
	case e_convert:
		return exp_has_sideeffect(e->l);
	case e_func:
		{
			sql_subfunc *f = e->f;

			if (f->func->side_effect) 
				return 1;
			if (e->l)
				return exps_has_sideeffect(e->l);
			return 0;
		}
	case e_atom:
	case e_aggr: 
	case e_cmp:
	case e_column:
	case e_psm:
		return 0;
	}
	return 0;
}

int
exp_unsafe( sql_exp *e, int allow_identity) 
{
	if (!e)
		return 0;

	if (e->type != e_func && e->type != e_convert)
		return 0;

	if (e->type == e_convert && e->l)
		return exp_unsafe(e->l, allow_identity);
	if (e->type == e_func && e->l) {
		sql_subfunc *f = e->f;
		list *args = e->l;
		node *n;

		if (IS_ANALYTIC(f->func) || (!allow_identity && is_identity(e, NULL)))
			return 1;
		for(n = args->h; n; n = n->next) {
			sql_exp *e = n->data;

			if (exp_unsafe(e, allow_identity))
				return 1;			
		}
	}
	return 0;
}

static int
exp_key( sql_exp *e )
{
	if (e->alias.name)
		return hash_key(e->alias.name);
	return 0;
}

sql_exp *
exps_bind_column( list *exps, const char *cname, int *ambiguous ) 
{
	sql_exp *e = NULL;

	if (exps && cname) {
		node *en;

		if (exps) {
			MT_lock_set(&exps->ht_lock);
			if (!exps->ht && list_length(exps) > HASH_MIN_SIZE) {
				exps->ht = hash_new(exps->sa, list_length(exps), (fkeyvalue)&exp_key);
				if (exps->ht == NULL) {
					MT_lock_unset(&exps->ht_lock);
					return NULL;
				}
				for (en = exps->h; en; en = en->next ) {
					sql_exp *e = en->data;
					if (e->alias.name) {
						int key = exp_key(e);

						if (hash_add(exps->ht, key, e) == NULL) {
							MT_lock_unset(&exps->ht_lock);
							return NULL;
						}
					}
				}
			}
			if (exps->ht) {
				int key = hash_key(cname);
				sql_hash_e *he = exps->ht->buckets[key&(exps->ht->size-1)]; 

				for (; he; he = he->chain) {
					sql_exp *ce = he->value;

					if (ce->alias.name && strcmp(ce->alias.name, cname) == 0) {
						if (e && e != ce && ce->alias.rname && e->alias.rname && strcmp(ce->alias.rname, e->alias.rname) != 0 ) {
							if (ambiguous)
								*ambiguous = 1;
							MT_lock_unset(&exps->ht_lock);
							return NULL;
						}
						e = ce;
					}
				}
				MT_lock_unset(&exps->ht_lock);
				return e;
			}
			MT_lock_unset(&exps->ht_lock);
		}
		for (en = exps->h; en; en = en->next ) {
			sql_exp *ce = en->data;
			if (ce->alias.name && strcmp(ce->alias.name, cname) == 0) {
				if (e) {
					if (ambiguous)
						*ambiguous = 1;
					return NULL;
				}
				e = ce;
			}
		}
	}
	return e;
}

sql_exp *
exps_bind_column2( list *exps, const char *rname, const char *cname ) 
{
	if (exps) {
		node *en;

		if (exps) {
			MT_lock_set(&exps->ht_lock);
			if (!exps->ht && list_length(exps) > HASH_MIN_SIZE) {
				exps->ht = hash_new(exps->sa, list_length(exps), (fkeyvalue)&exp_key);
				if (exps->ht == NULL) {
					MT_lock_unset(&exps->ht_lock);
					return NULL;
				}

				for (en = exps->h; en; en = en->next ) {
					sql_exp *e = en->data;
					if (e->alias.name) {
						int key = exp_key(e);

						if (hash_add(exps->ht, key, e) == NULL) {
							MT_lock_unset(&exps->ht_lock);
							return NULL;
						}
					}
				}
			}
			if (exps->ht) {
				int key = hash_key(cname);
				sql_hash_e *he = exps->ht->buckets[key&(exps->ht->size-1)]; 

				for (; he; he = he->chain) {
					sql_exp *e = he->value;

					if ((e && is_column(e->type) && e->alias.name && e->alias.rname && strcmp(e->alias.name, cname) == 0 && strcmp(e->alias.rname, rname) == 0) ||
					    (e && e->type == e_column && e->alias.name && !e->alias.rname && e->l && strcmp(e->alias.name, cname) == 0 && strcmp(e->l, rname) == 0)) {
						MT_lock_unset(&exps->ht_lock);
						return e;
					}
				}
				MT_lock_unset(&exps->ht_lock);
				return NULL;
			}
			MT_lock_unset(&exps->ht_lock);
		}
		for (en = exps->h; en; en = en->next ) {
			sql_exp *e = en->data;
		
			if (e && is_column(e->type) && e->alias.name && e->alias.rname && strcmp(e->alias.name, cname) == 0 && strcmp(e->alias.rname, rname) == 0)
				return e;
			if (e && e->type == e_column && e->alias.name && !e->alias.rname && e->l && strcmp(e->alias.name, cname) == 0 && strcmp(e->l, rname) == 0)
				return e;
		}
	}
	return NULL;
}

/* find an column based on the original name, not the alias it got */
sql_exp *
exps_bind_alias( list *exps, const char *rname, const char *cname ) 
{
	if (exps) {
		node *en;

		for (en = exps->h; en; en = en->next ) {
			sql_exp *e = en->data;
		
			if (e && is_column(e->type) && !rname && e->r && strcmp(e->r, cname) == 0)
				return e;
			if (e && e->type == e_column && rname && e->l && e->r && strcmp(e->r, cname) == 0 && strcmp(e->l, rname) == 0) {
				return e;
			}
		}
	}
	return NULL;
}

unsigned int
exps_card( list *l ) 
{
	node *n;
	unsigned int card = CARD_ATOM;

	if (l) for(n = l->h; n; n = n->next) {
		sql_exp *e = n->data;

		if (card < e->card)
			card = e->card;
	}
	return card;
}
	
void
exps_fix_card( list *exps, unsigned int card)
{
	node *n;

	for (n = exps->h; n; n = n->next) {
		sql_exp *e = n->data;

		if (e->card > card)
			e->card = card;
	}
}

void
exps_setcard( list *exps, unsigned int card)
{
	node *n;

	for (n = exps->h; n; n = n->next) {
		sql_exp *e = n->data;

		if (e->card != CARD_ATOM)
			e->card = card;
	}
}

int
exps_intern(list *exps)
{
	node *n;
			
	for (n=exps->h; n; n = n->next) {
		sql_exp *e = n->data;		

		if (is_intern(e))
			return 1;
	}
	return 0;
}

char *
compare_func( comp_type t, int anti )
{
	switch(t) {
	case mark_in:
	case cmp_equal:
		return anti?"<>":"=";
	case cmp_lt:
		return anti?">":"<";
	case cmp_lte:
		return anti?">=":"<=";
	case cmp_gte:
		return anti?"<=":">=";
	case cmp_gt:
		return anti?"<":">";
	case mark_notin:
	case cmp_notequal:
		return anti?"=":"<>";
	default:
		return NULL;
	}
}

int
is_identity( sql_exp *e, sql_rel *r)
{
	switch(e->type) {
	case e_column:
		if (r && is_project(r->op)) {
			sql_exp *re = NULL;
			if (e->l)
				re = exps_bind_column2(r->exps, e->l, e->r);
			if (!re && has_label(e))
				re = exps_bind_column(r->exps, e->r, NULL);
			if (re)
				return is_identity(re, r->l);
		}
		return 0;
	case e_func: {
		sql_subfunc *f = e->f;
		return (strcmp(f->func->base.name, "identity") == 0);
	}
	default:
		return 0;
	}
}

list *
exps_alias( sql_allocator *sa, list *exps)
{
	node *n;
	list *nl = new_exp_list(sa);

	for (n = exps->h; n; n = n->next) {
		sql_exp *e = n->data, *ne;

		assert(exp_name(e));
		ne = exp_ref(sa, e);
		append(nl, ne);
	}
	return nl;
}

list *
exps_copy( sql_allocator *sa, list *exps)
{
	node *n;
	list *nl;

	if (!exps)
		return exps;

	nl = new_exp_list(sa);
	for(n = exps->h; n; n = n->next) {
		sql_exp *arg = n->data;

		arg = exp_copy(sa, arg);
		if (!arg) 
			return NULL;
		append(nl, arg);
	}
	return nl;
}

sql_exp *
exp_copy( sql_allocator *sa, sql_exp * e)
{
	sql_exp *l, *r, *r2, *ne = NULL;

	switch(e->type){
	case e_column:
		ne = exp_column(sa, e->l, e->r, exp_subtype(e), e->card, has_nil(e), is_intern(e));
		ne->flag = e->flag;
		break;
	case e_cmp:
		if (get_cmp(e) == cmp_or || get_cmp(e) == cmp_filter) {
			list *l = exps_copy(sa, e->l);
			list *r = exps_copy(sa, e->r);
			if (l && r) {
				if (get_cmp(e) == cmp_filter)
					ne = exp_filter(sa, l, r, e->f, is_anti(e));
				else
					ne = exp_or(sa, l, r, is_anti(e));
			}
		} else if (e->flag == cmp_in || e->flag == cmp_notin) {
			sql_exp *l = exp_copy(sa, e->l);
			list *r = exps_copy(sa, e->r);

			if (l && r) 
				ne = exp_in(sa, l, r, e->flag);
		} else {
			l = exp_copy(sa, e->l);
			r = exp_copy(sa, e->r);

			if (e->f) {
				r2 = exp_copy(sa, e->f);
				if (l && r && r2)
					ne = exp_compare2(sa, l, r, r2, e->flag);
			} else if (l && r) {
				ne = exp_compare(sa, l, r, e->flag);
			}
		}
		break;
	case e_convert:
		l = exp_copy(sa, e->l);
		if (l)
			ne = exp_convert(sa, l, exp_fromtype(e), exp_totype(e));
		break;
	case e_aggr:
	case e_func: {
		list *l = e->l, *nl = NULL;

		if (!l) {
			return e;
		} else {
			nl = exps_copy(sa, l);
			if (!nl)
				return NULL;
		}
		if (e->type == e_func)
			ne = exp_op(sa, nl, e->f);
		else 
			ne = exp_aggr(sa, nl, e->f, need_distinct(e), need_no_nil(e), e->card, has_nil(e));
		break;
	}	
	case e_atom:
		if (e->l)
			ne = exp_atom(sa, e->l);
		else if (!e->r)
			ne = exp_atom_ref(sa, e->flag, &e->tpe);
		else 
			ne = exp_param(sa, e->r, &e->tpe, e->flag);
		break;
	case e_psm:
		if (e->flag == PSM_SET) 
			ne = exp_set(sa, e->alias.name, exp_copy(sa, e->l), GET_PSM_LEVEL(e->flag));
		break;
	}
	if (!ne)
		return ne;
	if (e->alias.name)
		//exp_setname(sa, ne, exp_find_rel_name(e), exp_name(e));
		exp_prop_alias(ne, e);
	ne = exp_propagate(sa, ne, e);
	if (is_freevar(e))
		set_freevar(ne);
	return ne;
}

atom *
exp_flatten(mvc *sql, sql_exp *e) 
{
	if (e->type == e_atom) {
		atom *v =  exp_value(sql, e, sql->args, sql->argc);

		if (v)
			return atom_dup(sql->sa, v);
	} else if (e->type == e_convert) {
		atom *v = exp_flatten(sql, e->l); 

		if (v && atom_cast(sql->sa, v, &e->tpe))
			return v;
		return NULL;
	} else if (e->type == e_func) {
		sql_subfunc *f = e->f;
		list *l = e->l;
		sql_arg *res = (f->func->res)?(f->func->res->h->data):NULL;

		/* TODO handle date + x months */
		if (strcmp(f->func->base.name, "sql_add") == 0 && list_length(l) == 2 && res && EC_NUMBER(res->type.type->eclass)) {
			atom *l1 = exp_flatten(sql, l->h->data);
			atom *l2 = exp_flatten(sql, l->h->next->data);
			if (l1 && l2)
				return atom_add(l1,l2);
		} else if (strcmp(f->func->base.name, "sql_sub") == 0 && list_length(l) == 2 && res && EC_NUMBER(res->type.type->eclass)) {
			atom *l1 = exp_flatten(sql, l->h->data);
			atom *l2 = exp_flatten(sql, l->h->next->data);
			if (l1 && l2)
				return atom_sub(l1,l2);
		}
	}
	return NULL;
}

void
exp_sum_scales(sql_subfunc *f, sql_exp *l, sql_exp *r)
{
	sql_arg *ares = f->func->res->h->data;

	if (strcmp(f->func->imp, "*") == 0 && ares->type.type->scale == SCALE_FIX) {
		sql_subtype t;
		sql_subtype *lt = exp_subtype(l);
		sql_subtype *rt = exp_subtype(r);
		sql_subtype *res = f->res->h->data;

		res->scale = lt->scale + rt->scale;
		res->digits = lt->digits + rt->digits;

		/* HACK alert: digits should be less than max */
#ifdef HAVE_HGE
		if (have_hge) {
			if (ares->type.type->radix == 10 && res->digits > 39)
				res->digits = 39;
			if (ares->type.type->radix == 2 && res->digits > 128)
				res->digits = 128;
		} else
#endif
		{

			if (ares->type.type->radix == 10 && res->digits > 19)
				res->digits = 19;
			if (ares->type.type->radix == 2 && res->digits > 64)
				res->digits = 64;
		}

		/* numeric types are fixed length */
		if (ares->type.type->eclass == EC_NUM) {
#ifdef HAVE_HGE
			if (have_hge && ares->type.type->localtype == TYPE_hge && res->digits == 128)
				t = *sql_bind_localtype("hge");
			else
#endif
			if (ares->type.type->localtype == TYPE_lng && res->digits == 64)
				t = *sql_bind_localtype("lng");
			else
				sql_find_numeric(&t, ares->type.type->localtype, res->digits);
		} else {
			sql_find_subtype(&t, ares->type.type->sqlname, res->digits, res->scale);
		}
		*res = t;
	}
}

sql_exp *
create_table_part_atom_exp(mvc *sql, sql_subtype tpe, ptr value)
{
	str buf = NULL;
	size_t len = 0;
	sql_exp *res = NULL;

	switch (tpe.type->eclass) {
		case EC_BIT: {
			bit bval = *((bit*) value);
			return exp_atom_bool(sql->sa, bval ? 1 : 0);
		}
		case EC_POS:
		case EC_NUM:
		case EC_DEC:
		case EC_SEC:
		case EC_MONTH:
			switch (tpe.type->localtype) {
#ifdef HAVE_HGE
				case TYPE_hge: {
					hge hval = *((hge*) value);
					return exp_atom_hge(sql->sa, hval);
				}
#endif
				case TYPE_lng: {
					lng lval = *((lng*) value);
					return exp_atom_lng(sql->sa, lval);
				}
				case TYPE_int: {
					int ival = *((int*) value);
					return exp_atom_int(sql->sa, ival);
				}
				case TYPE_sht: {
					sht sval = *((sht*) value);
					return exp_atom_sht(sql->sa, sval);
				}
				case TYPE_bte: {
					bte bbval = *((bte *) value);
					return exp_atom_bte(sql->sa, bbval);
				}
				default:
					return NULL;
			}
		case EC_FLT:
			switch (tpe.type->localtype) {
				case TYPE_flt: {
					flt fval = *((flt*) value);
					return exp_atom_flt(sql->sa, fval);
				}
				case TYPE_dbl: {
					dbl dval = *((dbl*) value);
					return exp_atom_dbl(sql->sa, dval);
				}
				default:
					return NULL;
			}
		case EC_DATE: {
			if(date_tostr(&buf, &len, (const date *)value, false) < 0)
				return NULL;
			res = exp_atom(sql->sa, atom_general(sql->sa, &tpe, buf));
			break;
		}
		case EC_TIME: {
			if(daytime_tostr(&buf, &len, (const daytime *)value, false) < 0)
				return NULL;
			res = exp_atom(sql->sa, atom_general(sql->sa, &tpe, buf));
			break;
		}
		case EC_TIMESTAMP: {
			if(timestamp_tostr(&buf, &len, (const timestamp *)value, false) < 0)
				return NULL;
			res = exp_atom(sql->sa, atom_general(sql->sa, &tpe, buf));
			break;
		}
		case EC_BLOB: {
			if(BLOBtostr(&buf, &len, (const blob *)value, false) < 0)
				return NULL;
			res = exp_atom(sql->sa, atom_general(sql->sa, &tpe, buf));
			break;
		}
		case EC_CHAR:
		case EC_STRING:
			return exp_atom_clob(sql->sa, sa_strdup(sql->sa, value));
		default:
			assert(0);
	}
	if(buf)
		GDKfree(buf);
	return res;
}

int 
exp_aggr_is_count(sql_exp *e)
{
	if (e->type == e_aggr && strcmp(((sql_subaggr *)e->f)->aggr->base.name, "count") == 0)
		return 1;
	return 0;
}

void
exps_reset_freevar(list *exps)
{
	node *n;

	for(n=exps->h; n; n=n->next) {
		sql_exp *e = n->data;

		/*later use case per type */
		reset_freevar(e);
	}
}

static int
exp_set_list_recurse(mvc *sql, sql_subtype *type, sql_exp *e, const char **relname, const char** expname)
{
	if (THRhighwater()) {
		(void) sql_error(sql, 10, SQLSTATE(42000) "Query too complex: running out of stack space");
		return -1;
	}
	assert(*relname && *expname);
	if (!e)
		return 0;

	if (e->f) {
		const char *next_rel = exp_relname(e), *next_exp = exp_name(e);
		if (next_rel && next_exp && !strcmp(next_rel, *relname) && !strcmp(next_exp, *expname))
			for (node *n = ((list *) e->f)->h; n; n = n->next)
				exp_set_list_recurse(sql, type, (sql_exp *) n->data, relname, expname);
	}
	if ((e->f || (!e->l && !e->r && !e->f)) && !e->tpe.type) {
		if (set_type_param(sql, type, e->flag) == 0)
			e->tpe = *type;
		else
			return -1;
	}
	return 0;
}

static int
exp_set_type_recurse(mvc *sql, sql_subtype *type, sql_exp *e, const char **relname, const char** expname)
{
	if (THRhighwater()) {
		(void) sql_error(sql, 10, SQLSTATE(42000) "Query too complex: running out of stack space");
		return -1;
	}
	assert(*relname && *expname);
	if (!e)
		return 0;

	switch (e->type) {
		case e_atom: {
			return exp_set_list_recurse(sql, type, e, relname, expname);
		} break;
		case e_convert:
		case e_column: {
			/* if the column pretended is found, set its type */
			const char *next_rel = exp_relname(e), *next_exp = exp_name(e);
			if (next_rel && !strcmp(next_rel, *relname)) {
				*relname = (e->type == e_column && e->l) ? (const char*) e->l : next_rel;
				if (next_exp && !strcmp(next_exp, *expname)) {
					*expname = (e->type == e_column && e->r) ? (const char*) e->r : next_exp;
					if (e->type == e_column && !e->tpe.type) {
						if (set_type_param(sql, type, e->flag) == 0)
							e->tpe = *type;
						else
							return -1;
					}
				}
			}
			if (e->type == e_convert)
				exp_set_type_recurse(sql, type, e->l, relname, expname);
		} break;
		case e_psm: {
			if (e->flag & PSM_RETURN) {
				for(node *n = ((list*)e->r)->h ; n ; n = n->next)
					exp_set_type_recurse(sql, type, (sql_exp*) n->data, relname, expname);
			} else if (e->flag & PSM_WHILE) {
				exp_set_type_recurse(sql, type, e->l, relname, expname);
				for(node *n = ((list*)e->r)->h ; n ; n = n->next)
					exp_set_type_recurse(sql, type, (sql_exp*) n->data, relname, expname);
			} else if (e->flag & PSM_IF) {
				exp_set_type_recurse(sql, type, e->l, relname, expname);
				for(node *n = ((list*)e->r)->h ; n ; n = n->next)
					exp_set_type_recurse(sql, type, (sql_exp*) n->data, relname, expname);
				if (e->f)
					for(node *n = ((list*)e->f)->h ; n ; n = n->next)
						exp_set_type_recurse(sql, type, (sql_exp*) n->data, relname, expname);
			} else if (e->flag & PSM_REL) {
				rel_set_type_recurse(sql, type, e->l, relname, expname);
			} else if (e->flag & PSM_EXCEPTION) {
				exp_set_type_recurse(sql, type, e->l, relname, expname);
			}
		} break;
		case e_func: {
			for(node *n = ((list*)e->l)->h ; n ; n = n->next)
				exp_set_type_recurse(sql, type, (sql_exp*) n->data, relname, expname);
			if (e->r)
				for(node *n = ((list*)e->r)->h ; n ; n = n->next)
					exp_set_type_recurse(sql, type, (sql_exp*) n->data, relname, expname);
		} 	break;
		case e_aggr: {
			if (e->l)
				for(node *n = ((list*)e->l)->h ; n ; n = n->next)
					exp_set_type_recurse(sql, type, (sql_exp*) n->data, relname, expname);
		} 	break;
		case e_cmp: {
			if (e->flag == cmp_in || e->flag == cmp_notin) {
				exp_set_type_recurse(sql, type, e->l, relname, expname);
				for(node *n = ((list*)e->r)->h ; n ; n = n->next)
					exp_set_type_recurse(sql, type, (sql_exp*) n->data, relname, expname);
			} else if (get_cmp(e) == cmp_or || get_cmp(e) == cmp_filter) {
				for(node *n = ((list*)e->l)->h ; n ; n = n->next)
					exp_set_type_recurse(sql, type, (sql_exp*) n->data, relname, expname);
				for(node *n = ((list*)e->r)->h ; n ; n = n->next)
					exp_set_type_recurse(sql, type, (sql_exp*) n->data, relname, expname);
			} else {
				if(e->l)
					exp_set_type_recurse(sql, type, e->l, relname, expname);
				if(e->r)
					exp_set_type_recurse(sql, type, e->r, relname, expname);
				if(e->f)
					exp_set_type_recurse(sql, type, e->f, relname, expname);
			}
		} break;
	}
	return 0;
}

int
rel_set_type_recurse(mvc *sql, sql_subtype *type, sql_rel *rel, const char **relname, const char **expname)
{
	if (THRhighwater()) {
		(void) sql_error(sql, 10, SQLSTATE(42000) "Query too complex: running out of stack space");
		return -1;
	}
	assert(*relname && *expname);
	if (!rel)
		return 0;

	if (rel->exps)
		for(node *n = rel->exps->h; n; n = n->next)
			exp_set_type_recurse(sql, type, (sql_exp*) n->data, relname, expname);

	switch (rel->op) {
		case op_basetable:
		case op_table:
		case op_ddl:
			break;
		case op_join:
		case op_left:
		case op_right:
		case op_full:
		case op_semi:
		case op_anti:
		case op_union:
		case op_inter:
		case op_except:
			if (rel->l)
				rel_set_type_recurse(sql, type, rel->l, relname, expname);
			if (rel->r)
				rel_set_type_recurse(sql, type, rel->r, relname, expname);
			break;
		case op_groupby:
		case op_project:
		case op_select:
		case op_topn:
		case op_sample:
			if (rel->l)
				rel_set_type_recurse(sql, type, rel->l, relname, expname);
			break;
		case op_insert:
		case op_update:
		case op_delete:
		case op_truncate:
			if (rel->r)
				rel_set_type_recurse(sql, type, rel->r, relname, expname);
			break;
	}
	return 0;
}
