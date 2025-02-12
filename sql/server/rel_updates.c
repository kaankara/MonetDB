/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0.  If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Copyright 1997 - July 2008 CWI, August 2008 - 2019 MonetDB B.V.
 */

#include "monetdb_config.h"
#include "rel_updates.h"
#include "rel_semantic.h"
#include "rel_select.h"
#include "rel_rel.h"
#include "rel_exp.h"
#include "sql_privileges.h"
#include "rel_unnest.h"
#include "rel_optimizer.h"
#include "rel_dump.h"
#include "rel_psm.h"
#include "sql_symbol.h"
#include "rel_prop.h"

static sql_exp *
insert_value(sql_query *query, sql_column *c, sql_rel **r, symbol *s, const char* action)
{
	mvc *sql = query->sql;
	if (s->token == SQL_NULL) {
		return exp_atom(sql->sa, atom_general(sql->sa, &c->type, NULL));
	} else if (s->token == SQL_DEFAULT) {
		if (c->def) {
			sql_exp *e;
			char *typestr = subtype2string2(&c->type);
			if(!typestr)
				return sql_error(sql, 02, SQLSTATE(HY001) MAL_MALLOC_FAIL);
			e = rel_parse_val(sql, sa_message(sql->sa, "select cast(%s as %s);", c->def, typestr), sql->emode, NULL);
			_DELETE(typestr);
			if (!e || (e = rel_check_type(sql, &c->type, r ? *r : NULL, e, type_equal)) == NULL)
				return sql_error(sql, 02, SQLSTATE(HY005) "%s: default expression could not be evaluated", action);
			return e;
		} else {
			return sql_error(sql, 02, SQLSTATE(42000) "%s: column '%s' has no valid default value", action, c->base.name);
		}
	} else {
		int is_last = 0;
		exp_kind ek = {type_value, card_value, FALSE};
		sql_exp *e = rel_value_exp2(query, r, s, sql_sel, ek, &is_last);

		if (!e)
			return(NULL);
		return rel_check_type(sql, &c->type, r ? *r : NULL, e, type_equal);
	}
}

static sql_exp **
insert_exp_array(mvc *sql, sql_table *t, int *Len)
{
	*Len = list_length(t->columns.set);
	return SA_ZNEW_ARRAY(sql->sa, sql_exp*, *Len);
}

#define get_basetable(rel) rel->l

static sql_table *
get_table( sql_rel *t)
{
	sql_table *tab = NULL;

	assert(is_updateble(t)); 
	if (t->op == op_basetable) { /* existing base table */
		tab = get_basetable(t);
	} else if (t->op == op_ddl &&
			   (t->flag == ddl_alter_table || t->flag == ddl_create_table || t->flag == ddl_create_view)) {
		return rel_ddl_table_get(t);
	}
	return tab;
}

static list *
get_inserts( sql_rel *ins )
{
	sql_rel *r = ins->r;

	assert(is_project(r->op) || r->op == op_table);
	return r->exps;
}

static sql_rel *
rel_insert_hash_idx(mvc *sql, const char* alias, sql_idx *i, sql_rel *inserts)
{
	char *iname = sa_strconcat( sql->sa, "%", i->base.name);
	node *m;
	sql_subtype *it, *lng;
	int bits = 1 + ((sizeof(lng)*8)-1)/(list_length(i->columns)+1);
	sql_exp *h = NULL;

	if (list_length(i->columns) <= 1 || i->type == no_idx) {
		/* dummy append */
		append(get_inserts(inserts), exp_label(sql->sa, exp_atom_lng(sql->sa, 0), ++sql->label));
		return inserts;
	}

	it = sql_bind_localtype("int");
	lng = sql_bind_localtype("lng");
	for (m = i->columns->h; m; m = m->next) {
		sql_kc *c = m->data;
		sql_exp *e = list_fetch(get_inserts(inserts), c->c->colnr);

		if (h && i->type == hash_idx)  { 
			list *exps = new_exp_list(sql->sa);
			sql_subfunc *xor = sql_bind_func_result3(sql->sa, sql->session->schema, "rotate_xor_hash", lng, it, &c->c->type, lng);

			append(exps, h);
			append(exps, exp_atom_int(sql->sa, bits));
			append(exps, e);
			h = exp_op(sql->sa, exps, xor);
		} else if (h)  { /* order preserving hash */
			sql_exp *h2;
			sql_subfunc *lsh = sql_bind_func_result(sql->sa, sql->session->schema, "left_shift", lng, it, lng);
			sql_subfunc *lor = sql_bind_func_result(sql->sa, sql->session->schema, "bit_or", lng, lng, lng);
			sql_subfunc *hf = sql_bind_func_result(sql->sa, sql->session->schema, "hash", &c->c->type, NULL, lng);

			h = exp_binop(sql->sa, h, exp_atom_int(sql->sa, bits), lsh); 
			h2 = exp_unop(sql->sa, e, hf);
			h = exp_binop(sql->sa, h, h2, lor);
		} else {
			sql_subfunc *hf = sql_bind_func_result(sql->sa, sql->session->schema, "hash", &c->c->type, NULL, lng);
			h = exp_unop(sql->sa, e, hf);
			if (i->type == oph_idx) 
				break;
		}
	}
	/* append inserts to hash */
	append(get_inserts(inserts), h);
	exp_setname(sql->sa, h, alias, iname);
	return inserts;
}

static sql_rel *
rel_insert_join_idx(mvc *sql, const char* alias, sql_idx *i, sql_rel *inserts)
{
	char *iname = sa_strconcat( sql->sa, "%", i->base.name);
	int need_nulls = 0;
	node *m, *o;
	sql_key *rk = &((sql_fkey *) i->key)->rkey->k;
	sql_rel *rt = rel_basetable(sql, rk->t, rk->t->base.name);

	sql_subtype *bt = sql_bind_localtype("bit");
	sql_subfunc *or = sql_bind_func_result(sql->sa, sql->session->schema, "or", bt, bt, bt);

	sql_rel *_nlls = NULL, *nnlls, *ins = inserts->r;
	sql_exp *lnll_exps = NULL, *rnll_exps = NULL, *e;
	list *join_exps = new_exp_list(sql->sa), *pexps;

	for (m = i->columns->h; m; m = m->next) {
		sql_kc *c = m->data;

		if (c->c->null) 
			need_nulls = 1;
	}
	/* NULL and NOT NULL, for 'SIMPLE MATCH' semantics */
	/* AND joins expressions */
	for (m = i->columns->h, o = rk->columns->h; m && o; m = m->next, o = o->next) {
		sql_kc *c = m->data;
		sql_kc *rc = o->data;
		sql_subfunc *isnil = sql_bind_func(sql->sa, sql->session->schema, "isnull", &c->c->type, NULL, F_FUNC);
		sql_exp *_is = list_fetch(ins->exps, c->c->colnr), *lnl, *rnl, *je; 
		sql_exp *rtc = exp_column(sql->sa, rel_name(rt), rc->c->base.name, &rc->c->type, CARD_MULTI, rc->c->null, 0);

		if (!exp_name(_is))
			exp_label(sql->sa, _is, ++sql->label);
		_is = exp_ref(sql->sa, _is);
		lnl = exp_unop(sql->sa, _is, isnil);
		rnl = exp_unop(sql->sa, _is, isnil);
		if (need_nulls) {
			if (lnll_exps) {
				lnll_exps = exp_binop(sql->sa, lnll_exps, lnl, or);
				rnll_exps = exp_binop(sql->sa, rnll_exps, rnl, or);
			} else {
				lnll_exps = lnl;
				rnll_exps = rnl;
			}
		}

		if (rel_convert_types(sql, rt, ins, &rtc, &_is, 1, type_equal) < 0)
			return NULL;
		je = exp_compare(sql->sa, rtc, _is, cmp_equal);
		append(join_exps, je);
	}
	if (need_nulls) {
		_nlls = rel_select( sql->sa, rel_dup(ins),
				exp_compare(sql->sa, lnll_exps, exp_atom_bool(sql->sa, 1), cmp_equal ));
		nnlls = rel_select( sql->sa, rel_dup(ins),
				exp_compare(sql->sa, rnll_exps, exp_atom_bool(sql->sa, 0), cmp_equal ));
		_nlls = rel_project(sql->sa, _nlls, rel_projections(sql, _nlls, NULL, 1, 1));
		/* add constant value for NULLS */
		e = exp_atom(sql->sa, atom_general(sql->sa, sql_bind_localtype("oid"), NULL));
		exp_setname(sql->sa, e, alias, iname);
		append(_nlls->exps, e);
	} else {
		nnlls = ins;
	}

	pexps = rel_projections(sql, nnlls, NULL, 1, 1);
	nnlls = rel_crossproduct(sql->sa, nnlls, rt, op_join);
	nnlls->exps = join_exps;
	nnlls = rel_project(sql->sa, nnlls, pexps);
	/* add row numbers */
	e = exp_column(sql->sa, rel_name(rt), TID, sql_bind_localtype("oid"), CARD_MULTI, 0, 1);
	exp_setname(sql->sa, e, alias, iname);
	append(nnlls->exps, e);

	if (need_nulls) {
		rel_destroy(ins);
		rt = inserts->r = rel_setop(sql->sa, _nlls, nnlls, op_union );
		rt->exps = rel_projections(sql, nnlls, NULL, 1, 1);
		set_processed(rt);
	} else {
		inserts->r = nnlls;
	}
	return inserts;
}

static sql_rel *
rel_insert_idxs(mvc *sql, sql_table *t, const char* alias, sql_rel *inserts)
{
	sql_rel *p = inserts->r;
	node *n;

	if (!t->idxs.set)
		return inserts;

	inserts->r = rel_label(sql, inserts->r, 1); 
	for (n = t->idxs.set->h; n; n = n->next) {
		sql_idx *i = n->data;
		sql_rel *ins = inserts->r;

		if (ins->op == op_union) 
			inserts->r = rel_project(sql->sa, ins, rel_projections(sql, ins, NULL, 0, 1));
		if (hash_index(i->type) || i->type == no_idx) {
			rel_insert_hash_idx(sql, alias, i, inserts);
		} else if (i->type == join_idx) {
			rel_insert_join_idx(sql, alias, i, inserts);
		}
	}
	if (inserts->r != p) {
		sql_rel *r = rel_create(sql->sa);
		if(!r)
			return NULL;

		r->op = op_insert;
		r->l = rel_dup(p);
		r->r = inserts;
		r->flag |= UPD_COMP; /* mark as special update */
		return r;
	}
	return inserts;
}

sql_rel *
rel_insert(mvc *sql, sql_rel *t, sql_rel *inserts)
{
	sql_rel * r = rel_create(sql->sa);
	sql_table *tab = get_table(t);
	if(!r)
		return NULL;

	r->op = op_insert;
	r->l = t;
	r->r = inserts;
	/* insert indices */
	if (tab)
		return rel_insert_idxs(sql, tab, rel_name(t), r);
	return r;
}

static sql_rel *
rel_insert_table(sql_query *query, sql_table *t, char *name, sql_rel *inserts)
{
	return rel_insert(query->sql, rel_basetable(query->sql, t, name), inserts);
}

static list *
check_table_columns(mvc *sql, sql_table *t, dlist *columns, const char *op, char *tname)
{
	list *collist;

	if (columns) {
		dnode *n;

		collist = sa_list(sql->sa);
		for (n = columns->h; n; n = n->next) {
			sql_column *c = mvc_bind_column(sql, t, n->data.sval);

			if (c) {
				list_append(collist, c);
			} else {
				return sql_error(sql, 02, SQLSTATE(42S22) "%s: no such column '%s.%s'", op, tname, n->data.sval);
			}
		}
	} else {
		collist = t->columns.set;
	}
	return collist;
}

static list *
rel_inserts(mvc *sql, sql_table *t, sql_rel *r, list *collist, size_t rowcount, int copy, const char* action)
{
	int len, i;
	sql_exp **inserts = insert_exp_array(sql, t, &len);
	list *exps = NULL;
	node *n, *m;

	if (r->exps) {
		if (!copy) {
			for (n = r->exps->h, m = collist->h; n && m; n = n->next, m = m->next) {
				sql_column *c = m->data;
				sql_exp *e = n->data;
		
				inserts[c->colnr] = rel_check_type(sql, &c->type, r, e, type_equal);
			}
		} else {
			for (m = collist->h; m; m = m->next) {
				sql_column *c = m->data;
				sql_exp *e;

				e = exps_bind_column2( r->exps, c->t->base.name, c->base.name);
				if (e)
					inserts[c->colnr] = exp_ref(sql->sa, e);
			}
		}
	}
	for (i = 0; i < len; i++) {
		if (!inserts[i]) {
			for (m = t->columns.set->h; m; m = m->next) {
				sql_column *c = m->data;

				if (c->colnr == i) {
					size_t j = 0;
					sql_exp *exps = NULL;

					for (j = 0; j < rowcount; j++) {
						sql_exp *e = NULL;

						if (c->def) {
							char *q, *typestr = subtype2string2(&c->type);
							if(!typestr)
								return sql_error(sql, 02, SQLSTATE(HY001) MAL_MALLOC_FAIL);
							q = sa_message(sql->sa, "select cast(%s as %s);", c->def, typestr);
							_DELETE(typestr);
							e = rel_parse_val(sql, q, sql->emode, NULL);
							if (!e || (e = rel_check_type(sql, &c->type, r, e, type_equal)) == NULL)
								return sql_error(sql, 02, SQLSTATE(HY005) "%s: default expression could not be evaluated", action);
						} else {
							atom *a = atom_general(sql->sa, &c->type, NULL);
							e = exp_atom(sql->sa, a);
						}
						if (!e) 
							return sql_error(sql, 02, SQLSTATE(42000) "%s: column '%s' has no valid default value", action, c->base.name);
						if (exps) {
							list *vals_list = exps->f;
			
							list_append(vals_list, e);
						}
						if (!exps && j+1 < rowcount) {
							exps = exp_values(sql->sa, sa_list(sql->sa));
							exps->tpe = c->type;
							exp_label(sql->sa, exps, ++sql->label);
						}
						if (!exps)
							exps = e;
					}
					inserts[i] = exps;
				}
			}
			assert(inserts[i]);
		}
	}
	/* now rewrite project exps in proper table order */
	exps = new_exp_list(sql->sa);
	for (i = 0; i<len; i++) 
		list_append(exps, inserts[i]);
	return exps;
}

sql_table *
insert_allowed(mvc *sql, sql_table *t, char *tname, char *op, char *opname)
{
	if (!t) {
		return sql_error(sql, 02, SQLSTATE(42S02) "%s: no such table '%s'", op, tname);
	} else if (isView(t)) {
		return sql_error(sql, 02, SQLSTATE(42000) "%s: cannot %s view '%s'", op, opname, tname);
	} else if (isNonPartitionedTable(t)) {
		return sql_error(sql, 02, SQLSTATE(42000) "%s: cannot %s merge table '%s'", op, opname, tname);
	} else if ((isRangePartitionTable(t) || isListPartitionTable(t)) && cs_size(&t->members) == 0) {
		return sql_error(sql, 02, SQLSTATE(42000) "%s: %s partitioned table '%s' has no partitions set", op, isListPartitionTable(t)?"list":"range", tname);
	} else if (isRemote(t)) {
		return sql_error(sql, 02, SQLSTATE(42000) "%s: cannot %s remote table '%s' from this server at the moment", op, opname, tname);
	} else if (isReplicaTable(t)) {
		return sql_error(sql, 02, SQLSTATE(42000) "%s: cannot %s replica table '%s'", op, opname, tname);
	} else if (isStream(t)) {
		return sql_error(sql, 02, SQLSTATE(42000) "%s: cannot %s stream '%s'", op, opname, tname);
	} else if (t->access == TABLE_READONLY) {
		return sql_error(sql, 02, SQLSTATE(42000) "%s: cannot %s read only table '%s'", op, opname, tname);
	}
	if (t && !isTempTable(t) && STORE_READONLY)
		return sql_error(sql, 02, SQLSTATE(42000) "%s: %s table '%s' not allowed in readonly mode", op, opname, tname);

	if (!table_privs(sql, t, PRIV_INSERT)) {
		return sql_error(sql, 02, SQLSTATE(42000) "%s: insufficient privileges for user '%s' to %s table '%s'", op, stack_get_string(sql, "current_user"), opname, tname);
	}
	return t;
}

static int 
copy_allowed(mvc *sql, int from)
{
	if (!global_privs(sql, (from)?PRIV_COPYFROMFILE:PRIV_COPYINTOFILE)) 
		return 0;
	return 1;
}

sql_table *
update_allowed(mvc *sql, sql_table *t, char *tname, char *op, char *opname, int is_delete)
{
	if (!t) {
		return sql_error(sql, 02, SQLSTATE(42S02) "%s: no such table '%s'", op, tname);
	} else if (isView(t)) {
		return sql_error(sql, 02, SQLSTATE(42000) "%s: cannot %s view '%s'", op, opname, tname);
	} else if (isNonPartitionedTable(t) && is_delete == 0) {
		return sql_error(sql, 02, SQLSTATE(42000) "%s: cannot %s merge table '%s'", op, opname, tname);
	} else if (isNonPartitionedTable(t) && is_delete != 0 && cs_size(&t->members) == 0) {
		return sql_error(sql, 02, SQLSTATE(42000) "%s: cannot %s merge table '%s' has no partitions set", op, opname, tname);
	} else if ((isRangePartitionTable(t) || isListPartitionTable(t)) && cs_size(&t->members) == 0) {
		return sql_error(sql, 02, SQLSTATE(42000) "%s: %s partitioned table '%s' has no partitions set", op, isListPartitionTable(t)?"list":"range", tname);
	} else if (isRemote(t)) {
		return sql_error(sql, 02, SQLSTATE(42000) "%s: cannot %s remote table '%s' from this server at the moment", op, opname, tname);
	} else if (isReplicaTable(t)) {
		return sql_error(sql, 02, SQLSTATE(42000) "%s: cannot %s replica table '%s'", op, opname, tname);
	} else if (isStream(t)) {
		return sql_error(sql, 02, SQLSTATE(42000) "%s: cannot %s stream '%s'", op, opname, tname);
	} else if (t->access == TABLE_READONLY || t->access == TABLE_APPENDONLY) {
		return sql_error(sql, 02, SQLSTATE(42000) "%s: cannot %s read or append only table '%s'", op, opname, tname);
	}
	if (t && !isTempTable(t) && STORE_READONLY)
		return sql_error(sql, 02, SQLSTATE(42000) "%s: %s table '%s' not allowed in readonly mode", op, opname, tname);
	if ((is_delete == 1 && !table_privs(sql, t, PRIV_DELETE)) || (is_delete == 2 && !table_privs(sql, t, PRIV_TRUNCATE)))
		return sql_error(sql, 02, SQLSTATE(42000) "%s: insufficient privileges for user '%s' to %s table '%s'", op, stack_get_string(sql, "current_user"), opname, tname);
	return t;
}

static sql_rel *
insert_generate_inserts(sql_query *query, sql_table *t, dlist *columns, symbol *val_or_q, const char* action)
{
	mvc *sql = query->sql;
	sql_rel *r = NULL;
	size_t rowcount = 1;
	list *collist = check_table_columns(sql, t, columns, action, t->base.name);
	if (!collist)
		return NULL;

	if (val_or_q->token == SQL_VALUES) {
		dlist *rowlist = val_or_q->data.lval;
		dlist *values;
		dnode *o;
		list *exps = new_exp_list(sql->sa);
		sql_rel *inner = NULL;

		if (!rowlist->h) {
			r = rel_project(sql->sa, NULL, NULL);
			if (!columns)
				collist = NULL;
		}

		for (o = rowlist->h; o; o = o->next, rowcount++) {
			values = o->data.lval;

			if (dlist_length(values) != list_length(collist)) {
				return sql_error(sql, 02, SQLSTATE(21S01) "%s: number of values doesn't match number of columns of table '%s'", action, t->base.name);
			} else {
				dnode *n;
				node *v, *m;

				if (o->next && list_empty(exps)) {
					for (n = values->h, m = collist->h; n && m; n = n->next, m = m->next) {
						sql_exp *vals = exp_values(sql->sa, sa_list(sql->sa));
						sql_column *c = m->data;

						vals->tpe = c->type;
						exp_label(sql->sa, vals, ++sql->label);
						list_append(exps, vals);
					}
				}
				if (!list_empty(exps)) {
					for (n = values->h, m = collist->h, v = exps->h; n && m && v; n = n->next, m = m->next, v = v->next) {
						sql_exp *vals = v->data;
						list *vals_list = vals->f;
						sql_column *c = m->data;
						sql_rel *r = NULL;
						sql_exp *ins = insert_value(query, c, &r, n->data.sym, action);
						if (!ins)
							return NULL;
						if (r && inner)
							inner = rel_crossproduct(sql->sa, inner, r, op_join);
						else if (r)
							inner = r;
						if (inner && !exp_name(ins) && !exp_is_atom(ins)) {
							exp_label(sql->sa, ins, ++sql->label);
							ins = exp_ref(sql->sa, ins);
						}
						list_append(vals_list, ins);
					}
				} else {
					/* only allow correlation in a single row of values */
					for (n = values->h, m = collist->h; n && m; n = n->next, m = m->next) {
						sql_column *c = m->data;
						sql_rel *r = NULL;
						sql_exp *ins = insert_value(query, c, &r, n->data.sym, action);
						if (!ins)
							return NULL;
						if (r && inner)
							inner = rel_crossproduct(sql->sa, inner, r, op_join);
						else if (r)
							inner = r;
						if (!exp_name(ins))
							exp_label(sql->sa, ins, ++sql->label);
						list_append(exps, ins);
					}
				}
			}
		}
		if (collist)
			r = rel_project(sql->sa, inner, exps);
	} else {
		exp_kind ek = {type_value, card_relation, TRUE};

		r = rel_subquery(query, NULL, val_or_q, ek);
	}
	if (!r)
		return NULL;

	/* In case of missing project, order by or distinct, we need to add
	   and projection */
	if (r->op != op_project || r->r || need_distinct(r))
		r = rel_project(sql->sa, r, rel_projections(sql, r, NULL, 1, 0));
	if ((r->exps && list_length(r->exps) != list_length(collist)) ||
		(!r->exps && collist))
		return sql_error(sql, 02, SQLSTATE(21S01) "%s: query result doesn't match number of columns in table '%s'", action, t->base.name);

	r->exps = rel_inserts(sql, t, r, collist, rowcount, 0, action);
	if(!r->exps)
		return NULL;
	return r;
}

static sql_rel *
merge_generate_inserts(sql_query *query, sql_table *t, sql_rel *r, dlist *columns, symbol *val_or_q)
{
	mvc *sql = query->sql;
	sql_rel *res = NULL;
	list *collist = check_table_columns(sql, t, columns, "MERGE", t->base.name);

	if (!collist)
		return NULL;

	if (val_or_q->token == SQL_VALUES) {
		list *exps = new_exp_list(sql->sa);
		dlist *rowlist = val_or_q->data.lval;

		if (!rowlist->h) {
			res = rel_project(sql->sa, NULL, NULL);
			if (!columns)
				collist = NULL;
		} else {
			node *m;
			dnode *n;
			dlist *inserts = rowlist->h->data.lval;

			if (dlist_length(rowlist) != 1)
				return sql_error(sql, 02, SQLSTATE(42000) "MERGE: number of insert rows must be exactly one in a merge statement");
			if (dlist_length(inserts) != list_length(collist))
				return sql_error(sql, 02, SQLSTATE(21S01) "MERGE: number of values doesn't match number of columns of table '%s'", t->base.name);

			for (n = inserts->h, m = collist->h; n && m; n = n->next, m = m->next) {
				sql_column *c = m->data;
				sql_exp *ins = insert_value(query, c, &r, n->data.sym, "MERGE");
				if (!ins)
					return NULL;
				if (!exp_name(ins))
					exp_label(sql->sa, ins, ++sql->label);
				list_append(exps, ins);
			}
		}
		if (collist)
			res = rel_project(sql->sa, r, exps);
	} else {
		return sql_error(sql, 02, SQLSTATE(42000) "MERGE: sub-queries not yet supported in INSERT clauses inside MERGE statements");
	}
	if (!res)
		return NULL;
	if ((res->exps && list_length(res->exps) != list_length(collist)) || (!res->exps && collist))
		return sql_error(sql, 02, SQLSTATE(21S01) "MERGE: query result doesn't match number of columns in table '%s'", t->base.name);

	res->l = r;
	res->exps = rel_inserts(sql, t, res, collist, 2, 0, "MERGE");
	if(!res->exps)
		return NULL;
	return res;
}

static sql_rel *
insert_into(sql_query *query, dlist *qname, dlist *columns, symbol *val_or_q)
{
	mvc *sql = query->sql;
	char *sname = qname_schema(qname);
	char *tname = qname_table(qname);
	sql_schema *s = NULL;
	sql_table *t = NULL;
	sql_rel *r = NULL;

	if (sname && !(s=mvc_bind_schema(sql, sname))) {
		(void) sql_error(sql, 02, SQLSTATE(3F000) "INSERT INTO: no such schema '%s'", sname);
		return NULL;
	}
	if (!s)
		s = cur_schema(sql);
	t = mvc_bind_table(sql, s, tname);
	if (!t && !sname) {
		s = tmp_schema(sql);
		t = mvc_bind_table(sql, s, tname);
		if (!t) 
			t = mvc_bind_table(sql, NULL, tname);
	}
	if (insert_allowed(sql, t, tname, "INSERT INTO", "insert into") == NULL) 
		return NULL;
	r = insert_generate_inserts(query, t, columns, val_or_q, "INSERT INTO");
	if(!r)
		return NULL;
	return rel_insert_table(query, t, t->base.name, r);
}

static int
is_idx_updated(sql_idx * i, list *exps)
{
	int update = 0;
	node *m, *n;

	for (m = i->columns->h; m; m = m->next) {
		sql_kc *ic = m->data;

		for (n = exps->h; n; n = n->next) {
			sql_exp *ce = n->data;
			sql_column *c = find_sql_column(i->t, exp_name(ce));

			if (c && ic->c->colnr == c->colnr) {
				update = 1;
				break;
			}
		}
	}
	return update;
}

static sql_rel *
rel_update_hash_idx(mvc *sql, const char* alias, sql_idx *i, sql_rel *updates)
{
	char *iname = sa_strconcat( sql->sa, "%", i->base.name);
	node *m;
	sql_subtype *it, *lng = 0; /* is not set in first if below */
	int bits = 1 + ((sizeof(lng)*8)-1)/(list_length(i->columns)+1);
	sql_exp *h = NULL;

	if (list_length(i->columns) <= 1 || i->type == no_idx) {
		h = exp_label(sql->sa, exp_atom_lng(sql->sa, 0), ++sql->label);
	} else {
		it = sql_bind_localtype("int");
		lng = sql_bind_localtype("lng");
		for (m = i->columns->h; m; m = m->next) {
			sql_kc *c = m->data;
			sql_exp *e;

			e = list_fetch(get_inserts(updates), c->c->colnr+1);

			if (h && i->type == hash_idx)  { 
				list *exps = new_exp_list(sql->sa);
				sql_subfunc *xor = sql_bind_func_result3(sql->sa, sql->session->schema, "rotate_xor_hash", lng, it, &c->c->type, lng);

				append(exps, h);
				append(exps, exp_atom_int(sql->sa, bits));
				append(exps, e);
				h = exp_op(sql->sa, exps, xor);
			} else if (h)  { /* order preserving hash */
				sql_exp *h2;
				sql_subfunc *lsh = sql_bind_func_result(sql->sa, sql->session->schema, "left_shift", lng, it, lng);
				sql_subfunc *lor = sql_bind_func_result(sql->sa, sql->session->schema, "bit_or", lng, lng, lng);
				sql_subfunc *hf = sql_bind_func_result(sql->sa, sql->session->schema, "hash", &c->c->type, NULL, lng);

				h = exp_binop(sql->sa, h, exp_atom_int(sql->sa, bits), lsh); 
				h2 = exp_unop(sql->sa, e, hf);
				h = exp_binop(sql->sa, h, h2, lor);
			} else {
				sql_subfunc *hf = sql_bind_func_result(sql->sa, sql->session->schema, "hash", &c->c->type, NULL, lng);
				h = exp_unop(sql->sa, e, hf);
				if (i->type == oph_idx) 
					break;
			}
		}
	}
	/* append hash to updates */
	append(get_inserts(updates), h);
	exp_setname(sql->sa, h, alias, iname);

	if (!updates->exps)
		updates->exps = new_exp_list(sql->sa);
	append(updates->exps, exp_column(sql->sa, alias, iname, lng, CARD_MULTI, 0, 0));
	return updates;
}

/*
         A referential constraint is satisfied if one of the following con-
         ditions is true, depending on the <match option> specified in the
         <referential constraint definition>:

         -  If no <match type> was specified then, for each row R1 of the
            referencing table, either at least one of the values of the
            referencing columns in R1 shall be a null value, or the value of
            each referencing column in R1 shall be equal to the value of the
            corresponding referenced column in some row of the referenced
            table.

         -  If MATCH FULL was specified then, for each row R1 of the refer-
            encing table, either the value of every referencing column in R1
            shall be a null value, or the value of every referencing column
            in R1 shall not be null and there shall be some row R2 of the
            referenced table such that the value of each referencing col-
            umn in R1 is equal to the value of the corresponding referenced
            column in R2.

         -  If MATCH PARTIAL was specified then, for each row R1 of the
            referencing table, there shall be some row R2 of the refer-
            enced table such that the value of each referencing column in
            R1 is either null or is equal to the value of the corresponding
            referenced column in R2.
*/
static sql_rel *
rel_update_join_idx(mvc *sql, const char* alias, sql_idx *i, sql_rel *updates)
{
	int nr = ++sql->label;
	char name[16], *nme = number2name(name, sizeof(name), nr);
	char *iname = sa_strconcat( sql->sa, "%", i->base.name);

	int need_nulls = 0;
	node *m, *o;
	sql_key *rk = &((sql_fkey *) i->key)->rkey->k;
	sql_rel *rt = rel_basetable(sql, rk->t, sa_strdup(sql->sa, nme));

	sql_subtype *bt = sql_bind_localtype("bit");
	sql_subfunc *or = sql_bind_func_result(sql->sa, sql->session->schema, "or", bt, bt, bt);

	sql_rel *_nlls = NULL, *nnlls, *ups = updates->r;
	sql_exp *lnll_exps = NULL, *rnll_exps = NULL, *e;
	list *join_exps = new_exp_list(sql->sa), *pexps;

	for (m = i->columns->h; m; m = m->next) {
		sql_kc *c = m->data;

		if (c->c->null) 
			need_nulls = 1;
	}
	for (m = i->columns->h, o = rk->columns->h; m && o; m = m->next, o = o->next) {
		sql_kc *c = m->data;
		sql_kc *rc = o->data;
		sql_subfunc *isnil = sql_bind_func(sql->sa, sql->session->schema, "isnull", &c->c->type, NULL, F_FUNC);
		sql_exp *upd = list_fetch(get_inserts(updates), c->c->colnr + 1), *lnl, *rnl, *je;
		sql_exp *rtc = exp_column(sql->sa, rel_name(rt), rc->c->base.name, &rc->c->type, CARD_MULTI, rc->c->null, 0);

		/* FOR MATCH FULL/SIMPLE/PARTIAL see above */
		/* Currently only the default MATCH SIMPLE is supported */
		upd = exp_ref(sql->sa, upd);
		lnl = exp_unop(sql->sa, upd, isnil);
		rnl = exp_unop(sql->sa, upd, isnil);
		if (need_nulls) {
			if (lnll_exps) {
				lnll_exps = exp_binop(sql->sa, lnll_exps, lnl, or);
				rnll_exps = exp_binop(sql->sa, rnll_exps, rnl, or);
			} else {
				lnll_exps = lnl;
				rnll_exps = rnl;
			}
		}
		if (rel_convert_types(sql, rt, updates, &rtc, &upd, 1, type_equal) < 0) {
			list_destroy(join_exps);
			return NULL;
		}
		je = exp_compare(sql->sa, rtc, upd, cmp_equal);
		append(join_exps, je);
	}
	if (need_nulls) {
		_nlls = rel_select( sql->sa, rel_dup(ups),
				exp_compare(sql->sa, lnll_exps, exp_atom_bool(sql->sa, 1), cmp_equal ));
		nnlls = rel_select( sql->sa, rel_dup(ups),
				exp_compare(sql->sa, rnll_exps, exp_atom_bool(sql->sa, 0), cmp_equal ));
		_nlls = rel_project(sql->sa, _nlls, rel_projections(sql, _nlls, NULL, 1, 1));
		/* add constant value for NULLS */
		e = exp_atom(sql->sa, atom_general(sql->sa, sql_bind_localtype("oid"), NULL));
		exp_setname(sql->sa, e, alias, iname);
		append(_nlls->exps, e);
	} else {
		nnlls = ups;
	}

	pexps = rel_projections(sql, nnlls, NULL, 1, 1);
	nnlls = rel_crossproduct(sql->sa, nnlls, rt, op_join);
	nnlls->exps = join_exps;
	nnlls->flag = LEFT_JOIN;
	nnlls = rel_project(sql->sa, nnlls, pexps);
	/* add row numbers */
	e = exp_column(sql->sa, rel_name(rt), TID, sql_bind_localtype("oid"), CARD_MULTI, 0, 1);
	exp_setname(sql->sa, e, alias, iname);
	append(nnlls->exps, e);

	if (need_nulls) {
		rel_destroy(ups);
		rt = updates->r = rel_setop(sql->sa, _nlls, nnlls, op_union );
		rt->exps = rel_projections(sql, nnlls, NULL, 1, 1);
		set_processed(rt);
	} else {
		updates->r = nnlls;
	}
	if (!updates->exps)
		updates->exps = new_exp_list(sql->sa);
	append(updates->exps, exp_column(sql->sa, alias, iname, sql_bind_localtype("oid"), CARD_MULTI, 0, 0));
	return updates;
}

/* for cascade of updates we change the 'relup' relations into
 * a ddl_list of update relations.
 */
static sql_rel *
rel_update_idxs(mvc *sql, const char *alias, sql_table *t, sql_rel *relup)
{
	sql_rel *p = relup->r;
	node *n;

	if (!t->idxs.set)
		return relup;

	for (n = t->idxs.set->h; n; n = n->next) {
		sql_idx *i = n->data;

		/* check if update is needed, 
		 * ie atleast on of the idx columns is updated 
		 */
		if (relup->exps && is_idx_updated(i, relup->exps) == 0) 
			continue;

		/* 
		 * relup->exps isn't set in case of alter statements!
		 * Ie todo check for new indices.
		 */

		if (hash_index(i->type) || i->type == no_idx) {
			rel_update_hash_idx(sql, alias, i, relup);
		} else if (i->type == join_idx) {
			rel_update_join_idx(sql, alias, i, relup);
		}
	}
	if (relup->r != p) {
		sql_rel *r = rel_create(sql->sa);
		if(!r)
			return NULL;
		r->op = op_update;
		r->l = rel_dup(p);
		r->r = relup;
		r->flag |= UPD_COMP; /* mark as special update */
		return r;
	}
	return relup;
}

sql_rel *
rel_update(mvc *sql, sql_rel *t, sql_rel *uprel, sql_exp **updates, list *exps)
{
	sql_rel *r = rel_create(sql->sa);
	sql_table *tab = get_table(t);
	const char *alias = rel_name(t);
	node *m;

	if (!r)
		return NULL;

	if (tab && updates)
		for (m = tab->columns.set->h; m; m = m->next) {
			sql_column *c = m->data;
			sql_exp *v = updates[c->colnr];

			if (tab->idxs.set && !v)
				v = exp_column(sql->sa, alias, c->base.name, &c->type, CARD_MULTI, c->null, 0);
			if (v)
				v = rel_project_add_exp(sql, uprel, v);
		}

	r->op = op_update;
	r->l = t;
	r->r = uprel;
	r->exps = exps;
	/* update indices */
	if (tab)
		return rel_update_idxs(sql, alias, tab, r);
	return r;
}

static sql_exp *
update_check_column(mvc *sql, sql_table *t, sql_column *c, sql_exp *v, sql_rel *r, char *cname, const char *action)
{
	if (!c) {
		rel_destroy(r);
		return sql_error(sql, 02, SQLSTATE(42S22) "%s: no such column '%s.%s'", action, t->base.name, cname);
	}
	if (!table_privs(sql, t, PRIV_UPDATE) && !sql_privilege(sql, sql->user_id, c->base.id, PRIV_UPDATE, 0)) 
		return sql_error(sql, 02, SQLSTATE(42000) "%s: insufficient privileges for user '%s' to update table '%s' on column '%s'", action, stack_get_string(sql, "current_user"), t->base.name, cname);
	if (!v || (v = rel_check_type(sql, &c->type, r, v, type_equal)) == NULL) {
		rel_destroy(r);
		return NULL;
	}
	return v;
}

static sql_rel *
update_generate_assignments(sql_query *query, sql_table *t, sql_rel *r, sql_rel *bt, dlist *assignmentlist, const char *action)
{
	mvc *sql = query->sql;
	sql_table *mt = NULL;
	sql_exp *e = NULL, **updates = SA_ZNEW_ARRAY(sql->sa, sql_exp*, list_length(t->columns.set));
	list *exps, *pcols = NULL;
	dnode *n;
	const char *rname = NULL;

	if (isPartitionedByColumnTable(t) || isPartitionedByExpressionTable(t))
		mt = t;
	else if (t->p && (isPartitionedByColumnTable(t->p) || isPartitionedByExpressionTable(t->p)))
		mt = t->p;

	if (mt && isPartitionedByColumnTable(mt)) {
		pcols = sa_list(sql->sa);
		int *nid = sa_alloc(sql->sa, sizeof(int));
		*nid = mt->part.pcol->colnr;
		list_append(pcols, nid);
	} else if (mt && isPartitionedByExpressionTable(mt)) {
		pcols = mt->part.pexp->cols;
	}
	/* first create the project */
	e = exp_column(sql->sa, rname = rel_name(r), TID, sql_bind_localtype("oid"), CARD_MULTI, 0, 1);
	exps = new_exp_list(sql->sa);
	append(exps, e);

	for (n = assignmentlist->h; n; n = n->next) {
		symbol *a = NULL;
		sql_exp *v = NULL;
		sql_rel *rel_val = NULL;
		dlist *assignment = n->data.sym->data.lval;
		int single = (assignment->h->next->type == type_string), outer = 0;
		/* Single assignments have a name, multicolumn a list */

		a = assignment->h->data.sym;
		if (a) {
			int status = sql->session->status;
			exp_kind ek = {type_value, (single)?card_column:card_relation, FALSE};

			if(single && a->token == SQL_DEFAULT) {
				char *colname = assignment->h->next->data.sval;
				sql_column *col = mvc_bind_column(sql, t, colname);
				if (col->def) {
					char *typestr = subtype2string2(&col->type);
					if(!typestr)
						return sql_error(sql, 02, SQLSTATE(HY001) MAL_MALLOC_FAIL);
					v = rel_parse_val(sql, sa_message(sql->sa, "select cast(%s as %s);", col->def, typestr), sql->emode, NULL);
					_DELETE(typestr);
				} else {
					return sql_error(sql, 02, SQLSTATE(42000) "%s: column '%s' has no valid default value", action, col->base.name);
				}
			} else if (single) {
				v = rel_value_exp(query, &rel_val, a, sql_sel, ek);
				outer = 1;
			} else {
				rel_val = rel_subquery(query, NULL, a, ek);
			}
			if ((single && !v) || (!single && !rel_val)) {
				sql->errstr[0] = 0;
				sql->session->status = status;
				assert(!rel_val);
				outer = 1;
				if (single) {
					v = rel_value_exp(query, &r, a, sql_sel, ek);
				} else if (!rel_val && r) {
					query_push_outer(query, r);
					rel_val = rel_subquery(query, NULL, a, ek);
					r = query_pop_outer(query);
					if (/* DISABLES CODE */ (0) && r) {
						list *val_exps = rel_projections(sql, r->r, NULL, 0, 1);

						r = rel_project(sql->sa, r, rel_projections(sql, r, NULL, 1, 1));
						if (r)
							list_merge(r->exps, val_exps, (fdup)NULL);
						reset_processed(r);
					}
				}
			}
			if ((single && !v) || (!single && !rel_val)) {
				rel_destroy(r);
				return NULL;
			}
			if (rel_val && outer) {
				if (single) {
					if (!exp_name(v))
						exp_label(sql->sa, v, ++sql->label);
					if (rel_val->op != op_project || is_processed(rel_val))
						rel_val = rel_project(sql->sa, rel_val, NULL);
					v = rel_project_add_exp(sql, rel_val, v);
					reset_processed(rel_val);
				}
				r = rel_crossproduct(sql->sa, r, rel_val, op_left);
				set_dependent(r);
				if (single) {
					v = exp_column(sql->sa, NULL, exp_name(v), exp_subtype(v), v->card, has_nil(v), is_intern(v));
					rel_val = NULL;
				}
			}
		}
		if (!single) {
			dlist *cols = assignment->h->next->data.lval;
			dnode *m;
			node *n;
			int nr;

			if (!rel_val)
				rel_val = r;
			if (!rel_val || !is_project(rel_val->op) ||
				dlist_length(cols) > list_length(rel_val->exps)) {
				rel_destroy(r);
				return sql_error(sql, 02, SQLSTATE(42000) "%s: too many columns specified", action);
			}
			nr = (list_length(rel_val->exps)-dlist_length(cols));
			for (n=rel_val->exps->h; nr; nr--, n = n->next)
				;
			for (m = cols->h; n && m; n = n->next, m = m->next) {
				char *cname = m->data.sval;
				sql_column *c = mvc_bind_column(sql, t, cname);
				sql_exp *v = n->data;

				if (mt && pcols) {
					for (node *nn = pcols->h; nn; nn = n->next) {
						int next = *(int*) nn->data;
						if (next == c->colnr) {
							if (isPartitionedByColumnTable(mt)) {
								return sql_error(sql, 02, SQLSTATE(42000) "%s: Update on the partitioned column is not possible at the moment", action);
							} else if (isPartitionedByExpressionTable(mt)) {
								return sql_error(sql, 02, SQLSTATE(42000) "%s: Update a column used by the partition's expression is not possible at the moment", action);
							}
						}
					}
				}
				if (!exp_name(v))
					exp_label(sql->sa, v, ++sql->label);
				if (!exp_is_atom(v) || outer)
					v = exp_ref(sql->sa, v);
				if (!v) { /* check for NULL */
					v = exp_atom(sql->sa, atom_general(sql->sa, &c->type, NULL));
				} else if ((v = update_check_column(sql, t, c, v, r, cname, action)) == NULL) {
					return NULL;
				}
				list_append(exps, exp_column(sql->sa, t->base.name, cname, &c->type, CARD_MULTI, 0, 0));
				assert(!updates[c->colnr]);
				exp_setname(sql->sa, v, c->t->base.name, c->base.name);
				updates[c->colnr] = v;
			}
		} else {
			char *cname = assignment->h->next->data.sval;
			sql_column *c = mvc_bind_column(sql, t, cname);

			if (mt && pcols) {
				for (node *nn = pcols->h; nn; nn = nn->next) {
					int next = *(int*) nn->data;
					if (next == c->colnr) {
						if (isPartitionedByColumnTable(mt)) {
							return sql_error(sql, 02, SQLSTATE(42000) "%s: Update on the partitioned column is not possible at the moment", action);
						} else if (isPartitionedByExpressionTable(mt)) {
							return sql_error(sql, 02, SQLSTATE(42000) "%s: Update a column used by the partition's expression is not possible at the moment", action);
						}
					}
				}
			}
			if (!v) {
				v = exp_atom(sql->sa, atom_general(sql->sa, &c->type, NULL));
			} else if ((v = update_check_column(sql, t, c, v, r, cname, action)) == NULL) {
				return NULL;
			}
			list_append(exps, exp_column(sql->sa, t->base.name, cname, &c->type, CARD_MULTI, 0, 0));
			exp_setname(sql->sa, v, c->t->base.name, c->base.name);
			updates[c->colnr] = v;
		}
	}
	e = exp_column(sql->sa, rname, TID, sql_bind_localtype("oid"), CARD_MULTI, 0, 1);
	r = rel_project(sql->sa, r, append(new_exp_list(sql->sa),e));
	r = rel_update(sql, bt, r, updates, exps);
	return r;
}

static sql_rel *
update_table(sql_query *query, dlist *qname, str alias, dlist *assignmentlist, symbol *opt_from, symbol *opt_where)
{
	mvc *sql = query->sql;
	char *sname = qname_schema(qname);
	char *tname = qname_table(qname);
	sql_schema *s = NULL;
	sql_table *t = NULL;

	if (sname && !(s=mvc_bind_schema(sql,sname))) {
		(void) sql_error(sql, 02, SQLSTATE(3F000) "UPDATE: no such schema '%s'", sname);
		return NULL;
	}
	if (!s)
		s = cur_schema(sql);
	t = mvc_bind_table(sql, s, tname);
	if (!t && !sname) {
		s = tmp_schema(sql);
		t = mvc_bind_table(sql, s, tname);
		if (!t) 
			t = mvc_bind_table(sql, NULL, tname);
		if (!t) 
			t = stack_find_table(sql, tname);
	}
	if (update_allowed(sql, t, tname, "UPDATE", "update", 0) != NULL) {
		sql_rel *r = NULL, *bt = rel_basetable(sql, t, t->base.name), *res = bt;

		if (alias) {
			for (node *nn = res->exps->h ; nn ; nn = nn->next)
				exp_setname(sql->sa, (sql_exp*) nn->data, alias, NULL); //the last parameter is optional, hence NULL
		}
#if 0
			dlist *selection = dlist_create(sql->sa);
			dlist *from_list = dlist_create(sql->sa);
			symbol *sym;
			sql_rel *sq;

			dlist_append_list(sql->sa, from_list, qname);
			dlist_append_symbol(sql->sa, from_list, NULL);
			sym = symbol_create_list(sql->sa, SQL_NAME, from_list);
			from_list = dlist_create(sql->sa);
			dlist_append_symbol(sql->sa, from_list, sym);

			{
				dlist *l = dlist_create(sql->sa);


				dlist_append_string(sql->sa, l, tname);
				dlist_append_string(sql->sa, l, TID);
				sym = symbol_create_list(sql->sa, SQL_COLUMN, l);

				l = dlist_create(sql->sa);
				dlist_append_symbol(sql->sa, l, sym);
				dlist_append_string(sql->sa, l, TID);
				dlist_append_symbol(sql->sa, selection, 
				  symbol_create_list(sql->sa, SQL_COLUMN, l));
			}
			for (n = assignmentlist->h; n; n = n->next) {
				dlist *assignment = n->data.sym->data.lval, *l;
				int single = (assignment->h->next->type == type_string);
				symbol *a = assignment->h->data.sym;

				l = dlist_create(sql->sa);
				dlist_append_symbol(sql->sa, l, a);
				dlist_append_string(sql->sa, l, (single)?assignment->h->next->data.sval:NULL);
				a = symbol_create_list(sql->sa, SQL_COLUMN, l);
				dlist_append_symbol(sql->sa, selection, a);
			}
		       
			sym = newSelectNode(sql->sa, 0, selection, NULL, symbol_create_list(sql->sa, SQL_FROM, from_list), opt_where, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
			sq = rel_selects(sql, sym);
			if (sq)
				sq = rel_unnest(sql, sq);
			if (sq)
				sq = rel_optimizer(sql, sq, 0);
		}
#endif

		if (opt_from) {
			dlist *fl = opt_from->data.lval;
			dnode *n = NULL;
			sql_rel *fnd = NULL;

			for (n = fl->h; n && res; n = n->next) {
				fnd = table_ref(query, NULL, n->data.sym, 0);
				if (fnd) {
					if (alias) {
						for (node *nn = fnd->exps->h ; nn ; nn = nn->next) {
							sql_exp* ee = (sql_exp*) nn->data;
							if (exp_relname(ee) && !strcmp(exp_relname(ee), alias))
								return sql_error(sql, 02, SQLSTATE(42000) "UPDATE: multiple references into table '%s'", alias);
						}
					}
					res = rel_crossproduct(sql->sa, res, fnd, op_join);
				} else
					res = fnd;
			}
			if (!res) 
				return NULL;
		}
		if (opt_where) {
			int status = sql->session->status;

			if (!table_privs(sql, t, PRIV_SELECT)) 
				return sql_error(sql, 02, SQLSTATE(42000) "UPDATE: insufficient privileges for user '%s' to update table '%s'", stack_get_string(sql, "current_user"), tname);
			r = rel_logical_exp(query, NULL, opt_where, sql_where);
			if (r) { /* simple predicate which is not using the to 
				    be updated table. We add a select all */
				//r = rel_crossproduct(sql->sa, NULL, r, op_semi);
				//r = res;
				printf("#simple select\n");
			} else {
				sql->errstr[0] = 0;
				sql->session->status = status;
				//query->outer = res;
				//r = rel_logical_exp(query, NULL, opt_where, sql_where);
				//query->outer = NULL;
				r = rel_logical_exp(query, res, opt_where, sql_where);
				if (!r)
					return NULL;
				//r = rel_crossproduct(sql->sa, res, r, op_semi);
				//set_dependent(r);
				/* handle join */
				if (!opt_from && r && is_join(r->op))
					r->op = op_semi;
				else if (r && res && r->nrcols != res->nrcols) {
					list *exps = rel_projections(sql, res, NULL, 1, 1);
					r = rel_project(sql->sa, r, exps);
				}
			}
			if (!r) 
				return NULL;
		} else {	/* update all */
			r = res;
		}
		return update_generate_assignments(query, t, r, bt, assignmentlist, "UPDATE");
	}
	return NULL;
}

sql_rel *
rel_delete(sql_allocator *sa, sql_rel *t, sql_rel *deletes)
{
	sql_rel *r = rel_create(sa);
	if(!r)
		return NULL;

	r->op = op_delete;
	r->l = t;
	r->r = deletes;
	return r;
}

sql_rel *
rel_truncate(sql_allocator *sa, sql_rel *t, int restart_sequences, int drop_action)
{
	sql_rel *r = rel_create(sa);
	list *exps = new_exp_list(sa);

	append(exps, exp_atom_int(sa, restart_sequences));
	append(exps, exp_atom_int(sa, drop_action));
	r->exps = exps;
	r->op = op_truncate;
	r->l = t;
	r->r = NULL;
	return r;
}

static sql_rel *
delete_table(sql_query *query, dlist *qname, str alias, symbol *opt_where)
{
	mvc *sql = query->sql;
	char *sname = qname_schema(qname);
	char *tname = qname_table(qname);
	sql_schema *schema = NULL;
	sql_table *t = NULL;

	if (sname && !(schema=mvc_bind_schema(sql, sname))) {
		(void) sql_error(sql, 02, SQLSTATE(3F000) "DELETE FROM: no such schema '%s'", sname);
		return NULL;
	}
	if (!schema)
		schema = cur_schema(sql);
	t = mvc_bind_table(sql, schema, tname);
	if (!t && !sname) {
		schema = tmp_schema(sql);
		t = mvc_bind_table(sql, schema, tname);
		if (!t) 
			t = mvc_bind_table(sql, NULL, tname);
		if (!t) 
			t = stack_find_table(sql, tname);
	}
	if (update_allowed(sql, t, tname, "DELETE FROM", "delete from", 1) != NULL) {
		sql_rel *r = NULL;
		sql_exp *e;

		if (opt_where) {
			int status = sql->session->status;

			if (!table_privs(sql, t, PRIV_SELECT)) 
				return sql_error(sql, 02, SQLSTATE(42000) "DELETE FROM: insufficient privileges for user '%s' to delete from table '%s'", stack_get_string(sql, "current_user"), tname);

			r = rel_logical_exp(query, NULL, opt_where, sql_where);
			if (r) { /* simple predicate which is not using the to 
					    be updated table. We add a select all */
				sql_rel *l = rel_basetable(sql, t, t->base.name );
				r = rel_crossproduct(sql->sa, l, r, op_join);
			} else {
				sql->errstr[0] = 0;
				sql->session->status = status;
				r = rel_basetable(sql, t, t->base.name );
				if (alias) {
					for (node *nn = r->exps->h ; nn ; nn = nn->next)
						exp_setname(sql->sa, (sql_exp*) nn->data, alias, NULL); //the last parameter is optional, hence NULL
				}
				r = rel_logical_exp(query, r, opt_where, sql_where);
			}
			if (!r)
				return NULL;
			e = exp_column(sql->sa, rel_name(r), TID, sql_bind_localtype("oid"), CARD_MULTI, 0, 1);
			r = rel_project(sql->sa, r, append(new_exp_list(sql->sa), e));
			r = rel_delete(sql->sa, rel_basetable(sql, t, tname), r);
		} else {	/* delete all */
			r = rel_delete(sql->sa, rel_basetable(sql, t, tname), NULL);
		}
		return r;
	}
	return NULL;
}

static sql_rel *
truncate_table(mvc *sql, dlist *qname, int restart_sequences, int drop_action)
{
	char *sname = qname_schema(qname);
	char *tname = qname_table(qname);
	sql_schema *schema = NULL;
	sql_table *t = NULL;

	if (sname && !(schema=mvc_bind_schema(sql, sname))) {
		(void) sql_error(sql, 02, SQLSTATE(3F000) "TRUNCATE: no such schema '%s'", sname);
		return NULL;
	}
	if (!schema)
		schema = cur_schema(sql);
	t = mvc_bind_table(sql, schema, tname);
	if (!t && !sname) {
		schema = tmp_schema(sql);
		t = mvc_bind_table(sql, schema, tname);
		if (!t)
			t = mvc_bind_table(sql, NULL, tname);
		if (!t)
			t = stack_find_table(sql, tname);
	}
	if (update_allowed(sql, t, tname, "TRUNCATE", "truncate", 2) != NULL)
		return rel_truncate(sql->sa, rel_basetable(sql, t, tname), restart_sequences, drop_action);
	return NULL;
}

#define MERGE_UPDATE_DELETE 1
#define MERGE_INSERT        2

extern sql_rel *rel_list(sql_allocator *sa, sql_rel *l, sql_rel *r);

static sql_rel *
validate_merge_update_delete(mvc *sql, sql_table *t, str alias, sql_rel *joined_table, tokens upd_token,
							 sql_rel *upd_del, sql_rel *bt, sql_rel *extra_selection)
{
	char buf[BUFSIZ];
	sql_exp *aggr, *bigger, *ex;
	sql_subaggr *cf = sql_bind_aggr(sql->sa, sql->session->schema, "count", NULL);
	sql_subfunc *bf;
	list *exps = new_exp_list(sql->sa);
	sql_rel *groupby, *res;
	const char *join_rel_name = rel_name(joined_table);

	assert(upd_token == SQL_UPDATE || upd_token == SQL_DELETE);

	groupby = rel_groupby(sql, rel_dup(extra_selection), NULL); //aggregate by all column and count (distinct values)
	groupby->r = rel_projections(sql, bt, NULL, 1, 0);
	aggr = exp_aggr(sql->sa, NULL, cf, 0, 0, groupby->card, 0);
	(void) rel_groupby_add_aggr(sql, groupby, aggr);
	exp_label(sql->sa, aggr, ++sql->label);

	bf = sql_bind_func(sql->sa, sql->session->schema, ">", exp_subtype(aggr), exp_subtype(aggr), F_FUNC);
	if (!bf)
		return sql_error(sql, 02, SQLSTATE(42000) "MERGE: function '>' not found");
	list_append(exps, exp_ref(sql->sa, aggr));
	list_append(exps, exp_atom_lng(sql->sa, 1));
	bigger = exp_op(sql->sa, exps, bf);
	exp_label(sql->sa, bigger, ++sql->label);
	groupby = rel_select(sql->sa, groupby, bigger); //select only columns with more than 1 value

	groupby = rel_groupby(sql, groupby, NULL);
	aggr = exp_aggr(sql->sa, NULL, cf, 0, 0, groupby->card, 0);
	(void) rel_groupby_add_aggr(sql, groupby, aggr);
	exp_label(sql->sa, aggr, ++sql->label); //count all of them, if there is at least one, throw the exception

	ex = exp_ref(sql->sa, aggr);
	snprintf(buf, BUFSIZ, "MERGE %s: Multiple rows in the input relation%s%s%s match the same row in the target %s '%s%s%s'",
			 (upd_token == SQL_DELETE) ? "DELETE" : "UPDATE",
			 join_rel_name ? " '" : "", join_rel_name ? join_rel_name : "", join_rel_name ? "'" : "",
			 alias ? "relation" : "table",
			 alias ? alias : t->s->base.name, alias ? "" : ".", alias ? "" : t->base.name);
	ex = exp_exception(sql->sa, ex, buf);

	res = rel_exception(sql->sa, groupby, NULL, list_append(new_exp_list(sql->sa), ex));
	return rel_list(sql->sa, res, upd_del);
}

static sql_rel *
merge_into_table(sql_query *query, dlist *qname, str alias, symbol *tref, symbol *search_cond, dlist *merge_list)
{
	mvc *sql = query->sql;
	char *sname = qname_schema(qname), *tname = qname_table(qname), *alias_name;
	sql_schema *s = NULL;
	sql_table *t = NULL;
	sql_rel *bt, *joined, *join_rel = NULL, *extra_project, *insert = NULL, *upd_del = NULL, *res = NULL, *extra_select;
	sql_exp *nils, *project_first;
	int processed = 0;

	assert(tref && search_cond && merge_list);

	if (sname && !(s=mvc_bind_schema(sql, sname)))
		return sql_error(sql, 02, SQLSTATE(3F000) "MERGE: no such schema '%s'", sname);
	if (!s)
		s = cur_schema(sql);
	t = mvc_bind_table(sql, s, tname);
	if (!t && !sname) {
		s = tmp_schema(sql);
		t = mvc_bind_table(sql, s, tname);
		if (!t)
			t = mvc_bind_table(sql, NULL, tname);
		if (!t)
			t = stack_find_table(sql, tname);
	}
	if (!t)
		return sql_error(sql, 02, SQLSTATE(42S02) "MERGE: no such table '%s'", tname);
	if (!table_privs(sql, t, PRIV_SELECT))
		return sql_error(sql, 02, SQLSTATE(42000) "MERGE: access denied for %s to table '%s.%s'", stack_get_string(sql, "current_user"), s->base.name, tname);
	if (isMergeTable(t))
		return sql_error(sql, 02, SQLSTATE(42000) "MERGE: merge statements not available for merge tables yet");

	bt = rel_basetable(sql, t, t->base.name);
	joined = table_ref(query, NULL, tref, 0);
	if (!bt || !joined)
		return NULL;

	if (alias) {
		for (node *nn = bt->exps->h ; nn ; nn = nn->next)
			exp_setname(sql->sa, (sql_exp*) nn->data, alias, NULL); //the last parameter is optional, hence NULL
	}
	alias_name = alias ? alias : t->base.name;
	if (rel_name(bt) && rel_name(joined) && strcmp(rel_name(bt), rel_name(joined)) == 0)
		return sql_error(sql, 02, SQLSTATE(42000) "MERGE: '%s' on both sides of the joining condition", rel_name(bt));

	for (dnode *m = merge_list->h; m; m = m->next) {
		symbol *sym = m->data.sym, *opt_search, *action;
		tokens token = sym->token;
		dlist* dl = sym->data.lval, *sts;
		opt_search = dl->h->data.sym;
		action = dl->h->next->data.sym;
		sts = action->data.lval;

		if (opt_search)
			return sql_error(sql, 02, SQLSTATE(42000) "MERGE: search condition not yet supported");

		if (token == SQL_MERGE_MATCH) {
			tokens uptdel = action->token;

			if ((processed & MERGE_UPDATE_DELETE) == MERGE_UPDATE_DELETE)
				return sql_error(sql, 02, SQLSTATE(42000) "MERGE: only one WHEN MATCHED clause is allowed");
			processed |= MERGE_UPDATE_DELETE;

			if (uptdel == SQL_UPDATE) {
				if (!update_allowed(sql, t, tname, "MERGE", "update", 0))
					return NULL;
				if ((processed & MERGE_INSERT) == MERGE_INSERT) {
					join_rel = rel_dup(join_rel);
				} else {
					join_rel = rel_crossproduct(sql->sa, joined, bt, op_left);
					if (!(join_rel = rel_logical_exp(query, join_rel, search_cond, sql_where)))
						return NULL;
					set_processed(join_rel);
				}

				//project columns of both bt and joined + oid
				extra_project = rel_project(sql->sa, join_rel, rel_projections(sql, bt, NULL, 1, 0));
				extra_project->exps = list_merge(extra_project->exps, rel_projections(sql, joined, NULL, 1, 0), (fdup)NULL);
				list_append(extra_project->exps, exp_column(sql->sa, alias_name, TID, sql_bind_localtype("oid"), CARD_MULTI, 0, 1));

				//select bt values which are not null (they had a match in the join)
				project_first = extra_project->exps->h->next->data; // this expression must come from bt!!
				project_first = exp_ref(sql->sa, project_first);
				nils = rel_unop_(query, extra_project, project_first, NULL, "isnull", card_value);
				extra_select = rel_select(sql->sa, extra_project, exp_compare(sql->sa, nils, exp_atom_bool(sql->sa, 1), cmp_notequal));

				//the update statement requires a projection on the right side
				extra_project = rel_project(sql->sa, extra_select, rel_projections(sql, bt, NULL, 1, 0));
				extra_project->exps = list_merge(extra_project->exps, rel_projections(sql, joined, NULL, 1, 0), (fdup)NULL);
				list_append(extra_project->exps,
					exp_column(sql->sa, alias_name, TID, sql_bind_localtype("oid"), CARD_MULTI, 0, 1));
				upd_del = update_generate_assignments(query, t, extra_project, rel_dup(bt), sts->h->data.lval, "MERGE");
			} else if (uptdel == SQL_DELETE) {
				if (!update_allowed(sql, t, tname, "MERGE", "delete", 1))
					return NULL;
				if ((processed & MERGE_INSERT) == MERGE_INSERT) {
					join_rel = rel_dup(join_rel);
				} else {
					join_rel = rel_crossproduct(sql->sa, joined, bt, op_left);
					if (!(join_rel = rel_logical_exp(query, join_rel, search_cond, sql_where)))
						return NULL;
					set_processed(join_rel);
				}

				//project columns of bt + oid
				extra_project = rel_project(sql->sa, join_rel, rel_projections(sql, bt, NULL, 1, 0));
				list_append(extra_project->exps, exp_column(sql->sa, alias_name, TID, sql_bind_localtype("oid"), CARD_MULTI, 0, 1));

				//select bt values which are not null (they had a match in the join)
				project_first = extra_project->exps->h->next->data; // this expression must come from bt!!
				project_first = exp_ref(sql->sa, project_first);
				nils = rel_unop_(query, extra_project, project_first, NULL, "isnull", card_value);
				extra_select = rel_select(sql->sa, extra_project, exp_compare(sql->sa, nils, exp_atom_bool(sql->sa, 1), cmp_notequal));

				//the delete statement requires a projection on the right side, which will be the oid values
				extra_project = rel_project(sql->sa, extra_select, list_append(new_exp_list(sql->sa),
					exp_column(sql->sa, alias_name, TID, sql_bind_localtype("oid"), CARD_MULTI, 0, 1)));
				upd_del = rel_delete(sql->sa, rel_dup(bt), extra_project);
			} else {
				assert(0);
			}
			if (!upd_del || !(upd_del = validate_merge_update_delete(sql, t, alias, joined, uptdel, upd_del, bt, extra_select)))
				return NULL;
		} else if (token == SQL_MERGE_NO_MATCH) {
			if ((processed & MERGE_INSERT) == MERGE_INSERT)
				return sql_error(sql, 02, SQLSTATE(42000) "MERGE: only one WHEN NOT MATCHED clause is allowed");
			processed |= MERGE_INSERT;

			assert(action->token == SQL_INSERT);
			if (!insert_allowed(sql, t, tname, "MERGE", "insert"))
				return NULL;
			if ((processed & MERGE_UPDATE_DELETE) == MERGE_UPDATE_DELETE) {
				join_rel = rel_dup(join_rel);
			} else {
				join_rel = rel_crossproduct(sql->sa, joined, bt, op_left);
				if (!(join_rel = rel_logical_exp(query, join_rel, search_cond, sql_where)))
					return NULL;
				set_processed(join_rel);
			}

			//project columns of both
			extra_project = rel_project(sql->sa, join_rel, rel_projections(sql, bt, NULL, 1, 0));
			extra_project->exps = list_merge(extra_project->exps, rel_projections(sql, joined, NULL, 1, 0), (fdup)NULL);

			//select bt values which are null (they didn't have match in the join)
			project_first = extra_project->exps->h->next->data; // this expression must come from bt!!
			project_first = exp_ref(sql->sa, project_first);
			nils = rel_unop_(query, extra_project, project_first, NULL, "isnull", card_value);
			extra_select = rel_select(sql->sa, extra_project, exp_compare(sql->sa, nils, exp_atom_bool(sql->sa, 1), cmp_equal));

			//project only values from the joined relation
			extra_project = rel_project(sql->sa, extra_select, rel_projections(sql, joined, NULL, 1, 0));
			if (!(insert = merge_generate_inserts(query, t, extra_project, sts->h->data.lval, sts->h->next->data.sym)))
				return NULL;
			if (!(insert = rel_insert(query->sql, rel_dup(bt), insert)))
				return NULL;
		} else {
			assert(0);
		}
	}

	if (processed == (MERGE_UPDATE_DELETE | MERGE_INSERT)) {
		res = rel_list(sql->sa, insert, upd_del);
		res->p = prop_create(sql->sa, PROP_DISTRIBUTE, res->p);
	} else if ((processed & MERGE_UPDATE_DELETE) == MERGE_UPDATE_DELETE) {
		res = upd_del;
		res->p = prop_create(sql->sa, PROP_DISTRIBUTE, res->p);
	} else if ((processed & MERGE_INSERT) == MERGE_INSERT) {
		res = insert;
	} else {
		assert(0);
	}
	return res;
}

static list *
table_column_types(sql_allocator *sa, sql_table *t)
{
	node *n;
	list *types = sa_list(sa);

	if (t->columns.set) for (n = t->columns.set->h; n; n = n->next) {
		sql_column *c = n->data;
		if (c->base.name[0] != '%')
			append(types, &c->type);
	}
	return types;
}

static list *
table_column_names_and_defaults(sql_allocator *sa, sql_table *t)
{
	node *n;
	list *types = sa_list(sa);

	if (t->columns.set) for (n = t->columns.set->h; n; n = n->next) {
		sql_column *c = n->data;
		append(types, &c->base.name);
		append(types, c->def);
	}
	return types;
}

static sql_rel *
rel_import(mvc *sql, sql_table *t, const char *tsep, const char *rsep, const char *ssep, const char *ns, const char *filename, lng nr, lng offset, int locked, int best_effort, dlist *fwf_widths, int onclient)
{
	sql_rel *res;
	list *exps, *args;
	node *n;
	sql_subtype tpe;
	sql_exp *import;
	sql_schema *sys = mvc_bind_schema(sql, "sys");
	sql_subfunc *f = sql_find_func(sql->sa, sys, "copyfrom", 12, F_UNION, NULL);
	char *fwf_string = NULL;
	
	if (!f) /* we do expect copyfrom to be there */
		return NULL;
	f->res = table_column_types(sql->sa, t);
 	sql_find_subtype(&tpe, "varchar", 0, 0);
	args = append( append( append( append( append( new_exp_list(sql->sa), 
		exp_atom_ptr(sql->sa, t)), 
		exp_atom_str(sql->sa, tsep, &tpe)), 
		exp_atom_str(sql->sa, rsep, &tpe)), 
		exp_atom_str(sql->sa, ssep, &tpe)), 
		exp_atom_str(sql->sa, ns, &tpe));

	if (fwf_widths && dlist_length(fwf_widths) > 0) {
		dnode *dn;
		int ncol = 0;
		char *fwf_string_cur = fwf_string = sa_alloc(sql->sa, 20 * dlist_length(fwf_widths) + 1); // a 64 bit int needs 19 characters in decimal representation plus the separator

		if (!fwf_string) 
			return NULL;
		for (dn = fwf_widths->h; dn; dn = dn->next) {
			fwf_string_cur += sprintf(fwf_string_cur, LLFMT"%c", dn->data.l_val, STREAM_FWF_FIELD_SEP);
			ncol++;
		}
		if(list_length(f->res) != ncol) {
			(void) sql_error(sql, 02, SQLSTATE(3F000) "COPY INTO: fixed width import for %d columns but %d widths given.", list_length(f->res), ncol);
			return NULL;
		}
		*fwf_string_cur = '\0';
	}

	append( args, exp_atom_str(sql->sa, filename, &tpe)); 
	import = exp_op(sql->sa,
	append(
		append(
			append(
				append(
					append(
						append( args,
							exp_atom_lng(sql->sa, nr)),
							exp_atom_lng(sql->sa, offset)),
							exp_atom_int(sql->sa, locked)),
							exp_atom_int(sql->sa, best_effort)),
							exp_atom_str(sql->sa, fwf_string, &tpe)),
							exp_atom_int(sql->sa, onclient)), f);

	exps = new_exp_list(sql->sa);
	for (n = t->columns.set->h; n; n = n->next) {
		sql_column *c = n->data;
		if (c->base.name[0] != '%')
			append(exps, exp_column(sql->sa, t->base.name, c->base.name, &c->type, CARD_MULTI, c->null, 0));
	}
	res = rel_table_func(sql->sa, NULL, import, exps, 1);
	return res;
}

static sql_rel *
copyfrom(sql_query *query, dlist *qname, dlist *columns, dlist *files, dlist *headers, dlist *seps, dlist *nr_offset, str null_string, int locked, int best_effort, int constraint, dlist *fwf_widths, int onclient)
{
	mvc *sql = query->sql;
	sql_rel *rel = NULL;
	char *sname = qname_schema(qname);
	char *tname = qname_table(qname);
	sql_schema *s = NULL;
	sql_table *t = NULL, *nt = NULL;
	const char *tsep = seps->h->data.sval;
	const char *rsep = seps->h->next->data.sval;
	const char *ssep = (seps->h->next->next)?seps->h->next->next->data.sval:NULL;
	const char *ns = (null_string)?null_string:"null";
	lng nr = (nr_offset)?nr_offset->h->data.l_val:-1;
	lng offset = (nr_offset)?nr_offset->h->next->data.l_val:0;
	list *collist;
	int reorder = 0;
	assert(!nr_offset || nr_offset->h->type == type_lng);
	assert(!nr_offset || nr_offset->h->next->type == type_lng);
	if (sname && !(s=mvc_bind_schema(sql, sname))) {
		(void) sql_error(sql, 02, SQLSTATE(3F000) "COPY INTO: no such schema '%s'", sname);
		return NULL;
	}
	if (!s)
		s = cur_schema(sql);
	t = mvc_bind_table(sql, s, tname);
	if (!t && !sname) {
		s = tmp_schema(sql);
		t = mvc_bind_table(sql, s, tname);
		if (!t)
			t = stack_find_table(sql, tname);
	}
	if (insert_allowed(sql, t, tname, "COPY INTO", "copy into") == NULL)
		return NULL;
	/* Only the MONETDB user is allowed copy into with
	   a lock and only on tables without idx */
	if (locked && !copy_allowed(sql, 1)) {
		return sql_error(sql, 02, SQLSTATE(42000) "COPY INTO: insufficient privileges: "
		    "COPY INTO from .. LOCKED requires database administrator rights");
	}
	if (locked && (!list_empty(t->idxs.set) || !list_empty(t->keys.set))) {
		return sql_error(sql, 02, SQLSTATE(42000) "COPY INTO: insufficient privileges: "
		    "COPY INTO from .. LOCKED requires tables without indices");
	}
	if (locked && has_snapshots(sql->session->tr)) {
		return sql_error(sql, 02, SQLSTATE(42000) "COPY INTO .. LOCKED: not allowed on snapshots");
	}
	if (locked && !sql->session->auto_commit) {
		return sql_error(sql, 02, SQLSTATE(42000) "COPY INTO .. LOCKED: only allowed in auto commit mode");
	}
	/* lock the store, for single user/transaction */
	if (locked) {
		if (headers)
			return sql_error(sql, 02, SQLSTATE(42000) "COPY INTO .. LOCKED: not allowed with column lists");
		store_lock();
		while (ATOMIC_GET(&store_nr_active) > 1) {
			store_unlock();
			MT_sleep_ms(100);
			store_lock();
		}
		sql->emod |= mod_locked;
		sql->caching = 0; 	/* do not cache this query */
	}

	collist = check_table_columns(sql, t, columns, "COPY INTO", tname);
	if (!collist)
		return NULL;
	/* If we have a header specification use intermediate table, for
	 * column specification other then the default list we need to reorder
	 */
	nt = t;
	if (headers || collist != t->columns.set)
		reorder = 1;
	if (headers) {
		int has_formats = 0;
		dnode *n;

		nt = mvc_create_table(sql, s, tname, tt_table, 0, SQL_DECLARED_TABLE, CA_COMMIT, -1, 0);
		for (n = headers->h; n; n = n->next) {
			dnode *dn = n->data.lval->h;
			char *cname = dn->data.sval;
			char *format = NULL;
			sql_column *cs = NULL;

			if (dn->next)
				format = dn->next->data.sval;
			if (!list_find_name(collist, cname)) {
				char *name;
				size_t len = strlen(cname) + 2;
				sql_subtype *ctype = sql_bind_localtype("oid");

				name = sa_alloc(sql->sa, len);
				snprintf(name, len, "%%cname");
				cs = mvc_create_column(sql, nt, name, ctype);
			} else if (!format) {
				cs = find_sql_column(t, cname);
				cs = mvc_create_column(sql, nt, cname, &cs->type);
			} else { /* load as string, parse later */
				sql_subtype *ctype = sql_bind_localtype("str");
				cs = mvc_create_column(sql, nt, cname, ctype);
				has_formats = 1;
			}
			(void)cs;
		}
		if (!has_formats)
			headers = NULL;
		reorder = 1;
	}
	if (files) {
		dnode *n = files->h;

		if (!onclient && !copy_allowed(sql, 1)) {
			return sql_error(sql, 02, SQLSTATE(42000)
					 "COPY INTO: insufficient privileges: "
					 "COPY INTO from file(s) requires database administrator rights, "
					 "use 'COPY INTO \"%s\" FROM file ON CLIENT' instead", tname);
		}

		for (; n; n = n->next) {
			const char *fname = n->data.sval;
			sql_rel *nrel;

			if (!onclient && fname && !MT_path_absolute(fname)) {
				char *fn = ATOMformat(TYPE_str, fname);
				sql_error(sql, 02, SQLSTATE(42000) "COPY INTO: filename must "
					  "have absolute path: %s", fn);
				GDKfree(fn);
				return NULL;
			}

			nrel = rel_import(sql, nt, tsep, rsep, ssep, ns, fname, nr, offset, locked, best_effort, fwf_widths, onclient);

			if (!rel)
				rel = nrel;
			else {
				rel = rel_setop(sql->sa, rel, nrel, op_union);
				set_processed(rel);
			}
			if (!rel)
				return rel;
		}
	} else {
		assert(onclient == 0);
		rel = rel_import(sql, nt, tsep, rsep, ssep, ns, NULL, nr, offset, locked, best_effort, NULL, onclient);
	}
	if (headers) {
		dnode *n;
		node *m = rel->exps->h;
		list *nexps = sa_list(sql->sa);

		assert(is_project(rel->op) || is_base(rel->op));
		for (n = headers->h; n; n = n->next) {
			dnode *dn = n->data.lval->h;
			char *cname = dn->data.sval;
			sql_exp *e, *ne;

			if (!list_find_name(collist, cname))
				continue;
		       	e = m->data;
			if (dn->next) {
				char *format = dn->next->data.sval;
				sql_column *cs = find_sql_column(t, cname);
				sql_schema *sys = mvc_bind_schema(sql, "sys");
				sql_subtype st;
				sql_subfunc *f;
				list *args = sa_list(sql->sa);
				size_t l = strlen(cs->type.type->sqlname);
				char *fname = sa_alloc(sql->sa, l+8);

				snprintf(fname, l+8, "str_to_%s", cs->type.type->sqlname);
				sql_find_subtype(&st, "clob", 0, 0);
				f = sql_bind_func_result(sql->sa, sys, fname, &st, &st, &cs->type);
				if (!f)
					return sql_error(sql, 02, SQLSTATE(42000) "COPY INTO: '%s' missing for type %s", fname, cs->type.type->sqlname);
				append(args, e);
				append(args, exp_atom_clob(sql->sa, format));
				ne = exp_op(sql->sa, args, f);
				exp_setname(sql->sa, ne, exp_relname(e), exp_name(e));
				append(nexps, ne);
			} else {
				append(nexps, e);
			}
			m = m->next;
		}
		rel = rel_project(sql->sa, rel, nexps);
		reorder = 0;
	}

	if (!rel)
		return rel;
	if (reorder) {
		list *exps = rel_inserts(sql, t, rel, collist, 1, 1, "COPY INTO");
		if(!exps)
			return NULL;
		rel = rel_project(sql->sa, rel, exps);
	} else {
		rel->exps = rel_inserts(sql, t, rel, collist, 1, 0, "COPY INTO");
		if(!rel->exps)
			return NULL;
	}
	rel = rel_insert_table(query, t, tname, rel);
	if (rel && locked) {
		rel->flag |= UPD_LOCKED;
		if (rel->flag & UPD_COMP)
			((sql_rel *) rel->r)->flag |= UPD_LOCKED;
	}
	if (rel && !constraint)
		rel->flag |= UPD_NO_CONSTRAINT;
	return rel;
}

static sql_rel *
bincopyfrom(sql_query *query, dlist *qname, dlist *columns, dlist *files, int constraint, int onclient)
{
	mvc *sql = query->sql;
	char *sname = qname_schema(qname);
	char *tname = qname_table(qname);
	sql_schema *s = NULL;
	sql_table *t = NULL;

	dnode *dn;
	node *n;
	sql_rel *res;
	list *exps, *args;
	sql_subtype strtpe;
	sql_exp *import;
	sql_schema *sys = mvc_bind_schema(sql, "sys");
	sql_subfunc *f = sql_find_func(sql->sa, sys, "copyfrom", 3, F_UNION, NULL); 
	list *collist;
	int i;

	assert(f);
	if (!copy_allowed(sql, 1)) {
		(void) sql_error(sql, 02, SQLSTATE(42000) "COPY INTO: insufficient privileges: "
				"binary COPY INTO requires database administrator rights");
		return NULL;
	}

	if (sname && !(s=mvc_bind_schema(sql, sname))) {
		(void) sql_error(sql, 02, SQLSTATE(3F000) "COPY INTO: no such schema '%s'", sname);
		return NULL;
	}
	if (!s)
		s = cur_schema(sql);
	t = mvc_bind_table(sql, s, tname);
	if (!t && !sname) {
		s = tmp_schema(sql);
		t = mvc_bind_table(sql, s, tname);
		if (!t) 
			t = stack_find_table(sql, tname);
	}
	if (insert_allowed(sql, t, tname, "COPY INTO", "copy into") == NULL) 
		return NULL;
	if (files == NULL)
		return sql_error(sql, 02, SQLSTATE(42000) "COPY INTO: must specify files");

	collist = check_table_columns(sql, t, columns, "COPY BINARY INTO", tname);
	if (!collist)
		return NULL;

	f->res = table_column_types(sql->sa, t);
 	sql_find_subtype(&strtpe, "varchar", 0, 0);
	args = append( append( append( new_exp_list(sql->sa),
		exp_atom_str(sql->sa, t->s?t->s->base.name:NULL, &strtpe)), 
		exp_atom_str(sql->sa, t->base.name, &strtpe)),
		exp_atom_int(sql->sa, onclient));

	// create the list of files that is passed to the function as parameter
	for (i = 0; i < list_length(t->columns.set); i++) {
		// we have one file per column, however, because we have column selection that file might be NULL
		// first, check if this column number is present in the passed in the parameters
		int found = 0;
		dn = files->h;
		for (n = collist->h; n && dn; n = n->next, dn = dn->next) {
			sql_column *c = n->data;
			if (i == c->colnr) {
				// this column number was present in the input arguments; pass in the file name
				append(args, exp_atom_str(sql->sa, dn->data.sval, &strtpe)); 
				found = 1;
				break;
			}
		}
		if (!found) {
			// this column was not present in the input arguments; pass in NULL
			append(args, exp_atom_str(sql->sa, NULL, &strtpe)); 
		}
	}

	import = exp_op(sql->sa,  args, f); 

	exps = new_exp_list(sql->sa);
	for (n = t->columns.set->h; n; n = n->next) {
		sql_column *c = n->data;
		append(exps, exp_column(sql->sa, t->base.name, c->base.name, &c->type, CARD_MULTI, c->null, 0));
	}
	res = rel_table_func(sql->sa, NULL, import, exps, 1);
	res = rel_insert_table(query, t, t->base.name, res);
	if (res && !constraint)
		res->flag |= UPD_NO_CONSTRAINT;
	return res;
}

static sql_rel *
copyfromloader(sql_query *query, dlist *qname, symbol *fcall)
{
	mvc *sql = query->sql;
	sql_schema *s = NULL;
	char *sname = qname_schema(qname);
	char *tname = qname_table(qname);
	sql_subfunc *loader = NULL;
	sql_rel* rel = NULL;
	sql_table* t;

	if (!copy_allowed(sql, 1)) {
		(void) sql_error(sql, 02, SQLSTATE(42000) "COPY INTO: insufficient privileges: "
				"binary COPY INTO requires database administrator rights");
		return NULL;
	}
	if (sname && !(s = mvc_bind_schema(sql, sname))) {
		(void) sql_error(sql, 02, SQLSTATE(3F000) "COPY INTO: no such schema '%s'", sname);
		return NULL;
	}
	if (!s)
		s = cur_schema(sql);
	t = mvc_bind_table(sql, s, tname);
	if (!t && !sname) {
		s = tmp_schema(sql);
		t = mvc_bind_table(sql, s, tname);
		if (!t)
			t = stack_find_table(sql, tname);
	}
	//TODO the COPY LOADER INTO should return an insert relation (instead of ddl) to handle partitioned tables properly
	if (insert_allowed(sql, t, tname, "COPY INTO", "copy into") == NULL) {
		return NULL;
	} else if (isPartitionedByColumnTable(t) || isPartitionedByExpressionTable(t)) {
		(void) sql_error(sql, 02, SQLSTATE(3F000) "COPY LOADER INTO: not possible for partitioned tables at the moment");
		return NULL;
	} else if (t->p && (isPartitionedByColumnTable(t->p) || isPartitionedByExpressionTable(t->p))) {
		(void) sql_error(sql, 02, SQLSTATE(3F000) "COPY LOADER INTO: not possible for tables child of partitioned tables at the moment");
		return NULL;
	}

	rel = rel_loader_function(query, fcall, new_exp_list(sql->sa), &loader);
	if (!rel || !loader) {
		return NULL;
	}

	loader->sname = sname ? sa_zalloc(sql->sa, strlen(sname) + 1) : NULL;
	loader->tname = tname ? sa_zalloc(sql->sa, strlen(tname) + 1) : NULL;
	loader->coltypes = table_column_types(sql->sa, t);
	loader->colnames = table_column_names_and_defaults(sql->sa, t);

	if (sname) strcpy(loader->sname, sname);
	if (tname) strcpy(loader->tname, tname);

	return rel;
}

static sql_rel *
rel_output(mvc *sql, sql_rel *l, sql_exp *sep, sql_exp *rsep, sql_exp *ssep, sql_exp *null_string, sql_exp *file, sql_exp *onclient) 
{
	sql_rel *rel = rel_create(sql->sa);
	list *exps = new_exp_list(sql->sa);
	if(!rel || !exps)
		return NULL;

	append(exps, sep);
	append(exps, rsep);
	append(exps, ssep);
	append(exps, null_string);
	if (file) {
		append(exps, file);
		append(exps, onclient);
	}
	rel->l = l;
	rel->r = NULL;
	rel->op = op_ddl;
	rel->flag = ddl_output;
	rel->exps = exps;
	rel->card = 0;
	rel->nrcols = 0;
	return rel;
}

static sql_rel *
copyto(sql_query *query, symbol *sq, const char *filename, dlist *seps, const char *null_string, int onclient)
{
	mvc *sql = query->sql;
	const char *tsep = seps->h->data.sval;
	const char *rsep = seps->h->next->data.sval;
	const char *ssep = (seps->h->next->next)?seps->h->next->next->data.sval:"\"";
	const char *ns = (null_string)?null_string:"null";
	sql_exp *tsep_e, *rsep_e, *ssep_e, *ns_e, *fname_e, *oncl_e;
	exp_kind ek = {type_value, card_relation, TRUE};
	sql_rel *r = rel_subquery(query, NULL, sq, ek);

	if (!r)
		return NULL;

	tsep_e = exp_atom_clob(sql->sa, tsep);
	rsep_e = exp_atom_clob(sql->sa, rsep);
	ssep_e = exp_atom_clob(sql->sa, ssep);
	ns_e = exp_atom_clob(sql->sa, ns);
	oncl_e = exp_atom_int(sql->sa, onclient);
	fname_e = filename?exp_atom_clob(sql->sa, filename):NULL;

	if (!onclient && filename) {
		struct stat fs;
		if (!copy_allowed(sql, 0))
			return sql_error(sql, 02, SQLSTATE(42000) "COPY INTO: insufficient privileges: "
					 "COPY INTO file requires database administrator rights, "
					 "use 'COPY ... INTO file ON CLIENT' instead");
		if (filename && !MT_path_absolute(filename))
			return sql_error(sql, 02, SQLSTATE(42000) "COPY INTO ON SERVER: filename must "
					 "have absolute path: %s", filename);
		if (lstat(filename, &fs) == 0)
			return sql_error(sql, 02, SQLSTATE(42000) "COPY INTO ON SERVER: file already "
					 "exists: %s", filename);
	}

	return rel_output(sql, r, tsep_e, rsep_e, ssep_e, ns_e, fname_e, oncl_e);
}

sql_exp *
rel_parse_val(mvc *m, char *query, char emode, sql_rel *from)
{
	mvc o = *m;
	sql_exp *e = NULL;
	buffer *b;
	char *n;
	size_t len = _strlen(query);
	exp_kind ek = {type_value, card_value, FALSE};
	stream *s;
	bstream *bs;

	m->qc = NULL;

	m->caching = 0;
	m->emode = emode;
	b = (buffer*)GDKmalloc(sizeof(buffer));
	n = GDKmalloc(len + 1 + 1);
	if(!b || !n) {
		GDKfree(b);
		GDKfree(n);
		return NULL;
	}
	snprintf(n, len + 2, "%s\n", query);
	query = n;
	len++;
	buffer_init(b, query, len);
	s = buffer_rastream(b, "sqlstatement");
	if(!s) {
		buffer_destroy(b);
		return NULL;
	}
	bs = bstream_create(s, b->len);
	if(bs == NULL) {
		buffer_destroy(b);
		return NULL;
	}
	scanner_init(&m->scanner, bs, NULL);
	m->scanner.mode = LINE_1; 
	bstream_next(m->scanner.rs);

	m->params = NULL;
	/*m->args = NULL;*/
	m->argc = 0;
	m->sym = NULL;
	m->errstr[0] = '\0';
	/* via views we give access to protected objects */
	m->user_id = USER_MONETDB;

	(void) sqlparse(m);	
	
	/* get out the single value as we don't want an enclosing projection! */
	if (m->sym && m->sym->token == SQL_SELECT) {
		SelectNode *sn = (SelectNode *)m->sym;
		if (sn->selection->h->data.sym->token == SQL_COLUMN || sn->selection->h->data.sym->token == SQL_IDENT) {
			int is_last = 0;
			sql_rel *r = from;
			symbol* sq = sn->selection->h->data.sym->data.lval->h->data.sym;
			sql_query *query = query_create(m);
			e = rel_value_exp2(query, &r, sq, sql_sel, ek, &is_last);
		}
	}
	GDKfree(query);
	GDKfree(b);
	bstream_destroy(m->scanner.rs);

	m->sym = NULL;
	o.vars = m->vars;	/* may have been realloc'ed */
	o.sizevars = m->sizevars;
	if (m->session->status || m->errstr[0]) {
		int status = m->session->status;
		char errstr[ERRSIZE];

		strcpy(errstr, m->errstr);
		*m = o;
		m->session->status = status;
		strcpy(m->errstr, errstr);
	} else {
		int label = m->label;
		*m = o;

		m->label = label;
	}
	return e;
}

sql_rel *
rel_updates(sql_query *query, symbol *s)
{
	mvc *sql = query->sql;
	sql_rel *ret = NULL;
	int old = sql->use_views;

	sql->use_views = 1;
	switch (s->token) {
	case SQL_COPYFROM:
	{
		dlist *l = s->data.lval;

		ret = copyfrom(query,
				l->h->data.lval, 
				l->h->next->data.lval, 
				l->h->next->next->data.lval, 
				l->h->next->next->next->data.lval, 
				l->h->next->next->next->next->data.lval, 
				l->h->next->next->next->next->next->data.lval, 
				l->h->next->next->next->next->next->next->data.sval, 
				l->h->next->next->next->next->next->next->next->data.i_val, 
				l->h->next->next->next->next->next->next->next->next->data.i_val, 
				l->h->next->next->next->next->next->next->next->next->next->data.i_val,
				l->h->next->next->next->next->next->next->next->next->next->next->data.lval, 
				l->h->next->next->next->next->next->next->next->next->next->next->next->data.i_val);
		sql->type = Q_UPDATE;
	}
		break;
	case SQL_BINCOPYFROM:
	{
		dlist *l = s->data.lval;

		ret = bincopyfrom(query, l->h->data.lval, l->h->next->data.lval, l->h->next->next->data.lval, l->h->next->next->next->data.i_val, l->h->next->next->next->next->data.i_val);
		sql->type = Q_UPDATE;
	}
		break;
	case SQL_COPYLOADER:
	{
	    dlist *l = s->data.lval;
	    dlist *qname = l->h->data.lval;
	    symbol *sym = l->h->next->data.sym;

	    ret = rel_psm_stmt(sql->sa, exp_rel(sql, copyfromloader(query, qname, sym)));
	    sql->type = Q_SCHEMA;
	}
		break;
	case SQL_COPYTO:
	{
		dlist *l = s->data.lval;

		ret = copyto(query, l->h->data.sym, l->h->next->data.sval, l->h->next->next->data.lval, l->h->next->next->next->data.sval, l->h->next->next->next->next->data.i_val);
		sql->type = Q_UPDATE;
	}
		break;
	case SQL_INSERT:
	{
		dlist *l = s->data.lval;

		ret = insert_into(query, l->h->data.lval, l->h->next->data.lval, l->h->next->next->data.sym);
		sql->type = Q_UPDATE;
	}
		break;
	case SQL_UPDATE:
	{
		dlist *l = s->data.lval;

		ret = update_table(query, l->h->data.lval, l->h->next->data.sval, l->h->next->next->data.lval,
						   l->h->next->next->next->data.sym, l->h->next->next->next->next->data.sym);
		sql->type = Q_UPDATE;
	}
		break;
	case SQL_DELETE:
	{
		dlist *l = s->data.lval;

		ret = delete_table(query, l->h->data.lval, l->h->next->data.sval, l->h->next->next->data.sym);
		sql->type = Q_UPDATE;
	}
		break;
	case SQL_TRUNCATE:
	{
		dlist *l = s->data.lval;

		int restart_sequences = l->h->next->data.i_val;
		int drop_action = l->h->next->next->data.i_val;
		ret = truncate_table(sql, l->h->data.lval, restart_sequences, drop_action);
		sql->type = Q_UPDATE;
	}
		break;
	case SQL_MERGE:
	{
		dlist *l = s->data.lval;

		ret = merge_into_table(query, l->h->data.lval, l->h->next->data.sval, l->h->next->next->data.sym,
							   l->h->next->next->next->data.sym, l->h->next->next->next->next->data.lval);
		sql->type = Q_UPDATE;
	} break;
	default:
		sql->use_views = old;
		return sql_error(sql, 01, SQLSTATE(42000) "Updates statement unknown Symbol(%p)->token = %s", s, token2string(s->token));
	}
	sql->use_views = old;
	return ret;
}
