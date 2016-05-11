/* ------------------------------------------------------------------------
 *
 * hooks.c
 *		definitions of rel_pathlist and join_pathlist hooks
 *
 * Copyright (c) 2016, Postgres Professional
 *
 * ------------------------------------------------------------------------
 */
#include "postgres.h"
#include "optimizer/cost.h"
#include "optimizer/restrictinfo.h"
#include "utils/guc.h"
#include "hooks.h"
#include "pathman.h"
#include "runtimeappend.h"
#include "runtime_merge_append.h"


static int cmp_tlist_vars(const void *a, const void *b);
static List * sort_rel_tlist(List *tlist);

set_join_pathlist_hook_type		set_join_pathlist_next = NULL;
set_rel_pathlist_hook_type		set_rel_pathlist_hook_next = NULL;

/* Take care of joins */
void
pathman_join_pathlist_hook(PlannerInfo *root,
						   RelOptInfo *joinrel,
						   RelOptInfo *outerrel,
						   RelOptInfo *innerrel,
						   JoinType jointype,
						   JoinPathExtraData *extra)
{
	JoinCostWorkspace	workspace;
	Path			   *outer,
					   *inner;
	Relids				inner_required;
	RangeTblEntry	   *inner_entry = root->simple_rte_array[innerrel->relid];
	PartRelationInfo   *inner_prel;
	NestPath		   *nest_path;
	List			   *pathkeys = NIL;
	List			   *joinrestrictclauses = extra->restrictlist;
	List			   *joinclauses,
					   *otherclauses;
	ListCell		   *lc;
	double				paramsel;
	WalkerContext		context;

	if (set_join_pathlist_next)
		set_join_pathlist_next(root, joinrel, outerrel,
							   innerrel, jointype, extra);

	if (jointype == JOIN_UNIQUE_OUTER ||
		jointype == JOIN_UNIQUE_INNER)
	{
		jointype = JOIN_INNER;
	}

	if (jointype == JOIN_FULL || !pg_pathman_enable_runtimeappend)
		return;

	if (innerrel->reloptkind != RELOPT_BASEREL ||
		!inner_entry->inh ||
		!(inner_prel = get_pathman_relation_info(inner_entry->relid, NULL)))
	{
		return; /* Obviously not our case */
	}

	/* Extract join clauses which will separate partitions */
	if (IS_OUTER_JOIN(extra->sjinfo->jointype))
	{
		extract_actual_join_clauses(joinrestrictclauses,
									&joinclauses, &otherclauses);
	}
	else
	{
		/* We can treat all clauses alike for an inner join */
		joinclauses = extract_actual_clauses(joinrestrictclauses, false);
		otherclauses = NIL;
	}

	paramsel = 1.0;
	foreach (lc, joinclauses)
	{
		WrapperNode	   *wrap;

		context.prel = inner_prel;
		context.econtext = NULL;
		context.hasLeast = false;
		context.hasGreatest = false;

		wrap = walk_expr_tree((Expr *) lfirst(lc), &context);
		paramsel *= wrap->paramsel;
	}

	foreach (lc, innerrel->pathlist)
	{
		AppendPath *cur_inner_path = (AppendPath *) lfirst(lc);

		if (!IsA(cur_inner_path, AppendPath))
			continue;

		outer = outerrel->cheapest_total_path;

		inner_required = bms_union(PATH_REQ_OUTER((Path *) cur_inner_path),
								   bms_make_singleton(outerrel->relid));

		inner = create_runtimeappend_path(root, cur_inner_path,
										  get_appendrel_parampathinfo(innerrel,
																	  inner_required),
										  paramsel);

		initial_cost_nestloop(root, &workspace, jointype,
							  outer, inner,
							  extra->sjinfo, &extra->semifactors);

		pathkeys = build_join_pathkeys(root, joinrel, jointype, outer->pathkeys);

		nest_path = create_nestloop_path(root, joinrel, jointype, &workspace,
										 extra->sjinfo, &extra->semifactors,
										 outer, inner, extra->restrictlist,
										 pathkeys,
										 calc_nestloop_required_outer(outer, inner));

		add_path(joinrel, (Path *) nest_path);
	}
}

/* Cope with simple relations */
void
pathman_rel_pathlist_hook(PlannerInfo *root, RelOptInfo *rel, Index rti, RangeTblEntry *rte)
{
	PartRelationInfo   *prel = NULL;
	RangeTblEntry	  **new_rte_array;
	RelOptInfo		  **new_rel_array;
	bool				found;
	int					len;

	/* Invoke original hook if needed */
	if (set_rel_pathlist_hook_next != NULL)
		set_rel_pathlist_hook_next(root, rel, rti, rte);

	if (!pg_pathman_enable)
		return;

	/* This works only for SELECT queries (at least for now) */
	if (root->parse->commandType != CMD_SELECT || !inheritance_disabled)
		return;

	/* Lookup partitioning information for parent relation */
	prel = get_pathman_relation_info(rte->relid, &found);

	if (prel != NULL && found)
	{
		ListCell	   *lc;
		Oid			   *dsm_arr;
		List		   *ranges,
					   *wrappers;
		PathKey		   *pathkeyAsc = NULL,
					   *pathkeyDesc = NULL;
		double			paramsel = 1.0;
		WalkerContext	context;
		int				i;

		if (prel->parttype == PT_RANGE)
		{
			/*
			 * Get pathkeys for ascending and descending sort by partition
			 * column
			 */
			List		   *pathkeys;
			Var			   *var;
			Oid				vartypeid,
							varcollid;
			int32			type_mod;
			TypeCacheEntry *tce;

			/* Make Var from patition column */
			get_rte_attribute_type(rte, prel->attnum,
								   &vartypeid, &type_mod, &varcollid);
			var = makeVar(rti, prel->attnum, vartypeid, type_mod, varcollid, 0);
			var->location = -1;

			/* Determine operator type */
			tce = lookup_type_cache(var->vartype, TYPECACHE_LT_OPR | TYPECACHE_GT_OPR);

			/* Make pathkeys */
			pathkeys = build_expression_pathkey(root, (Expr *)var, NULL,
												tce->lt_opr, NULL, false);
			if (pathkeys)
				pathkeyAsc = (PathKey *) linitial(pathkeys);
			pathkeys = build_expression_pathkey(root, (Expr *)var, NULL,
												tce->gt_opr, NULL, false);
			if (pathkeys)
				pathkeyDesc = (PathKey *) linitial(pathkeys);
		}

		rte->inh = true;
		dsm_arr = (Oid *) dsm_array_get_pointer(&prel->children);
		ranges = list_make1_int(make_irange(0, prel->children_count - 1, false));

		context.prel = prel;
		context.econtext = NULL;
		context.hasLeast = false;
		context.hasGreatest = false;

		/* Make wrappers over restrictions and collect final rangeset */
		wrappers = NIL;
		foreach(lc, rel->baserestrictinfo)
		{
			WrapperNode	   *wrap;
			RestrictInfo   *rinfo = (RestrictInfo*) lfirst(lc);

			wrap = walk_expr_tree(rinfo->clause, &context);
			if (!lc->next)
				finish_least_greatest(wrap, &context);

			paramsel *= wrap->paramsel;
			wrappers = lappend(wrappers, wrap);
			ranges = irange_list_intersect(ranges, wrap->rangeset);
		}

		/*
		 * Expand simple_rte_array and simple_rel_array
		 */

		if (ranges)
		{
			len = irange_list_length(ranges);

			/* Expand simple_rel_array and simple_rte_array */
			new_rel_array = (RelOptInfo **)
				palloc0((root->simple_rel_array_size + len) * sizeof(RelOptInfo *));

			/* simple_rte_array is an array equivalent of the rtable list */
			new_rte_array = (RangeTblEntry **)
				palloc0((root->simple_rel_array_size + len) * sizeof(RangeTblEntry *));

			/* Copy relations to the new arrays */
	        for (i = 0; i < root->simple_rel_array_size; i++)
	        {
	                new_rel_array[i] = root->simple_rel_array[i];
	                new_rte_array[i] = root->simple_rte_array[i];
	        }

			/* Free old arrays */
			pfree(root->simple_rel_array);
			pfree(root->simple_rte_array);

			root->simple_rel_array_size += len;
			root->simple_rel_array = new_rel_array;
			root->simple_rte_array = new_rte_array;
		}

		/* Target list should be sorted in physical order for custom nodes to work */
		rel->reltargetlist = sort_rel_tlist(rel->reltargetlist);

		/*
		 * Iterate all indexes in rangeset and append corresponding child
		 * relations.
		 */
		foreach(lc, ranges)
		{
			IndexRange	irange = lfirst_irange(lc);

			for (i = irange_lower(irange); i <= irange_upper(irange); i++)
				append_child_relation(root, rel, rti, rte, i, dsm_arr[i], wrappers);
		}

		/* Clear old path list */
		list_free(rel->pathlist);

		rel->pathlist = NIL;
		set_append_rel_pathlist(root, rel, rti, rte, pathkeyAsc, pathkeyDesc);
		set_append_rel_size(root, rel, rti, rte);

		/* No need to go further, return */
		if (!(pg_pathman_enable_runtimeappend ||
			  pg_pathman_enable_runtime_merge_append))
			return;

		foreach (lc, rel->pathlist)
		{
			AppendPath	   *cur_path = (AppendPath *) lfirst(lc);
			Relids			inner_required = PATH_REQ_OUTER((Path *) cur_path);
			ParamPathInfo  *ppi = get_appendrel_parampathinfo(rel, inner_required);
			Path		   *inner_path = NULL;

			/* Skip if rel contains some join-related stuff or path type mismatched */
			if (!(IsA(cur_path, AppendPath) || IsA(cur_path, MergeAppendPath)) ||
				rel->has_eclass_joins ||
				rel->joininfo)
			{
				continue;
			}

			if (IsA(cur_path, AppendPath) && pg_pathman_enable_runtimeappend)
				inner_path = create_runtimeappend_path(root, cur_path,
													   ppi, paramsel);
			else if (IsA(cur_path, MergeAppendPath) &&
					 pg_pathman_enable_runtime_merge_append)
				inner_path = create_runtimemergeappend_path(root, cur_path,
															ppi, paramsel);

			if (inner_path)
				add_path(rel, inner_path);
		}
	}
}

void pg_pathman_enable_assign_hook(bool newval, void *extra)
{
	/* Return quickly if nothing has changed */
	if (newval == (pg_pathman_enable &&
				   pg_pathman_enable_runtimeappend &&
				   pg_pathman_enable_runtime_merge_append))
		return;

	pg_pathman_enable_runtime_merge_append = newval;
	pg_pathman_enable_runtimeappend = newval;

	elog(NOTICE,
		 "RuntimeAppend and RuntimeMergeAppend nodes have been %s",
		 newval ? "enabled" : "disabled");
}

/*
 * Sorts reltargetlist by Var's varattno (physical order) since
 * we can't use static build_path_tlist() for our custom nodes.
 *
 * See create_scan_plan & use_physical_tlist for more details.
 */
static List *
sort_rel_tlist(List *tlist)
{
	int			i;
	int			plain_tlist_size = list_length(tlist);
	Var		  **plain_tlist = palloc(plain_tlist_size * sizeof(Var *));
	ListCell   *tlist_cell;
	List	   *result = NIL;

	i = 0;
	foreach (tlist_cell, tlist)
		plain_tlist[i++] = lfirst(tlist_cell);

	qsort(plain_tlist, plain_tlist_size, sizeof(Var *), cmp_tlist_vars);

	for (i = 0; i < plain_tlist_size; i++)
		result = lappend(result, plain_tlist[i]);

	return result;
}

/* Compare Vars by varattno */
static int
cmp_tlist_vars(const void *a, const void *b)
{
	Var *v1 = *(Var **) a;
	Var *v2 = *(Var **) b;

	Assert(IsA(v1, Var) && IsA(v2, Var));

	if (v1->varattno > v2->varattno)
		return 1;
	else if (v1->varattno < v2->varattno)
		return -1;
	else
	{
		/* XXX: I really doubt this case is ok */
		Assert(v1->varattno != v2->varattno);
		return 0;
	}
}
