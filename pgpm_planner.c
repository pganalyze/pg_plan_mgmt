/*-------------------------------------------------------------------------
 *
 * pgpm_planner.c
 *	  choose among the plans of a statement's baseline at planning time
 *
 * We take control in planner_hook. The query is first planned normally,
 * with pg_plan_advice asked to generate advice for the result; the hash of
 * that advice identifies the minimum-cost plan. If that plan is acceptable
 * we return it. Otherwise the query is planned again once for each
 * candidate plan of the baseline, with the candidate's outline supplied
 * through our pg_plan_advice advisor. A candidate counts as enforced only if
 * pg_plan_advice reports no problems with the supplied outline and the
 * advice generated for the resulting plan is the candidate's outline again,
 * i.e. we really got that plan back; the cheapest such candidate wins.
 *
 * Copyright (c) 2026, Duboce Labs, Inc. (pganalyze) <team@pganalyze.com>
 *
 *	  pgpm_planner.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/pg_class.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/planner.h"
#include "parser/scansup.h"
#include "pg_plan_advice.h"
#include "pg_plan_mgmt.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

/*
 * Context for pgpm_manageable_walker.
 */
typedef struct pgpm_manageable_context
{
	bool		user_relation;	/* references a permanent user table */
	bool		excluded;		/* references a catalog or temporary table */
} pgpm_manageable_context;

/* Saved hook value */
static planner_hook_type prev_planner_hook = NULL;

/*
 * Nesting depth of our planner hook, and the outline our advisor should
 * supply to the planner invocation at depth pgpm_supply_depth.
 */
static int	pgpm_planner_depth = 0;
static int	pgpm_supply_depth = -1;
static char *pgpm_supply_outline = NULL;

static PlannedStmt *pgpm_planner(Query *parse, const char *query_string,
								 int cursorOptions,
								 ParamListInfo boundParams,
								 ExplainState *es);
static PlannedStmt *pgpm_planner_guts(Query *parse, const char *query_string,
									  int cursorOptions,
									  ParamListInfo boundParams,
									  ExplainState *es);
static PlannedStmt *pgpm_call_next_planner(Query *parse,
										   const char *query_string,
										   int cursorOptions,
										   ParamListInfo boundParams,
										   ExplainState *es);
static bool pgpm_manageable_walker(Node *node,
								   pgpm_manageable_context *context);
static bool pgpm_plan_is_accepted(pgpm_plan_info *plan);
static pgpm_plan_info *pgpm_find_plan_info(pgpm_stmt_info *stmt,
										   const char *outline);
static char *pgpm_statement_text(const char *query_string, Query *parse);
static bool pgpm_feedback_is_clean(List *feedback, StringInfo problems);
static void pgpm_annotate(PlannedStmt *pstmt, const char *note,
						  int plan_no, char status, bool show_min,
						  int min_plan_no, char min_status);

/*
 * Install planner-related hooks.
 */
void
pgpm_planner_install_hooks(void)
{
	prev_planner_hook = planner_hook;
	planner_hook = pgpm_planner;
}

/*
 * Our pg_plan_advice advisor: supply the outline we're enforcing, but only
 * to the planner invocation that asked for it.
 */
char *
pgpm_advisor(PlannerGlobal *glob, Query *parse, const char *query_string,
			 int cursorOptions, ExplainState *es)
{
	if (pgpm_supply_outline != NULL &&
		pgpm_supply_depth == pgpm_planner_depth)
		return pgpm_supply_outline;
	return NULL;
}

/*
 * Plan a query once.
 *
 * If outline is NULL, the query is planned normally and pg_plan_advice is
 * asked to generate advice for the resulting plan. Otherwise the outline is
 * supplied as advice and we also ask for advice feedback, from which we
 * determine whether the outline was enforced cleanly.
 *
 * The planner may scribble on parse, so callers who want to plan again must
 * pass a copy.
 */
void
pgpm_plan_once(Query *parse, const char *query_string, int cursorOptions,
			   ParamListInfo boundParams, ExplainState *es, char *outline,
			   pgpm_planned *result)
{
	int			save_supply_depth = pgpm_supply_depth;
	char	   *save_supply_outline = pgpm_supply_outline;
	int			nestlevel = -1;
	PlannedStmt *pstmt;
	List	   *pgpa_items = NIL;

	memset(result, 0, sizeof(pgpm_planned));

	if (outline != NULL)
	{
		/*
		 * pg_plan_advice only generates feedback under EXPLAIN or when told
		 * to always do so; there's no API to request it. So, set the GUC for
		 * the duration of this planning. This also makes it generate the
		 * advice string.
		 */
		nestlevel = NewGUCNestLevel();
		(void) set_config_option("pg_plan_advice.always_store_advice_details",
								 "on", PGC_USERSET, PGC_S_SESSION,
								 GUC_ACTION_SAVE, true, 0, false);
		pgpm_supply_outline = outline;
		pgpm_supply_depth = pgpm_planner_depth;
	}
	else
		(*pgpm_request_advice_generation) (true);

	PG_TRY();
	{
		pstmt = pgpm_call_next_planner(parse, query_string, cursorOptions,
									   boundParams, es);
	}
	PG_FINALLY();
	{
		pgpm_supply_outline = save_supply_outline;
		pgpm_supply_depth = save_supply_depth;
		if (outline == NULL)
			(*pgpm_request_advice_generation) (false);
	}
	PG_END_TRY();

	if (nestlevel >= 0)
		AtEOXact_GUC(true, nestlevel);

	result->pstmt = pstmt;

	/* Find what pg_plan_advice left for us in the PlannedStmt. */
	foreach_node(DefElem, item, pstmt->extension_state)
	{
		if (strcmp(item->defname, "pg_plan_advice") == 0)
		{
			pgpa_items = (List *) item->arg;
			break;
		}
	}

	foreach_node(DefElem, item, pgpa_items)
	{
		if (strcmp(item->defname, "advice_string") == 0)
			result->outline = strVal(item->arg);
		else if (strcmp(item->defname, "feedback") == 0 && outline != NULL)
		{
			StringInfoData problems;

			initStringInfo(&problems);
			result->clean = pgpm_feedback_is_clean((List *) item->arg,
												   &problems);
			if (!result->clean)
				result->problems = problems.data;
		}
	}

	/*
	 * If we supplied an outline but got no feedback, either it could not be
	 * parsed or some other advisor answered first. Either way, we can't tell
	 * what was enforced.
	 */
	if (outline != NULL && !result->clean && result->problems == NULL)
		result->problems = "pg_plan_advice reported no feedback; the outline may not be valid advice, or another advisor supplied advice";
}

/*
 * Is this a statement whose plans we should manage?
 *
 * It must reference at least one permanent user table and must not reference
 * any catalog or temporary table. Temporary tables have per-session OIDs and
 * therefore per-session query IDs, so baselines for them are useless.
 */
bool
pgpm_query_is_manageable(Query *parse)
{
	pgpm_manageable_context context = {0};

	(void) pgpm_manageable_walker((Node *) parse, &context);
	return context.user_relation && !context.excluded;
}

static bool
pgpm_manageable_walker(Node *node, pgpm_manageable_context *context)
{
	if (node == NULL)
		return false;

	if (IsA(node, RangeTblEntry))
	{
		RangeTblEntry *rte = (RangeTblEntry *) node;

		if (rte->rtekind == RTE_RELATION)
		{
			if (IsCatalogRelationOid(rte->relid) ||
				get_rel_persistence(rte->relid) == RELPERSISTENCE_TEMP)
			{
				context->excluded = true;
				return true;
			}
			context->user_relation = true;
		}
		return false;
	}

	if (IsA(node, Query))
		return query_tree_walker((Query *) node, pgpm_manageable_walker,
								 context, QTW_EXAMINE_RTES_BEFORE);

	return expression_tree_walker(node, pgpm_manageable_walker, context);
}

/*
 * planner_hook entry point: keep track of the nesting depth.
 */
static PlannedStmt *
pgpm_planner(Query *parse, const char *query_string, int cursorOptions,
			 ParamListInfo boundParams, ExplainState *es)
{
	PlannedStmt *result;

	pgpm_planner_depth++;
	PG_TRY();
	{
		result = pgpm_planner_guts(parse, query_string, cursorOptions,
								   boundParams, es);
	}
	PG_FINALLY();
	{
		pgpm_planner_depth--;
	}
	PG_END_TRY();

	return result;
}

static PlannedStmt *
pgpm_planner_guts(Query *parse, const char *query_string, int cursorOptions,
				  ParamListInfo boundParams, ExplainState *es)
{
	pgpm_stmt_info *stmt;
	bool		capture;
	bool		may_enforce = false;
	MemoryContext tmpcxt = NULL;
	MemoryContext oldcxt = CurrentMemoryContext;
	Query	   *work;
	pgpm_planned p0;
	pgpm_plan_info *min_plan;
	char		min_status = 0;
	PlannedStmt *chosen;
	pgpm_plan_info *chosen_plan = NULL;
	const char *note = NULL;
	PlannedStmt *result;

	/*
	 * Capture only in superuser sessions. Planning happens before the
	 * executor checks permissions, so otherwise any user could get a
	 * statement captured that they may not even run, and since the first
	 * plan captured becomes Approved, seed the baseline that other sessions
	 * then enforce. The session user is checked rather than the current user
	 * so that a superuser can SET ROLE to the application's role and capture
	 * in its environment.
	 */
	capture = pgpm_capture_plan_baselines != PGPM_CAPTURE_OFF &&
		superuser_arg(GetSessionUserId());

	/*
	 * Quick exit if we have nothing to do. We also stay out of the way in
	 * parallel mode (say, a parallel-safe function planning a query in a
	 * parallel leader), since enforcing a plan requires setting a GUC.
	 */
	if ((!capture && !pgpm_use_plan_baselines) || parse->queryId == 0 ||
		IsInParallelMode())
		return pgpm_call_next_planner(parse, query_string, cursorOptions,
									  boundParams, es);

	if (unlikely(pgpm_stmt_dshash == NULL))
		pgpm_attach();

	/* Ignore baselines until they have been loaded from disk. */
	if (pg_atomic_unlocked_test_flag(&pgpm_state->plans_ready) ||
		!pgpm_query_is_manageable(parse))
		return pgpm_call_next_planner(parse, query_string, cursorOptions,
									  boundParams, es);

	stmt = pgpm_store_lookup(MyDatabaseId, parse->queryId);
	if (stmt == NULL && !capture)
		return pgpm_call_next_planner(parse, query_string, cursorOptions,
									  boundParams, es);

	if (pgpm_use_plan_baselines && stmt != NULL)
	{
		for (int i = 0; i < stmt->nplans; i++)
		{
			if (pgpm_plan_is_accepted(&stmt->plans[i]))
			{
				may_enforce = true;
				break;
			}
		}
	}

	/*
	 * If we might plan more than once, do all planning in a temporary
	 * context and on copies of the Query, and copy only the winning plan to
	 * the caller's context. Otherwise, the losing plans would stay around for
	 * as long as the caller's context lives, which for a cached plan is
	 * indefinitely.
	 */
	if (may_enforce)
	{
		tmpcxt = AllocSetContextCreate(CurrentMemoryContext,
									   "pg_plan_mgmt planning",
									   ALLOCSET_DEFAULT_SIZES);
		MemoryContextSwitchTo(tmpcxt);
		work = copyObject(parse);
	}
	else
		work = parse;

	/* Find the minimum-cost plan. */
	pgpm_plan_once(work, query_string, cursorOptions, boundParams, es, NULL,
				   &p0);
	if (p0.outline == NULL)
	{
		/* Shouldn't happen, but if it does, just leave it alone. */
		result = p0.pstmt;
		if (tmpcxt != NULL)
		{
			MemoryContextSwitchTo(oldcxt);
			result = copyObject(result);
			MemoryContextDelete(tmpcxt);
		}
		return result;
	}

	min_plan = pgpm_find_plan_info(stmt, p0.outline);
	if (min_plan != NULL)
		min_status = min_plan->status;

	/* Capture it. */
	if (capture)
	{
		bool		manual = pgpm_capture_plan_baselines == PGPM_CAPTURE_MANUAL;
		char	   *sql_text = NULL;
		const char *search_path = NULL;
		char	   *role_name = NULL;
		int			captured_no;
		char		captured_status;
		pgpm_capture_result cr;

		/*
		 * The statement's text and environment are only needed if its entry
		 * has to be created, which is possible only if we didn't find one.
		 */
		if (stmt == NULL)
		{
			sql_text = pgpm_statement_text(query_string, parse);
			search_path = GetConfigOption("search_path", false, false);
			role_name = GetUserNameFromId(GetUserId(), false);
		}

		cr = pgpm_store_capture(MyDatabaseId, parse->queryId,
								sql_text, search_path, role_name,
								p0.outline,
								p0.pstmt->planTree->total_cost,
								manual ? PGPM_ORIGIN_MANUAL :
								PGPM_ORIGIN_AUTOMATIC,
								manual ? 1 : 2, &captured_no,
								&captured_status);
		if (cr == PGPM_CAPTURE_ADDED ||
			(cr == PGPM_CAPTURE_EXISTING && min_plan == NULL))
		{
			/*
			 * Pretend we knew about it all along. (In the EXISTING case it
			 * was captured concurrently since we looked.)
			 */
			min_plan = palloc0_object(pgpm_plan_info);
			min_plan->plan_no = captured_no;
			min_plan->status = captured_status;
			min_plan->enabled = true;
			min_plan->valid = true;
			min_plan->outline = p0.outline;
			min_status = captured_status;
			if (cr == PGPM_CAPTURE_ADDED)
				ereport(DEBUG1,
						errmsg_internal("captured plan %d for query %" PRId64 " as %s",
										captured_no, parse->queryId,
										pgpm_status_name(captured_status)));
		}
	}

	/* Decide which plan to use. */
	chosen = p0.pstmt;
	chosen_plan = min_plan;
	if (!pgpm_use_plan_baselines)
	{
		/* Not enforcing anything, just capturing. */
	}
	else if (min_plan != NULL && pgpm_plan_is_accepted(min_plan))
	{
		/* The optimizer's choice is fine. */
	}
	else if (pgpm_unapproved_plan_execution_threshold > 0 &&
			 (min_status == 0 || min_status == PGPM_STATUS_UNAPPROVED) &&
			 (min_plan == NULL || (min_plan->enabled && min_plan->valid)) &&
			 p0.pstmt->planTree->total_cost <
			 pgpm_unapproved_plan_execution_threshold)
	{
		note = "Unapproved plan used because its cost is below unapproved_plan_execution_threshold";
	}
	else if (!may_enforce)
	{
		note = "No approved plan is available, minimum-cost plan used";
	}
	else
	{
		static const char tiers[] = {PGPM_STATUS_PREFERRED,
		PGPM_STATUS_APPROVED};
		pgpm_planned best = {0};
		pgpm_plan_info *best_plan = NULL;

		for (int t = 0; t < lengthof(tiers) && best_plan == NULL; t++)
		{
			for (int i = 0; i < stmt->nplans; i++)
			{
				pgpm_plan_info *cand = &stmt->plans[i];
				pgpm_planned pc;

				if (cand->status != tiers[t] || !cand->enabled ||
					!cand->valid)
					continue;

				pgpm_plan_once(copyObject(parse), query_string, cursorOptions,
							   boundParams, es, cand->outline, &pc);

				/*
				 * Clean feedback is not enough: advice items that matched
				 * nothing are not reported as problems, so an outline none
				 * of whose items apply to this planning (say, because it
				 * names partitions that were pruned this time) is "clean"
				 * while constraining nothing. Require the plan we got to
				 * regenerate the candidate's outline.
				 */
				if (pc.clean &&
					(pc.outline == NULL ||
					 strcmp(pc.outline, cand->outline) != 0))
				{
					pc.clean = false;
					pc.problems = "the outline was accepted but produced a different plan";
				}
				if (!pc.clean)
				{
					pgpm_store_note_failure(MyDatabaseId, parse->queryId,
											cand->plan_no);
					ereport(DEBUG1,
							errmsg_internal("could not enforce plan %d for query %" PRId64 ": %s",
											cand->plan_no, parse->queryId,
											pc.problems));
					continue;
				}
				if (best_plan == NULL ||
					pc.pstmt->planTree->total_cost <
					best.pstmt->planTree->total_cost)
				{
					best = pc;
					best_plan = cand;
				}
			}
		}

		if (best_plan != NULL)
		{
			chosen = best.pstmt;
			chosen_plan = best_plan;
			note = best_plan->status == PGPM_STATUS_PREFERRED ?
				"Preferred plan used instead of minimum-cost plan" :
				"Approved plan used instead of minimum-cost plan";
		}
		else
			note = "No approved plan could be enforced, minimum-cost plan used";
	}

	/* Get the result into the caller's memory context. */
	MemoryContextSwitchTo(oldcxt);
	result = tmpcxt != NULL ? copyObject(chosen) : chosen;

	if (chosen_plan != NULL && chosen != p0.pstmt)
		pgpm_annotate(result, note, chosen_plan->plan_no, chosen_plan->status,
					  true, min_plan ? min_plan->plan_no : 0, min_status);
	else
		pgpm_annotate(result, note, min_plan ? min_plan->plan_no : 0,
					  min_status, false, 0, 0);

	if (chosen_plan != NULL)
		pgpm_store_note_use(MyDatabaseId, parse->queryId,
							chosen_plan->plan_no);

	if (tmpcxt != NULL)
		MemoryContextDelete(tmpcxt);

	return result;
}

static PlannedStmt *
pgpm_call_next_planner(Query *parse, const char *query_string,
					   int cursorOptions, ParamListInfo boundParams,
					   ExplainState *es)
{
	if (prev_planner_hook)
		return prev_planner_hook(parse, query_string, cursorOptions,
								 boundParams, es);
	return standard_planner(parse, query_string, cursorOptions, boundParams,
							es);
}

/*
 * May this plan be used without further ado?
 */
static bool
pgpm_plan_is_accepted(pgpm_plan_info *plan)
{
	return (plan->status == PGPM_STATUS_APPROVED ||
			plan->status == PGPM_STATUS_PREFERRED) &&
		plan->enabled && plan->valid;
}

static pgpm_plan_info *
pgpm_find_plan_info(pgpm_stmt_info *stmt, const char *outline)
{
	if (stmt == NULL)
		return NULL;
	for (int i = 0; i < stmt->nplans; i++)
	{
		if (strcmp(stmt->plans[i].outline, outline) == 0)
			return &stmt->plans[i];
	}
	return NULL;
}

/*
 * Extract the text of this statement from the query string, which may
 * contain several statements.
 */
static char *
pgpm_statement_text(const char *query_string, Query *parse)
{
	int			loc = parse->stmt_location;
	int			len = parse->stmt_len;
	int			total;

	if (query_string == NULL)
		return pstrdup("");

	total = strlen(query_string);
	if (loc < 0 || loc > total)
	{
		loc = 0;
		len = total;
	}
	else if (len <= 0 || loc + len > total)
		len = total - loc;

	while (len > 0 && scanner_isspace(query_string[loc]))
	{
		loc++;
		len--;
	}
	while (len > 0 && (scanner_isspace(query_string[loc + len - 1]) ||
					   query_string[loc + len - 1] == ';'))
		len--;

	return pstrdup(pgpm_skip_explain(pnstrdup(query_string + loc, len)));
}

/*
 * Skip whitespace and comments.
 */
static const char *
pgpm_skip_space(const char *p)
{
	for (;;)
	{
		if (scanner_isspace(*p))
			p++;
		else if (p[0] == '-' && p[1] == '-')
		{
			while (*p != '\0' && *p != '\n')
				p++;
		}
		else if (p[0] == '/' && p[1] == '*')
		{
			int			depth = 1;

			p += 2;
			while (*p != '\0' && depth > 0)
			{
				if (p[0] == '/' && p[1] == '*')
				{
					depth++;
					p += 2;
				}
				else if (p[0] == '*' && p[1] == '/')
				{
					depth--;
					p += 2;
				}
				else
					p++;
			}
		}
		else
			return p;
	}
}

/*
 * If p begins with the given keyword (case-insensitively, as a whole word),
 * return a pointer just past it; otherwise return NULL.
 */
static const char *
pgpm_skip_keyword(const char *p, const char *keyword)
{
	size_t		len = strlen(keyword);

	if (pg_strncasecmp(p, keyword, len) != 0 ||
		IS_HIGHBIT_SET(p[len]) || isalnum((unsigned char) p[len]) ||
		p[len] == '_' || p[len] == '$')
		return NULL;
	return p + len;
}

/*
 * If a statement's text is an EXPLAIN, return the text of the explained
 * statement; otherwise return the text unchanged.
 *
 * The planner sees the explained statement's Query with the location of the
 * whole EXPLAIN, so this is how we get at the statement text we want to
 * store.
 */
const char *
pgpm_skip_explain(const char *sql_text)
{
	const char *p = pgpm_skip_space(sql_text);
	const char *q;

	if ((p = pgpm_skip_keyword(p, "explain")) == NULL)
		return sql_text;
	p = pgpm_skip_space(p);

	if (*p == '(')
	{
		int			depth = 0;
		char		quote = 0;

		/* Skip the parenthesized option list. */
		for (; *p != '\0'; p++)
		{
			if (quote != 0)
			{
				if (*p == quote)
					quote = 0;
			}
			else if (*p == '\'' || *p == '"')
				quote = *p;
			else if (*p == '(')
				depth++;
			else if (*p == ')' && --depth == 0)
			{
				p++;
				break;
			}
		}
	}
	else
	{
		/* Old-style options. */
		for (;;)
		{
			if ((q = pgpm_skip_keyword(p, "analyze")) == NULL &&
				(q = pgpm_skip_keyword(p, "analyse")) == NULL &&
				(q = pgpm_skip_keyword(p, "verbose")) == NULL)
				break;
			p = pgpm_skip_space(q);
		}
	}

	p = pgpm_skip_space(p);
	return *p == '\0' ? sql_text : p;
}

/*
 * Check whether advice feedback reports problems with the supplied outline:
 * anything that only partially matched, didn't apply, conflicted or failed.
 * Items that matched nothing at all are not problems as far as pg_plan_advice
 * is concerned (that's what happens to advice for partitions pruned at plan
 * time), so callers that need the outline to have been reproduced must also
 * compare the regenerated outline.
 */
static bool
pgpm_feedback_is_clean(List *feedback, StringInfo problems)
{
	bool		clean = true;

	foreach_node(DefElem, item, feedback)
	{
		int			flags = intVal(item->arg);

		if ((flags & (PGPA_FB_FAILED | PGPA_FB_INAPPLICABLE |
					  PGPA_FB_CONFLICTING)) != 0 ||
			((flags & PGPA_FB_MATCH_PARTIAL) != 0 &&
			 (flags & PGPA_FB_MATCH_FULL) == 0))
		{
			if (!clean)
				appendStringInfoString(problems, ", ");
			appendStringInfo(problems, "%s (flags 0x%x)", item->defname,
							 flags);
			clean = false;
		}
	}

	return clean;
}

/*
 * Record what we did in the PlannedStmt, for EXPLAIN: the plan that was
 * chosen, and, if a different one was the minimum-cost plan, that one too.
 * A plan number of 0 and a status of 0 mean the plan is not in the baseline.
 */
static void
pgpm_annotate(PlannedStmt *pstmt, const char *note, int plan_no, char status,
			  bool show_min, int min_plan_no, char min_status)
{
	List	   *items = NIL;

	if (note != NULL)
		items = lappend(items, makeDefElem("note",
										   (Node *) makeString(pstrdup(note)),
										   -1));
	if (plan_no > 0)
		items = lappend(items, makeDefElem("plan_no",
										   (Node *) makeInteger(plan_no), -1));
	items = lappend(items,
					makeDefElem("status",
								(Node *) makeString(pstrdup(status ? pgpm_status_name(status) : "Not Captured")),
								-1));
	if (show_min)
	{
		if (min_plan_no > 0)
			items = lappend(items,
							makeDefElem("min_cost_plan_no",
										(Node *) makeInteger(min_plan_no),
										-1));
		items = lappend(items,
						makeDefElem("min_cost_status",
									(Node *) makeString(pstrdup(min_status ? pgpm_status_name(min_status) : "Not Captured")),
									-1));
	}

	pstmt->extension_state = lappend(pstmt->extension_state,
									 makeDefElem("pg_plan_mgmt",
												 (Node *) items, -1));
}
