/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0.  If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Copyright 1997 - July 2008 CWI, August 2008 - 2019 MonetDB B.V.
 */

#include "monetdb_config.h"
#include "rel_rel.h"
#include "rel_exp.h"
#include "rel_prop.h"
#include "rel_remote.h"
#include "rel_unnest.h"
#include "sql_semantic.h"
#include "sql_mvc.h"


/* we don't name relations directly, but sometimes we need the relation
   name. So we look it up in the first expression

   we should clean up (remove) this function.
 */
const char *
rel_name( sql_rel *r )
{
	if (!is_project(r->op) && !is_base(r->op) && r->l) 
		return rel_name(r->l);
	if (r->exps && list_length(r->exps)) {
		sql_exp *e = r->exps->h->data;
		if (exp_relname(e))
			return exp_relname(e);
		if (e->type == e_column)
			return e->l;
	}
	return NULL;
}

sql_rel *
rel_distinct(sql_rel *l)
{
	if (l->card >= CARD_AGGR) /* in case of CARD_AGGR, we could
	                             do better, ie check the group by
	                             list etc */
		set_distinct(l);
	return l;
}

sql_rel *
rel_dup(sql_rel *r)
{
	sql_ref_inc(&r->ref);
	return r;
}

static void
rel_destroy_(sql_rel *rel)
{
	if (!rel)
		return;
	if (is_join(rel->op) ||
	    is_semi(rel->op) ||
	    is_select(rel->op) ||
	    is_set(rel->op) ||
	    rel->op == op_topn ||
		rel->op == op_sample) {
		if (rel->l)
			rel_destroy(rel->l);
		if (rel->r)
			rel_destroy(rel->r);
	} else if (is_project(rel->op)) {
		if (rel->l)
			rel_destroy(rel->l);
	} else if (is_modify(rel->op)) {
		if (rel->r)
			rel_destroy(rel->r);
	}
}

void
rel_destroy(sql_rel *rel)
{
	if (!rel)
		return;
	if (sql_ref_dec(&rel->ref) > 0)
		return;
	rel_destroy_(rel);
}

sql_rel*
rel_create( sql_allocator *sa )
{
	sql_rel *r = SA_NEW(sa, sql_rel);
	if(!r)
		return NULL;

	sql_ref_init(&r->ref);
	r->l = r->r = NULL;
	r->exps = NULL;
	r->nrcols = 0;
	r->flag = 0;
	r->card = CARD_ATOM;
	r->processed = 0;
	r->single = 0;
	r->dependent = 0;
	r->subquery = 0;
	r->p = NULL;
	return r;
}

sql_rel *
rel_copy( sql_allocator *sa, sql_rel *i, int deep )
{
	sql_rel *rel = rel_create(sa);
	if(!rel)
		return NULL;

	rel->l = NULL;
	rel->r = NULL;
	rel->card = i->card;
	rel->flag = i->flag;

	switch(i->op) {
	case op_basetable:
		rel->l = i->l;
		break;
	case op_table:
		rel->l = i->l;
		rel->r = i->r;
		break;
	case op_groupby:
		rel->l = rel_copy(sa, i->l, deep);
		if (i->r) {
			if (!deep) {
				rel->r = list_dup(i->r, (fdup) NULL);
			} else {
				list* l = (list*)i->r;
				rel->r = list_new(l->sa, l->destroy);
				for(node *n = l->h ; n ; n = n->next)
					list_append(rel->r, rel_copy(sa, (sql_rel *)n->data, deep));
			}
		}
		break;
	case op_join:
	case op_left:
	case op_right:
	case op_full:
	case op_semi:
	case op_anti:
	case op_project:
	case op_select:
	default:
		if (i->l)
			rel->l = rel_copy(sa, i->l, deep);
		if (i->r)
			rel->r = rel_copy(sa, i->r, deep);
		break;
	}
	rel->op = i->op;
	rel->exps = (!i->exps)?NULL:deep?exps_copy(sa, i->exps):list_dup(i->exps, (fdup)NULL);
	return rel;
}

sql_rel *
rel_select_copy(sql_allocator *sa, sql_rel *l, list *exps)
{
	sql_rel *rel = rel_create(sa);
	if(!rel)
		return NULL;

	rel->l = l;
	rel->r = NULL;
	rel->op = op_select;
	rel->exps = exps?list_dup(exps, (fdup)NULL):NULL;
	rel->card = CARD_ATOM; /* no relation */
	if (l) {
		rel->card = l->card;
		rel->nrcols = l->nrcols;
	}
	return rel;
}

static int
rel_issubquery(sql_rel*r)
{
	if (!r->subquery) {
		if (is_select(r->op))
			return rel_issubquery(r->l);
	}	
	return r->subquery;
}

static sql_rel *
rel_bind_column_(mvc *sql, sql_rel **p, sql_rel *rel, const char *cname)
{
	int ambiguous = 0;
	sql_rel *l = NULL, *r = NULL;

	if (THRhighwater())
		return sql_error(sql, 10, SQLSTATE(42000) "Query too complex: running out of stack space");

	switch(rel->op) {
	case op_join:
	case op_left:
	case op_right:
	case op_full: {
		sql_rel *right = rel->r;

		*p = rel;
		r = rel_bind_column_(sql, p, rel->r, cname);

		if (!r || !rel_issubquery(right)) {
			sql_exp *e = r?exps_bind_column(r->exps, cname, &ambiguous):NULL;

			if (!r || !e || !is_freevar(e)) {
				*p = rel;
				l = rel_bind_column_(sql, p, rel->l, cname);
				if (l && r && !rel_issubquery(r) && !is_dependent(rel)) {
					(void) sql_error(sql, ERR_AMBIGUOUS, SQLSTATE(42000) "SELECT: identifier '%s' ambiguous", cname);
					return NULL;
				}
			}
		}
		if (sql->session->status == -ERR_AMBIGUOUS)
			return NULL;
		if (l && !r)
			return l;
		return r;
	}
	case op_union:
	case op_except:
	case op_inter:
	case op_groupby:
	case op_project:
	case op_table:
	case op_basetable:
		if (rel->exps && exps_bind_column(rel->exps, cname, &ambiguous))
			return rel;
		if (ambiguous) {
			(void) sql_error(sql, ERR_AMBIGUOUS, SQLSTATE(42000) "SELECT: identifier '%s' ambiguous", cname);
			return NULL;
		}
		*p = rel;
		if (is_processed(rel))
			return NULL;
		if (rel->l && !(is_base(rel->op)))
			return rel_bind_column_(sql, p, rel->l, cname);
		break;
	case op_semi:
	case op_anti:

	case op_select:
	case op_topn:
	case op_sample:
		*p = rel;
		if (rel->l)
			return rel_bind_column_(sql, p, rel->l, cname);
		/* fall through */
	default:
		return NULL;
	}
	return NULL;
}

sql_exp *
rel_bind_column( mvc *sql, sql_rel *rel, const char *cname, int f )
{
	sql_rel *p = NULL, *orel = rel;

	if (is_sql_sel(f) && rel && is_simple_project(rel->op) && !is_processed(rel))
		rel = rel->l;

	if (!rel || (rel = rel_bind_column_(sql, &p, rel, cname)) == NULL)
		return NULL;

	if ((is_project(rel->op) || is_base(rel->op)) && rel->exps) {
		sql_exp *e = exps_bind_column(rel->exps, cname, NULL);
		if (e)
			e = exp_alias_or_copy(sql, exp_relname(e), cname, rel, e);
		if (p && e && is_simple_project(p->op) && !is_processed(p) && is_sql_orderby(f) && orel != rel)
			e = rel_project_add_exp(sql, p, e);
		return e;
	}
	return NULL;
}

sql_exp *
rel_bind_column2( mvc *sql, sql_rel *rel, const char *tname, const char *cname, int f )
{
	(void)f;

	if (!rel)
		return NULL;

	if (rel->exps && (is_project(rel->op) || is_base(rel->op))) {
		sql_exp *e = exps_bind_column2(rel->exps, tname, cname);
		/* in case of orderby we should also lookup the column in group by list (and use existing references) */
		if (!e && is_sql_orderby(f) && is_groupby(rel->op) && rel->r) {
			e = exps_bind_alias(rel->r, tname, cname);
			if (e) { 
				if (exp_relname(e))
					e = exps_bind_column2(rel->exps, exp_relname(e), exp_name(e));
				else
					e = exps_bind_column(rel->exps, exp_name(e), NULL);
				if (e)
					return e;
			}
		}
		if (e)
			return exp_alias_or_copy(sql, tname, cname, rel, e);
	}
	if (is_simple_project(rel->op) && rel->l) {
		if (!is_processed(rel))
			return rel_bind_column2(sql, rel->l, tname, cname, f);
	} else if (is_join(rel->op)) {
		sql_exp *e = rel_bind_column2(sql, rel->l, tname, cname, f);
		if (!e)
			e = rel_bind_column2(sql, rel->r, tname, cname, f);
		return e;
	} else if (is_set(rel->op) ||
		   is_sort(rel) ||
		   is_semi(rel->op) ||
		   is_select(rel->op) ||
		   is_topn(rel->op)) {
		if (rel->l)
			return rel_bind_column2(sql, rel->l, tname, cname, f);
	}
	return NULL;
}

sql_rel *
rel_inplace_setop(sql_rel *rel, sql_rel *l, sql_rel *r, operator_type setop, list *exps)
{
	rel_destroy_(rel);
	rel->l = l;
	rel->r = r;
	rel->op = setop;
	rel->exps = NULL;
	rel->card = CARD_MULTI;
	rel->flag = 0;
	if (l && r)
		rel->nrcols = l->nrcols + r->nrcols;
	rel->exps = exps;
	set_processed(rel);
	return rel;
}

sql_rel *
rel_inplace_project(sql_allocator *sa, sql_rel *rel, sql_rel *l, list *e)
{
	if (!l) {
		l = rel_create(sa);
		if(!l)
			return NULL;

		l->op = rel->op;
		l->l = rel->l;
		l->r = rel->r;
		l->exps = rel->exps;
		l->nrcols = rel->nrcols;
		l->flag = rel->flag;
		l->card = rel->card;
	} else {
		rel_destroy_(rel);
	}
	set_processed(rel);

	rel->l = l;
	rel->r = NULL;
	rel->op = op_project;
	rel->exps = e;
	rel->card = CARD_MULTI;
	rel->flag = 0;
	if (l) {
		rel->nrcols = l->nrcols;
		assert (exps_card(rel->exps) <= rel->card);
	}
	return rel;
}

sql_rel *
rel_inplace_groupby(sql_rel *rel, sql_rel *l, list *groupbyexps, list *exps )
{
	rel_destroy_(rel);
	rel->card = CARD_ATOM;
	if (groupbyexps)
		rel->card = CARD_AGGR;
	rel->l = l;
	rel->r = groupbyexps;
	rel->exps = exps;
	rel->nrcols = l->nrcols;
	rel->op = op_groupby;
	rel->flag = 0;
	return rel;
}

sql_rel *
rel_setop(sql_allocator *sa, sql_rel *l, sql_rel *r, operator_type setop)
{
	sql_rel *rel = rel_create(sa);
	if(!rel)
		return NULL;

	rel->l = l;
	rel->r = r;
	rel->op = setop;
	rel->exps = NULL;
	rel->card = CARD_MULTI;
	if (l && r)
		rel->nrcols = l->nrcols + r->nrcols;
	return rel;
}

sql_rel *
rel_setop_check_types(mvc *sql, sql_rel *l, sql_rel *r, list *ls, list *rs, operator_type op)
{
	list *nls = new_exp_list(sql->sa);
	list *nrs = new_exp_list(sql->sa);
	node *n, *m;

	if(!nls || !nrs)
		return NULL;

	for (n = ls->h, m = rs->h; n && m; n = n->next, m = m->next) {
		sql_exp *le = n->data;
		sql_exp *re = m->data;

		if ((rel_convert_types(sql, l, r, &le, &re, 1, type_set) < 0))
			return NULL;
		append(nls, le);
		append(nrs, re);
	}
	l = rel_project(sql->sa, l, nls);
	r = rel_project(sql->sa, r, nrs);
	set_processed(l);
	set_processed(r);
	return rel_setop(sql->sa, l, r, op);
}

sql_rel *
rel_crossproduct(sql_allocator *sa, sql_rel *l, sql_rel *r, operator_type join)
{
	sql_rel *rel = rel_create(sa);
	if(!rel)
		return NULL;

	rel->l = l;
	rel->r = r;
	rel->op = join;
	rel->exps = NULL;
	rel->card = CARD_MULTI;
	rel->nrcols = l->nrcols + r->nrcols;
	return rel;
}

sql_exp *
rel_is_constant(sql_rel **R, sql_exp *e)
{
	sql_rel *rel = *R;

	if (rel && rel->op == op_project && list_length(rel->exps) == 1 && 
	    !rel->l && !rel->r && !rel_is_ref(rel) && e->type == e_column) {
		sql_exp *ne = rel_find_exp(rel, e);
		if (ne) {
			rel_destroy(rel);
			*R = NULL;
			return ne;
		}
	}
	return e;
}

sql_rel *
rel_topn(sql_allocator *sa, sql_rel *l, list *exps )
{
	sql_rel *rel = rel_create(sa);
	if(!rel)
		return NULL;

	rel->l = l;
	rel->r = NULL;
	rel->op = op_topn;	
	rel->exps = exps;
	rel->card = l->card;
	rel->nrcols = l->nrcols;
	return rel;
}

sql_rel *
rel_sample(sql_allocator *sa, sql_rel *l, list *exps )
{
	sql_rel *rel = rel_create(sa);
	if(!rel)
		return NULL;

	rel->l = l;
	rel->r = NULL;
	rel->op = op_sample;
	rel->exps = exps;
	rel->card = l->card;
	rel->nrcols = l->nrcols;
	return rel;
}

sql_rel *
rel_label( mvc *sql, sql_rel *r, int all)
{
	int nr = ++sql->label;
	char tname[16], *tnme;
	char cname[16], *cnme = NULL;

	tnme = number2name(tname, sizeof(tname), nr);
	if (!is_project(r->op)) {
		r = rel_project(sql->sa, r, rel_projections(sql, r, NULL, 1, 1));
		set_processed(r);
	}
	if (is_project(r->op) && r->exps) {
		node *ne = r->exps->h;

		r->exps->ht = NULL;
		for (; ne; ne = ne->next) {
			sql_exp *e = ne->data;

			if (!e->freevar) {
				if (all) {
					nr = ++sql->label;
					cnme = number2name(cname, sizeof(cname), nr);
				}
				exp_setname(sql->sa, e, tnme, cnme );
			}
		}
	}
	/* op_projects can have a order by list */
	if (r->op == op_project && r->r) {
		list *exps = r->r;
		node *ne = exps->h;

		exps->ht = NULL;
		for (; ne; ne = ne->next) {
			if (all) {
				nr = ++sql->label;
				cnme = number2name(cname, sizeof(cname), nr);
			}
			exp_setname(sql->sa, ne->data, tnme, cnme );
		}
	}
	return r;
}

sql_exp *
rel_project_add_exp( mvc *sql, sql_rel *rel, sql_exp *e)
{
	assert(is_project(rel->op));

	if (!exp_relname(e)) {
		if (exp_name(e))
			exp_setrelname(sql->sa, e, ++sql->label);
		else
			exp_label(sql->sa, e, ++sql->label);
	}
	if (rel->op == op_project) {
		sql_rel *l = rel->l;
		if (!rel->exps)
			rel->exps = new_exp_list(sql->sa);
		if (l && is_groupby(l->op) && exp_card(e) <= CARD_ATOM && list_empty(l->exps)) 
			e = rel_project_add_exp(sql, l, e);
		if (e->card > rel->card)
			rel->card = e->card;
		append(rel->exps, e);
		rel->nrcols++;
	} else if (rel->op == op_groupby) {
		return rel_groupby_add_aggr(sql, rel, e);
	}
	return e;
}

void
rel_select_add_exp(sql_allocator *sa, sql_rel *l, sql_exp *e)
{
	assert(l->op == op_select || is_outerjoin(l->op));
	if (e->type != e_cmp && e->card > CARD_ATOM) {
		sql_exp *t = exp_atom_bool(sa, 1);
		e = exp_compare(sa, e, t, cmp_equal);
	}
	if (!l->exps)
		l->exps = new_exp_list(sa);
	append(l->exps, e);
}

void
rel_join_add_exp( sql_allocator *sa, sql_rel *rel, sql_exp *e)
{
	assert(is_join(rel->op) || is_semi(rel->op) || is_select(rel->op));

	if (!rel->exps)
		rel->exps = new_exp_list(sa);
	append(rel->exps, e);
	if (e->card > rel->card)
		rel->card = e->card;
}

static sql_exp * exps_match(sql_exp *m, sql_exp *e);

static int
explists_match(list *m, list *e)
{
	node *nm,*ne;

	if (!m || !e)
		return (m==e);
	if (list_length(m) != list_length(e))
		return 0;
	for (nm = m->h, ne = e->h; nm && ne; nm = nm->next, ne = ne->next) {
		if (!exps_match(nm->data, ne->data))
			return 0;
	}
	return 1;
}

static sql_exp *
exps_match(sql_exp *m, sql_exp *e)
{
	if (m->type != e->type)
		return NULL;
	switch (m->type) {
	case e_column:
		if (strcmp(m->r, e->r) == 0) {
			if (m->l && e->l && (strcmp(m->l, e->l) == 0))
				return m;
			else if (!m->l && !e->l)
				return m;
		}
		break;
	case e_aggr:
		if (m->f == e->f && explists_match(m->l, e->l))
			return m;
		break;
	default:
		return NULL;
	}
	return NULL;
}

static sql_exp *
exps_find_match_exp(list *l, sql_exp *e)
{
	node *n;
	if (!l || !list_length(l))
		return NULL;

	for (n = l->h; n; n = n->next){
		sql_exp *m = n->data;
		if (exps_match(m,e))
			return m;
	}
	return NULL;
}

sql_exp *
rel_groupby_add_aggr(mvc *sql, sql_rel *rel, sql_exp *e)
{
	sql_exp *m = NULL, *ne;
	char name[16], *nme = NULL;

	if ((m=exps_find_match_exp(rel->exps, e)) == NULL) {
		if (!exp_name(e)) {
			nme = number2name(name, sizeof(name), ++sql->label);
			exp_setname(sql->sa, e, nme, nme);
		}
		append(rel->exps, e);
		rel->nrcols++;
		m = e;
	}
	ne = exp_column(sql->sa, exp_relname(m), exp_name(m), exp_subtype(m),
			rel->card, has_nil(m), is_intern(m));
	return ne;
}

sql_rel *
rel_select(sql_allocator *sa, sql_rel *l, sql_exp *e)
{
	sql_rel *rel;
	
	if (l && is_outerjoin(l->op) && !is_processed(l)) {
		if (e) {
			if (!l->exps)
				l->exps = new_exp_list(sa);
			append(l->exps, e);
		}
		return l;
	}
		
	if (l && l->op == op_select && !rel_is_ref(l)) { /* refine old select */
		if (e)
			rel_select_add_exp(sa, l, e);
		return l;
	}
	rel = rel_create(sa);
	if(!rel)
		return NULL;

	rel->l = l;
	rel->r = NULL;
	rel->op = op_select;
	rel->exps = new_exp_list(sa);
	if (e)
		rel_select_add_exp(sa, rel, e);
	rel->card = CARD_ATOM; /* no relation */
	if (l) {
		rel->card = l->card;
		rel->nrcols = l->nrcols;
	}
	return rel;
}

sql_rel *
rel_basetable(mvc *sql, sql_table *t, const char *atname)
{
	prop *p = NULL;
	node *cn;
	sql_allocator *sa = sql->sa;
	sql_rel *rel = rel_create(sa);
	const char *tname = t->base.name;
	if(!rel)
		return NULL;

	assert(atname);
	rel->l = t;
	rel->r = NULL;
	rel->op = op_basetable;
	rel->exps = new_exp_list(sa);
	if(!rel->exps) {
		rel_destroy(rel);
		return NULL;
	}

	if (isRemote(t))
		tname = mapiuri_table(t->query, sql->sa, tname);
	for (cn = t->columns.set->h; cn; cn = cn->next) {
		sql_column *c = cn->data;
		sql_exp *e = exp_alias(sa, atname, c->base.name, tname, c->base.name, &c->type, CARD_MULTI, c->null, 0);

		if (e == NULL) {
			rel_destroy(rel);
			return NULL;
		}
		if (c->t->pkey && ((sql_kc*)c->t->pkey->k.columns->h->data)->c == c) {
			p = e->p = prop_create(sa, PROP_HASHCOL, e->p);
			p->value = c->t->pkey;
		} else if (c->unique == 1) {
			p = e->p = prop_create(sa, PROP_HASHCOL, e->p);
			p->value = NULL;
		}
		set_basecol(e);
		append(rel->exps, e);
	}
	append(rel->exps, exp_alias(sa, atname, TID, tname, TID, sql_bind_localtype("oid"), CARD_MULTI, 0, 1));

	if (t->idxs.set) {
		for (cn = t->idxs.set->h; cn; cn = cn->next) {
			sql_exp *e;
			sql_idx *i = cn->data;
			sql_subtype *t = sql_bind_localtype("lng"); /* hash "lng" */
			char *iname = NULL;

			/* do not include empty indices in the plan */
			if (hash_index(i->type) && list_length(i->columns) <= 1)
				continue;

			if (i->type == join_idx)
				t = sql_bind_localtype("oid");

			iname = sa_strconcat( sa, "%", i->base.name);
			e = exp_alias(sa, atname, iname, tname, iname, t, CARD_MULTI, 0, 1);
			/* index names are prefixed, to make them independent */
			if (hash_index(i->type)) {
				p = e->p = prop_create(sa, PROP_HASHIDX, e->p);
				p->value = i;
			}
			if (i->type == join_idx) {
				p = e->p = prop_create(sa, PROP_JOINIDX, e->p);
				p->value = i;
			}
			append(rel->exps, e);
		}
	}

	rel->card = CARD_MULTI;
	rel->nrcols = list_length(t->columns.set);
	return rel;
}

sql_rel *
rel_groupby(mvc *sql, sql_rel *l, list *groupbyexps )
{
	sql_rel *rel = rel_create(sql->sa);
	list *aggrs = new_exp_list(sql->sa);
	node *en;
	if(!rel || !aggrs) {
		rel_destroy(rel);
		return NULL;
	}

	rel->card = CARD_ATOM;
	/* reduce duplicates in groupbyexps */
	if (groupbyexps && list_length(groupbyexps) > 1) {
		list *gexps = sa_list(sql->sa);

		for (en = groupbyexps->h; en; en = en->next) {
			sql_exp *e = en->data, *ne;

			if ((ne=exps_find_exp(gexps, e)) == NULL ||
			    strcmp(exp_relname(e),exp_relname(ne)) != 0 ||
			    strcmp(exp_name(e),exp_name(ne)) != 0  )
				append(gexps, e);
		}
		groupbyexps = gexps;
	}

	if (groupbyexps) {
		rel->card = CARD_AGGR;
		for (en = groupbyexps->h; en; en = en->next) {
			sql_exp *e = en->data, *ne;

			/* after the group by the cardinality reduces */
			e->card = rel->card;
			if (!exp_name(e))
				exp_label(sql->sa, e, ++sql->label);
			ne = exp_ref(sql->sa, e);
			ne = exp_propagate(sql->sa, ne, e);
			append(aggrs, ne);
		}
	}
	rel->l = l;
	rel->r = groupbyexps;
	rel->exps = aggrs;
	rel->nrcols = l->nrcols;
	rel->op = op_groupby;
	return rel;
}

sql_rel *
rel_project(sql_allocator *sa, sql_rel *l, list *e)
{
	sql_rel *rel = rel_create(sa);
	if(!rel)
		return NULL;

	rel->l = l;
	rel->r = NULL;
	rel->op = op_project;
	rel->exps = e;
	rel->card = exps_card(e);
	if (l) {
		rel->card = l->card;
		rel->nrcols = l->nrcols;
	}
	if (e && !list_empty(e))
		set_processed(rel);
	return rel;
}

sql_rel*
rel_project_exp(sql_allocator *sa, sql_exp *e)
{
	sql_rel *rel = rel_project(sa, NULL, append(new_exp_list(sa), e));

	return rel;
}

sql_rel *
rel_exception(sql_allocator *sa, sql_rel *l, sql_rel *r, list *exps)
{
	sql_rel *rel = rel_create(sa);
	if(!rel)
		return NULL;
	rel->l = l;
	rel->r = r;
	rel->exps = exps;
	rel->op = op_ddl;
	rel->flag = ddl_exception;
	return rel;
}

sql_rel *
rel_relational_func(sql_allocator *sa, sql_rel *l, list *exps)
{
	sql_rel *rel = rel_create(sa);
	if(!rel)
		return NULL;

	rel->flag = 1;
	rel->l = l;
	rel->op = op_table;
	rel->exps = exps;
	rel->card = CARD_MULTI;
	rel->nrcols = list_length(exps);
	return rel;
}

sql_rel *
rel_table_func(sql_allocator *sa, sql_rel *l, sql_exp *f, list *exps, int kind)
{
	sql_rel *rel = rel_create(sa);
	if(!rel)
		return NULL;

	rel->flag = kind;
	rel->l = l; /* relation before call */
	rel->r = f; /* expression (table func call) */
	rel->op = op_table;
	rel->exps = exps;
	rel->card = CARD_MULTI;
	rel->nrcols = list_length(exps);
	return rel;
}

static void
exps_has_nil(list *exps)
{
	node *m;

	for (m = exps->h; m; m = m->next) {
		sql_exp *e = m->data;

		set_has_nil(e);
	}
}

list *
_rel_projections(mvc *sql, sql_rel *rel, const char *tname, int settname, int intern, int basecol /* basecol only */ )
{
	list *lexps, *rexps, *exps;
	int include_subquery = (intern==2)?1:0;

	if (THRhighwater())
		return sql_error(sql, 10, SQLSTATE(42000) "Query too complex: running out of stack space");

	if (!rel || (!include_subquery && is_subquery(rel) && rel->op == op_project))
		return new_exp_list(sql->sa);

	switch(rel->op) {
	case op_join:
	case op_left:
	case op_right:
	case op_full:
		exps = _rel_projections(sql, rel->l, tname, settname, intern, basecol);
		if (rel->op == op_full || rel->op == op_right)
			exps_has_nil(exps);
		rexps = _rel_projections(sql, rel->r, tname, settname, intern, basecol);
		if (rel->op == op_full || rel->op == op_left)
			exps_has_nil(rexps);
		exps = list_merge( exps, rexps, (fdup)NULL);
		return exps;
	case op_groupby:
	case op_project:
	case op_basetable:
	case op_table:

	case op_union:
	case op_except:
	case op_inter:
		if (rel->exps) {
			node *en;
			int label = ++sql->label;

			exps = new_exp_list(sql->sa);
			for (en = rel->exps->h; en; en = en->next) {
				sql_exp *e = en->data;

				if (basecol && !is_basecol(e))
					continue;
				if (intern || !is_intern(e)) {
					append(exps, e = exp_alias_or_copy(sql, tname, exp_name(e), rel, e));
					if (!settname) /* noname use alias */
						exp_setrelname(sql->sa, e, label);

				}
			}
			return exps;
		}
		lexps = _rel_projections(sql, rel->l, tname, settname, intern, basecol);
		rexps = _rel_projections(sql, rel->r, tname, settname, intern, basecol);
		exps = sa_list(sql->sa);
		if (lexps && rexps && exps) {
			node *en, *ren;
			int label = ++sql->label;
			for (en = lexps->h, ren = rexps->h; en && ren; en = en->next, ren = ren->next) {
				sql_exp *e = en->data;
				e->card = rel->card;
				if (!settname) /* noname use alias */
					exp_setrelname(sql->sa, e, label);
				append(exps, e);
			}
		}
		return exps;
	case op_ddl:
	case op_semi:
	case op_anti:

	case op_select:
	case op_topn:
	case op_sample:
		return _rel_projections(sql, rel->l, tname, settname, intern, basecol);
	default:
		return NULL;
	}
}

list *
rel_projections(mvc *sql, sql_rel *rel, const char *tname, int settname, int intern)
{
	return _rel_projections(sql, rel, tname, settname, intern, 0);
}

/* find the path to the relation containing the base of the expression
	(e_column), in most cases this means go down the join tree and
	find the base column.
 */
static int
rel_bind_path_(mvc *sql, sql_rel *rel, sql_exp *e, list *path )
{
	int found = 0;

	if (THRhighwater()) {
		sql_error(sql, 10, SQLSTATE(42000) "Query too complex: running out of stack space");
		return 0;
	}

	switch (rel->op) {
	case op_join:
	case op_left:
	case op_right:
	case op_full:
		/* first right (possible subquery) */
		found = rel_bind_path_(sql, rel->r, e, path);
		if (!found)
			found = rel_bind_path_(sql, rel->l, e, path);
		break;
	case op_semi:
	case op_anti:

	case op_select:
	case op_topn:
	case op_sample:
		found = rel_bind_path_(sql, rel->l, e, path);
		break;

	case op_union:
	case op_inter:
	case op_except:
		if (!rel->exps) {
			found = rel_bind_path_(sql, rel->l, e, path);
			assert(0);
			break;
		}
		/* fall through */
	case op_groupby:
	case op_project:
	case op_table:
	case op_basetable:
		if (!rel->exps)
			break;
		if (!found && e->l && exps_bind_column2(rel->exps, e->l, e->r))
			found = 1;
		if (!found && !e->l && exps_bind_column(rel->exps, e->r, NULL))
			found = 1;
		break;
	case op_insert:
	case op_update:
	case op_delete:
	case op_truncate:
		break;
	case op_ddl:
		break;
	}
	if (found)
		list_prepend(path, rel);
	return found;
}

static list *
rel_bind_path(mvc *sql, sql_rel *rel, sql_exp *e )
{
	list *path = new_rel_list(sql->sa);
	if(!path) {
		return NULL;
	}

	if (e->type == e_convert)
		e = e->l;
	if (e->type == e_column) {
		if (rel) {
			if (!rel_bind_path_(sql, rel, e, path)) {
				/* something is wrong */
				return NULL;
			}
		}
		return path;
	}
	/* default the top relation */
	append(path, rel);
	return path;
}

/* ls is the left expression of the select, rs is a simple atom, e is the
   select expression.
 */
sql_rel *
rel_push_select(mvc *sql, sql_rel *rel, sql_exp *ls, sql_exp *e)
{
	list *l = rel_bind_path(sql, rel, ls);
	node *n;
	sql_rel *lrel = NULL, *p = NULL;

	if (!l || !sql->pushdown) {
		/* expression has no clear parent relation, so filter current
		   with it */
		return rel_select(sql->sa, rel, e);
	}

	for (n = l->h; n; n = n->next ) {
		lrel = n->data;

		if (rel_is_ref(lrel))
			break;

		/* push down as long as the operators allow this */
		if (!is_select(lrel->op) &&
		    !(is_semi(lrel->op) && !rel_is_ref(lrel->l)) &&
		    lrel->op != op_join &&
		    lrel->op != op_left)
			break;
		/* pushing through left head of a left join is allowed */
		if (lrel->op == op_left && (!n->next || lrel->l != n->next->data))
			break;
		p = lrel;
	}
	if (!lrel)
		return NULL;
	if (p && p->op == op_select && !rel_is_ref(p)) { /* refine old select */
		rel_select_add_exp(sql->sa, p, e);
	} else {
		sql_rel *n = rel_select(sql->sa, lrel, e);

		if (p && p != lrel) {
			assert(p->op == op_join || p->op == op_left || is_semi(p->op));
			if (p->l == lrel) {
				p->l = n;
			} else {
				p->r = n;
			}
		} else {
			if (rel != lrel)
				assert(0);
			rel = n;
		}
	}
	return rel;
}


/* ls and rs are the left and right expression of the join, e is the
   join expression.
 */
sql_rel *
rel_push_join(mvc *sql, sql_rel *rel, sql_exp *ls, sql_exp *rs, sql_exp *rs2, sql_exp *e)
{
	list *l = rel_bind_path(sql, rel, ls);
	list *r = rel_bind_path(sql, rel, rs);
	list *r2 = NULL;
	node *ln, *rn;
	sql_rel *lrel = NULL, *rrel = NULL, *rrel2 = NULL, *p = NULL;

	if (rs2)
		r2 = rel_bind_path(sql, rel, rs2);
	if (!l || !r || (rs2 && !r2))
		return NULL;

	if (!sql->pushdown)
		return rel_push_select(sql, rel, ls, e);

	p = rel;
	if (r2) {
		node *rn2;

		for (ln = l->h, rn = r->h, rn2 = r2->h; ln && rn && rn2; ln = ln->next, rn = rn->next, rn2 = rn2->next ) {
			lrel = ln->data;
			rrel = rn->data;
			rrel2 = rn2->data;
			
			if (rel_is_ref(lrel) || rel_is_ref(rrel) || rel_is_ref(rrel2) || is_processed(lrel) || is_processed(rrel))
				break;

			/* push down as long as the operators allow this
				and the relation is equal.
			*/
			if (lrel != rrel || lrel != rrel2 ||
				(!is_select(lrel->op) &&
				 !(is_semi(lrel->op) && !rel_is_ref(lrel->l)) &&
				 lrel->op != op_join &&
				 lrel->op != op_left))
				break;
			/* pushing through left head of a left join is allowed */
			if (lrel->op == op_left && (!ln->next || lrel->l != ln->next->data))
				break;
			p = lrel;
		}
	} else {
		for (ln = l->h, rn = r->h; ln && rn; ln = ln->next, rn = rn->next ) {
			lrel = ln->data;
			rrel = rn->data;
			
			if (rel_is_ref(lrel) || rel_is_ref(rrel) || is_processed(lrel) || is_processed(rrel))
				break;

			/* push down as long as the operators allow this
				and the relation is equal.
			*/
			if (lrel != rrel ||
				(!is_select(lrel->op) &&
				 !(is_semi(lrel->op) && !rel_is_ref(lrel->l)) &&
				 lrel->op != op_join &&
				 lrel->op != op_left))
				break;
			/* pushing through left head of a left join is allowed */
			if (lrel->op == op_left && (!ln->next || lrel->l != ln->next->data))
				break;
			p = lrel;
		}
	}
	if (!lrel || !rrel || (r2 && !rrel2))
		return NULL;

	/* filter on columns of this relation */
	if ((lrel == rrel && (!r2 || lrel == rrel2) && lrel->op != op_join) || rel_is_ref(p)) {
		if (lrel->op == op_select && !rel_is_ref(lrel)) {
			rel_select_add_exp(sql->sa, lrel, e);
		} else if (p && p->op == op_select && !rel_is_ref(p)) {
			rel_select_add_exp(sql->sa, p, e);
		} else {
			sql_rel *n = rel_select(sql->sa, lrel, e);

			if (p && p != lrel) {
				if (p->l == lrel)
					p->l = n;
				else
					p->r = n;
			} else {
				rel = n;
			}
		}
		return rel;
	}

	rel_join_add_exp( sql->sa, p, e);
	return rel;
}

sql_rel *
rel_or(mvc *sql, sql_rel *rel, sql_rel *l, sql_rel *r, list *oexps, list *lexps, list *rexps)
{
	sql_rel *ll = l->l, *rl = r->l;
	list *ls, *rs;

	assert(!lexps || l == r);
	if (l == r && lexps) { /* merge both lists */
		sql_exp *e = exp_or(sql->sa, lexps, rexps, 0);
		list *nl = oexps?oexps:new_exp_list(sql->sa);
		
		rel_destroy(r);
		append(nl, e);
		if (is_outerjoin(l->op) && is_processed(l))
			l = rel_select(sql->sa, l, NULL);
		l->exps = nl;
		return l;
	}

	/* favor or expressions over union */
	if (l->op == r->op && l->op == op_select &&
	    ll == rl && ll == rel && !rel_is_ref(l) && !rel_is_ref(r)) {
		sql_exp *e = exp_or(sql->sa, l->exps, r->exps, 0);
		list *nl = new_exp_list(sql->sa);
		
		rel_destroy(r);
		append(nl, e);
		l->exps = nl;

		/* merge and expressions */
		ll = l->l;
		while (ll && ll->op == op_select && !rel_is_ref(ll)) {
			list_merge(l->exps, ll->exps, (fdup)NULL);
			l->l = ll->l;
			ll->l = NULL;
			rel_destroy(ll);
			ll = l->l;
		}
		return l;
	}

	if (rel) {
		ls = rel_projections(sql, rel, NULL, 1, 1);
		rs = rel_projections(sql, rel, NULL, 1, 1);
	} else {
		ls = rel_projections(sql, l, NULL, 1, 1);
		rs = rel_projections(sql, r, NULL, 1, 1);
	}
	set_processed(l);
	set_processed(r);
	rel = rel_setop_check_types(sql, l, r, ls, rs, op_union);
	if (!rel)
		return NULL;
	rel->exps = rel_projections(sql, rel, NULL, 1, 1);
	set_processed(rel);
	rel = rel_distinct(rel);
	if (!rel)
		return NULL;
	if (exps_card(l->exps) <= CARD_AGGR &&
	    exps_card(r->exps) <= CARD_AGGR)
	{
		rel->card = exps_card(l->exps);
		exps_fix_card( rel->exps, rel->card);
	}
	return rel;
}

sql_table *
rel_ddl_table_get(sql_rel *r)
{
	if (r->flag == ddl_alter_table || r->flag == ddl_create_table || r->flag == ddl_create_view) {
		sql_exp *e = r->exps->t->data;
		atom *a = e->l;

		return a->data.val.pval;
	}
	return NULL;
}

static sql_exp *
exps_find_identity(list *exps, sql_rel *p)
{
	node *n;

	for (n=exps->h; n; n = n->next) {
		sql_exp *e = n->data;

		if (is_identity(e, p))
			return e;
	}
	return NULL;
}

static sql_rel *
_rel_add_identity(mvc *sql, sql_rel *rel, sql_exp **exp)
{
	list *exps = rel_projections(sql, rel, NULL, 1, 2);
	sql_exp *e;

	if (list_length(exps) == 0) {
		*exp = NULL;
		return rel;
	}
	rel = rel_project(sql->sa, rel, exps);
	e = rel->exps->h->data;
	e = exp_column(sql->sa, exp_relname(e), exp_name(e), exp_subtype(e), rel->card, has_nil(e), is_intern(e));
	e = exp_unop(sql->sa, e, sql_bind_func(sql->sa, NULL, "identity", exp_subtype(e), NULL, F_FUNC));
	set_intern(e);
	e->p = prop_create(sql->sa, PROP_HASHCOL, e->p);
	*exp = exp_label(sql->sa, e, ++sql->label);
	(void) rel_project_add_exp(sql, rel, e);
	return rel;
}

sql_rel *
rel_add_identity(mvc *sql, sql_rel *rel, sql_exp **exp)
{
	if (rel && is_project(rel->op) && (*exp = exps_find_identity(rel->exps, rel->l)) != NULL)
		return rel;
	return _rel_add_identity(sql, rel, exp);
}

sql_rel *
rel_add_identity2(mvc *sql, sql_rel *rel, sql_exp **exp)
{
	sql_rel *l = rel, *p = rel;

	if (rel && is_project(rel->op) && (*exp = exps_find_identity(rel->exps, rel->l)) != NULL)
		return rel;
	while(l && !is_set(l->op) && rel_has_freevar(sql, l) && l->l) {
		p = l;
		l = l->l;
	}
	if (l != p) {
		sql_rel *o = rel;
		sql_exp *id;

		p->l = _rel_add_identity(sql, l, exp);
		l = p->l;
		id = exp_ref(sql->sa, *exp);
		while (o && o != l) {
			*exp = id;
			if (is_project(o->op))
				rel_project_add_exp(sql, o, id);
			o = o->l;
		}
		return rel;
	}
	return _rel_add_identity(sql, rel, exp);
}

sql_exp *
rel_find_column( sql_allocator *sa, sql_rel *rel, const char *tname, const char *cname )
{
	if (!rel)
		return NULL;

	if (rel->exps && (is_project(rel->op) || is_base(rel->op))) {
		int ambiguous = 0;
		sql_exp *e = exps_bind_column2(rel->exps, tname, cname);
		if (!e && cname[0] == '%')
			e = exps_bind_column(rel->exps, cname, &ambiguous);
		if (e && !ambiguous)
			return exp_alias(sa, exp_relname(e), exp_name(e), exp_relname(e), cname, exp_subtype(e), e->card, has_nil(e), is_intern(e));
	}
	if (is_project(rel->op) && rel->l && !is_processed(rel)) {
		return rel_find_column(sa, rel->l, tname, cname);
	} else if (is_join(rel->op)) {
		sql_exp *e = rel_find_column(sa, rel->l, tname, cname);
		if (!e)
			e = rel_find_column(sa, rel->r, tname, cname);
		return e;
	} else if (is_set(rel->op) ||
		   is_sort(rel) ||
		   is_semi(rel->op) ||
		   is_select(rel->op)) {
		if (rel->l)
			return rel_find_column(sa, rel->l, tname, cname);
	}
	return NULL;
}

int
rel_in_rel(sql_rel *super, sql_rel *sub)
{
	if (!super)
		return 0;
	if (super == sub)
		return 1;
	if (is_join(super->op) || is_semi(super->op) || is_set(super->op) || is_modify(super->op) || is_ddl(super->op))
		return rel_in_rel(super->l, sub) || rel_in_rel(super->r, sub);
	if (is_select(super->op) || is_project(super->op) || is_topn(super->op) || is_sample(super->op))
		return rel_in_rel(super->l, sub);
	return 0;
}

static sql_rel *
refs_find_rel(list *refs, sql_rel *rel)
{
	node *n;

	for(n=refs->h; n; n = n->next->next) {
		sql_rel *ref = n->data;
		sql_rel *s = n->next->data;
		
		if (rel == ref) 
			return s;
	}
	return NULL;
}

static int exp_deps(mvc *sql, sql_exp *e, list *refs, list *l);

static int
exps_deps(mvc *sql, list *exps, list *refs, list *l)
{
	node *n;

	for(n = exps->h; n; n = n->next) {
		if (exp_deps(sql, n->data, refs, l) != 0)
			return -1;
	}
	return 0;
}

static int
id_cmp(int *id1, int *id2)
{
	if (*id1 == *id2)
		return 0;
	return -1;
}

static list *
cond_append(list *l, int *id)
{
	if (*id >= FUNC_OIDS && !list_find(l, id, (fcmp) &id_cmp))
		 list_append(l, id);
	return l;
}

static int rel_deps(mvc *sql, sql_rel *r, list *refs, list *l);

static int
exp_deps(mvc *sql, sql_exp *e, list *refs, list *l)
{
	if (THRhighwater()) {
		(void) sql_error(sql, 10, SQLSTATE(42000) "Query too complex: running out of stack space");
		return -1;
	}

	switch(e->type) {
	case e_psm:
		if (e->flag & PSM_SET || e->flag & PSM_RETURN) {
			return exp_deps(sql, e->l, refs, l);
		} else if (e->flag & PSM_VAR) {
			return 0;
		} else if (e->flag & PSM_WHILE || e->flag & PSM_IF) {
			if (exp_deps(sql, e->l, refs, l) != 0 ||
		            exps_deps(sql, e->r, refs, l) != 0)
				return -1;
			if (e->flag == PSM_IF && e->f)
		            return exps_deps(sql, e->r, refs, l);
		} else if (e->flag & PSM_REL) {
			sql_rel *rel = e->l;
			return rel_deps(sql, rel, refs, l);
		} else if (e->flag & PSM_EXCEPTION) {
			return exps_deps(sql, e->l, refs, l);
		}
	case e_atom: 
	case e_column: 
		break;
	case e_convert: 
		return exp_deps(sql, e->l, refs, l);
	case e_func: {
			sql_subfunc *f = e->f;

			if (e->l && exps_deps(sql, e->l, refs, l) != 0)
				return -1;
			cond_append(l, &f->func->base.id);
			if (e->l && list_length(e->l) == 2 && strcmp(f->func->base.name, "next_value_for") == 0) {
				/* add dependency on seq nr */
				list *nl = e->l;
				sql_exp *schname = nl->h->data;
				sql_exp *seqname = nl->t->data;

				char *sch_name = ((atom*)schname->l)->data.val.sval;
				char *seq_name = ((atom*)seqname->l)->data.val.sval;
				sql_schema *sche = mvc_bind_schema(sql, sch_name);
				sql_sequence *seq = find_sql_sequence(sche, seq_name);

				cond_append(l, &seq->base.id);
			}
		} break;
	case e_aggr: {
			sql_subaggr *a = e->f;

			if (e->l &&exps_deps(sql, e->l, refs, l) != 0)
				return -1;
			cond_append(l, &a->aggr->base.id);
		} break;
	case e_cmp: {
			if (get_cmp(e) == cmp_or || get_cmp(e) == cmp_filter) {
				if (get_cmp(e) == cmp_filter) {
					sql_subfunc *f = e->f;
					cond_append(l, &f->func->base.id);
				}
				if (exps_deps(sql, e->l, refs, l) != 0 ||
			    	    exps_deps(sql, e->r, refs, l) != 0)
					return -1;
			} else if (e->flag == cmp_in || e->flag == cmp_notin) {
				if (exp_deps(sql, e->l, refs, l) != 0 ||
			            exps_deps(sql, e->r, refs, l) != 0)
					return -1;
			} else {
				if (exp_deps(sql, e->l, refs, l) != 0 ||
				    exp_deps(sql, e->r, refs, l) != 0)
					return -1;
				if (e->f)
					return exp_deps(sql, e->f, refs, l);
			}
		}	break;
	}
	return 0;
}

static int
rel_deps(mvc *sql, sql_rel *r, list *refs, list *l)
{
	if (THRhighwater()) {
		(void) sql_error(sql, 10, SQLSTATE(42000) "Query too complex: running out of stack space");
		return -1;
	}

	if (!r)
		return 0;

	if (rel_is_ref(r) && refs_find_rel(refs, r)) /* allready handled */
		return 0;
	switch (r->op) {
	case op_basetable: {
		sql_table *t = r->l;
		sql_column *c = r->r;

		if (!t && c)
			t = c->t;

		cond_append(l, &t->base.id);
		/* find all used columns */
		for (node *en = r->exps->h; en; en = en->next) {
			sql_exp *exp = en->data;
			const char *oname = exp->r;

			assert(!is_func(exp->type));
			if (oname[0] == '%' && strcmp(oname, TID) == 0) {
				continue;
			} else if (oname[0] == '%') { 
				sql_idx *i = find_sql_idx(t, oname+1);
				cond_append(l, &i->base.id);
			} else {
				sql_column *c = find_sql_column(t, oname);
				cond_append(l, &c->base.id);
			}
		}
	} break;
	case op_table: {
		if ((r->flag == 0 || r->flag == 1) && r->r) { /* table producing function, excluding rel_relational_func cases */
			sql_exp *op = r->r;
			sql_subfunc *f = op->f;
			cond_append(l, &f->func->base.id);
		}
	} break;
	case op_join: 
	case op_left: 
	case op_right: 
	case op_full: 
	case op_semi:
	case op_anti:
	case op_union: 
	case op_except: 
	case op_inter: 
		if (rel_deps(sql, r->l, refs, l) != 0 ||
		    rel_deps(sql, r->r, refs, l) != 0)
			return -1;
		break;
	case op_project:
	case op_select: 
	case op_groupby: 
	case op_topn: 
	case op_sample:
		if (rel_deps(sql, r->l, refs, l) != 0)
			return -1;
		break;
	case op_insert: 
	case op_update: 
	case op_delete:
	case op_truncate:
		if (rel_deps(sql, r->l, refs, l) != 0 ||
		    rel_deps(sql, r->r, refs, l) != 0)
			return -1;
		break;
	case op_ddl:
		if (r->flag == ddl_output) {
			if (r->l)
				return rel_deps(sql, r->l, refs, l);
		} else if (r->flag == ddl_list || r->flag == ddl_exception) {
			if (r->l)
				return rel_deps(sql, r->l, refs, l);
			if (r->r)
				return rel_deps(sql, r->r, refs, l);
		} else if (r->flag == ddl_psm) {
			break;
		} else if (r->flag == ddl_create_seq || r->flag == ddl_alter_seq) {
			if (r->l)
				return rel_deps(sql, r->l, refs, l);
		}
		break;
	}
	if (!is_base(r->op) && r->exps) {
		if (exps_deps(sql, r->exps, refs, l) != 0)
			return -1;
	}
	if (is_groupby(r->op) && r->r) {
		if (exps_deps(sql, r->r, refs, l) != 0)
			return -1;
	}
	if (rel_is_ref(r)) {
		list_append(refs, r);
		list_append(refs, l);
	}
	return 0;
}

list *
rel_dependencies(mvc *sql, sql_rel *r)
{
	list *refs = sa_list(sql->sa);
	list *l = sa_list(sql->sa);

	if (rel_deps(sql, r, refs, l) != 0)
		return NULL;
	return l;
}
