/*-------------------------------------------------------------------------
 *
 * pgpm_funcs.c
 *	  SQL interface to pg_plan_mgmt
 *
 * Copyright (c) 2026, Duboce Labs, Inc. (pganalyze) <team@pganalyze.com>
 *
 *	  pgpm_funcs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/xact.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/parsenodes.h"
#include "parser/parse_type.h"
#include "pg_plan_mgmt.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/resowner.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"

PG_FUNCTION_INFO_V1(pg_plan_mgmt_get_plans);
PG_FUNCTION_INFO_V1(pg_plan_mgmt_set_plan_status);
PG_FUNCTION_INFO_V1(pg_plan_mgmt_set_plan_enabled);
PG_FUNCTION_INFO_V1(pg_plan_mgmt_delete_plan);
PG_FUNCTION_INFO_V1(pg_plan_mgmt_add_plan);
PG_FUNCTION_INFO_V1(pg_plan_mgmt_validate_plans);
PG_FUNCTION_INFO_V1(pg_plan_mgmt_reset);

typedef enum
{
	PGPM_VALIDATE_UPDATE,
	PGPM_VALIDATE_DISABLE,
	PGPM_VALIDATE_DELETE
} pgpm_validate_action;

static void pgpm_prepare(bool modify);
static Query *pgpm_parse_statement(const char *sql_text, int *cursorOptions);
static bool pgpm_try_plan(const char *sql_text, char *outline,
						  int64 *queryId, pgpm_planned *planned,
						  char **error);
static void pgpm_plan_not_found(int64 queryId, int64 plan_hash);

/*
 * SQL-callable function returning every plan of every statement.
 */
Datum
pg_plan_mgmt_get_plans(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	List	   *stmts;

	InitMaterializedSRF(fcinfo, 0);

	if (unlikely(pgpm_stmt_dshash == NULL))
		pgpm_attach();
	if (pg_atomic_unlocked_test_flag(&pgpm_state->plans_ready))
		return (Datum) 0;

	stmts = pgpm_store_get_all(InvalidOid);
	foreach_ptr(pgpm_stmt_info, stmt, stmts)
	{
		for (int i = 0; i < stmt->nplans; i++)
		{
			pgpm_plan_info *plan = &stmt->plans[i];
			Datum		values[15];
			bool		nulls[15] = {0};

			values[0] = ObjectIdGetDatum(stmt->key.dbid);
			values[1] = Int64GetDatum(stmt->key.queryId);
			values[2] = Int64GetDatum(plan->plan_hash);
			values[3] = CStringGetTextDatum(pgpm_status_name(plan->status));
			values[4] = BoolGetDatum(plan->enabled);
			values[5] = BoolGetDatum(plan->valid);
			values[6] = CStringGetTextDatum(pgpm_origin_name(plan->origin));
			values[7] = CStringGetTextDatum(stmt->sql_text);
			values[8] = CStringGetTextDatum(plan->outline);
			values[9] = Float8GetDatum(plan->total_cost);
			values[10] = TimestampTzGetDatum(plan->created);
			if (plan->last_used == 0)
				nulls[11] = true;
			else
				values[11] = TimestampTzGetDatum(plan->last_used);
			if (plan->last_validated == 0)
				nulls[12] = true;
			else
				values[12] = TimestampTzGetDatum(plan->last_validated);
			values[13] = Int64GetDatum((int64) plan->plan_count);
			values[14] = Int64GetDatum((int64) plan->failures);

			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
								 values, nulls);
		}
	}

	return (Datum) 0;
}

/*
 * SQL-callable function to change the status of a plan.
 */
Datum
pg_plan_mgmt_set_plan_status(PG_FUNCTION_ARGS)
{
	int64		queryId = PG_GETARG_INT64(0);
	int64		plan_hash = PG_GETARG_INT64(1);
	char		status = pgpm_parse_status(text_to_cstring(PG_GETARG_TEXT_PP(2)));

	pgpm_prepare(true);
	if (!pgpm_store_set_status(MyDatabaseId, queryId, plan_hash, status))
		pgpm_plan_not_found(queryId, plan_hash);
	PG_RETURN_VOID();
}

/*
 * SQL-callable function to enable or disable a plan.
 */
Datum
pg_plan_mgmt_set_plan_enabled(PG_FUNCTION_ARGS)
{
	int64		queryId = PG_GETARG_INT64(0);
	int64		plan_hash = PG_GETARG_INT64(1);
	bool		enabled = PG_GETARG_BOOL(2);

	pgpm_prepare(true);
	if (!pgpm_store_set_enabled(MyDatabaseId, queryId, plan_hash, enabled))
		pgpm_plan_not_found(queryId, plan_hash);
	PG_RETURN_VOID();
}

/*
 * SQL-callable function to delete a plan, or all plans of a statement if
 * plan_hash is NULL. Returns the number of plans deleted.
 */
Datum
pg_plan_mgmt_delete_plan(PG_FUNCTION_ARGS)
{
	int64		queryId;

	if (PG_ARGISNULL(0))
		PG_RETURN_INT32(0);
	queryId = PG_GETARG_INT64(0);

	pgpm_prepare(true);
	PG_RETURN_INT32(pgpm_store_delete(MyDatabaseId, queryId,
									  PG_ARGISNULL(1) ? 0 : PG_GETARG_INT64(1),
									  PG_ARGISNULL(1)));
}

/*
 * SQL-callable function to add a plan given the statement text and an
 * outline.
 *
 * The statement is parsed in the current database to determine its query
 * ID, and planned with the outline. The outline must be enforced cleanly;
 * what we store is the outline pg_plan_advice generates for the resulting
 * plan, so that hand-written or partial outlines are canonicalized.
 */
Datum
pg_plan_mgmt_add_plan(PG_FUNCTION_ARGS)
{
	char	   *sql_text;
	char	   *outline;
	char		status;
	bool		enabled;
	Query	   *query;
	int			cursorOptions;
	int64		queryId;
	pgpm_planned planned;
	pgpm_stmt_key key;
	pgpm_stmt_info *existing;
	pgpm_plan_info info = {0};
	TupleDesc	tupdesc;
	Datum		values[2];
	bool		nulls[2] = {0};

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
		ereport(ERROR,
				errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				errmsg("statement text and plan outline must not be null"));
	sql_text = text_to_cstring(PG_GETARG_TEXT_PP(0));
	outline = text_to_cstring(PG_GETARG_TEXT_PP(1));
	status = PG_ARGISNULL(2) ? PGPM_STATUS_APPROVED :
		pgpm_parse_status(text_to_cstring(PG_GETARG_TEXT_PP(2)));
	enabled = PG_ARGISNULL(3) ? true : PG_GETARG_BOOL(3);

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	pgpm_prepare(true);

	query = pgpm_parse_statement(sql_text, &cursorOptions);
	queryId = query->queryId;
	pgpm_plan_once(query, sql_text, cursorOptions, NULL, NULL, outline,
				   &planned);
	if (!planned.clean)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("plan outline could not be enforced"),
				errdetail("%s", planned.problems));
	if (planned.outline == NULL)
		elog(ERROR, "pg_plan_advice did not generate an outline");

	memset(&key, 0, sizeof(key));
	key.dbid = MyDatabaseId;
	key.queryId = queryId;

	/* If we already have this plan, keep its history. */
	existing = pgpm_store_lookup(MyDatabaseId, queryId);
	if (existing != NULL)
	{
		for (int i = 0; i < existing->nplans; i++)
		{
			if (existing->plans[i].plan_hash == planned.plan_hash)
				info = existing->plans[i];
		}
	}
	if (info.plan_hash == 0)
	{
		info.plan_hash = planned.plan_hash;
		info.origin = PGPM_ORIGIN_USER;
		info.created = GetCurrentTimestamp();
	}
	info.status = status;
	info.enabled = enabled;
	info.valid = true;
	info.total_cost = planned.pstmt->planTree->total_cost;
	info.outline = planned.outline;

	pgpm_store_put_stmt(&key, pgpm_skip_explain(sql_text), 0, 0, true);
	if (!pgpm_store_put_plan(&key, &info, true))
		ereport(ERROR,
				errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				errmsg("could not store plan"),
				errdetail("pg_plan_mgmt.max_plans (%d) was reached or the statement was deleted concurrently.",
						  pgpm_max_plans));

	values[0] = Int64GetDatum(queryId);
	values[1] = Int64GetDatum(planned.plan_hash);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values,
													  nulls)));
}

/*
 * SQL-callable function to re-plan every plan of the current database with
 * its outline and record whether it can still be enforced.
 *
 * If a statement's text now has a different query ID, or a plan's outline
 * now regenerates differently, the statement is re-keyed or the plan
 * re-hashed. Depending on the action, invalid plans are also disabled or
 * deleted.
 */
Datum
pg_plan_mgmt_validate_plans(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	char	   *action_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	pgpm_validate_action action;
	List	   *stmts;
	TimestampTz now = GetCurrentTimestamp();

	if (pg_strcasecmp(action_name, "update") == 0)
		action = PGPM_VALIDATE_UPDATE;
	else if (pg_strcasecmp(action_name, "disable") == 0)
		action = PGPM_VALIDATE_DISABLE;
	else if (pg_strcasecmp(action_name, "delete") == 0)
		action = PGPM_VALIDATE_DELETE;
	else
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("invalid action \"%s\"", action_name),
				errhint("Valid actions are \"update\", \"disable\", and \"delete\"."));

	InitMaterializedSRF(fcinfo, 0);
	pgpm_prepare(true);

	stmts = pgpm_store_get_all(MyDatabaseId);
	foreach_ptr(pgpm_stmt_info, stmt, stmts)
	{
		for (int i = 0; i < stmt->nplans; i++)
		{
			pgpm_plan_info *plan = &stmt->plans[i];
			pgpm_planned planned;
			char	   *error = NULL;
			int64		queryId = stmt->key.queryId;
			bool		valid;
			const char *detail = NULL;
			pgpm_stmt_key newkey;
			int64		new_hash = plan->plan_hash;
			Datum		values[5];
			bool		nulls[5] = {0};

			CHECK_FOR_INTERRUPTS();

			if (!pgpm_try_plan(stmt->sql_text, plan->outline, &queryId,
							   &planned, &error))
			{
				valid = false;
				detail = error;
			}
			else
			{
				valid = planned.clean;
				if (!valid)
					detail = planned.problems;
			}

			memset(&newkey, 0, sizeof(newkey));
			newkey.dbid = stmt->key.dbid;
			newkey.queryId = queryId;
			if (valid && planned.plan_hash != 0)
				new_hash = planned.plan_hash;

			if (newkey.queryId != stmt->key.queryId ||
				new_hash != plan->plan_hash)
			{
				pgpm_plan_info info = *plan;

				if (newkey.queryId != stmt->key.queryId)
				{
					pgpm_store_put_stmt(&newkey, stmt->sql_text, stmt->created,
										stmt->plannings, false);
					detail = psprintf("re-keyed from query ID %" PRId64,
									  stmt->key.queryId);
				}
				if (new_hash != plan->plan_hash)
				{
					info.plan_hash = new_hash;
					info.outline = planned.outline;
					detail = psprintf("%s%sre-hashed from plan hash %" PRId64,
									  detail ? detail : "", detail ? ", " : "",
									  plan->plan_hash);
				}
				info.valid = valid;
				if (valid)
					info.total_cost = planned.pstmt->planTree->total_cost;
				info.last_validated = now;
				(void) pgpm_store_put_plan(&newkey, &info, false);
				(void) pgpm_store_delete(stmt->key.dbid, stmt->key.queryId,
										 plan->plan_hash, false);
			}
			else
				(void) pgpm_store_set_validation(newkey.dbid, newkey.queryId,
												 new_hash, valid,
												 valid ? planned.pstmt->planTree->total_cost : 0,
												 now);

			if (!valid && action == PGPM_VALIDATE_DISABLE)
				(void) pgpm_store_set_enabled(newkey.dbid, newkey.queryId,
											  new_hash, false);
			else if (!valid && action == PGPM_VALIDATE_DELETE)
				(void) pgpm_store_delete(newkey.dbid, newkey.queryId,
										 new_hash, false);

			values[0] = Int64GetDatum(newkey.queryId);
			values[1] = Int64GetDatum(new_hash);
			values[2] = BoolGetDatum(valid);
			if (valid)
				values[3] = Float8GetDatum(planned.pstmt->planTree->total_cost);
			else
				nulls[3] = true;
			if (detail != NULL)
				values[4] = CStringGetTextDatum(detail);
			else
				nulls[4] = true;
			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
								 values, nulls);
		}
	}

	return (Datum) 0;
}

/*
 * SQL-callable function to delete all baselines of the current database.
 */
Datum
pg_plan_mgmt_reset(PG_FUNCTION_ARGS)
{
	pgpm_prepare(true);
	pgpm_store_reset(MyDatabaseId);
	PG_RETURN_VOID();
}

/*
 * Attach to shared memory, and if we're going to modify it, make sure it has
 * been loaded.
 */
static void
pgpm_prepare(bool modify)
{
	if (unlikely(pgpm_stmt_dshash == NULL))
		pgpm_attach();
	if (modify)
		pgpm_check_lockout();
}

/*
 * Parse and analyze a single statement and return its Query, along with the
 * cursor options it would be planned with.
 *
 * The planner sees, and we capture, statements as they were submitted, which
 * may be a utility statement wrapping the query that was actually planned:
 * EXPLAIN, PREPARE, DECLARE CURSOR, CREATE TABLE AS or COPY (query) TO. Those
 * are unwrapped here. PREPARE supplies the types of $n parameters; otherwise
 * they are inferred.
 */
static Query *
pgpm_parse_statement(const char *sql_text, int *cursorOptions)
{
	List	   *raw_parsetree_list;
	RawStmt    *rawstmt;
	List	   *query_list;
	Oid		   *paramTypes = NULL;
	int			numParams = 0;
	Query	   *query;

	*cursorOptions = CURSOR_OPT_PARALLEL_OK;

	raw_parsetree_list = pg_parse_query(sql_text);
	if (list_length(raw_parsetree_list) != 1)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("statement text must contain exactly one statement"));
	rawstmt = linitial_node(RawStmt, raw_parsetree_list);

	/* Unwrap utility statements that carry the planned query. */
	for (;;)
	{
		Node	   *stmt = rawstmt->stmt;

		if (IsA(stmt, ExplainStmt))
			rawstmt->stmt = ((ExplainStmt *) stmt)->query;
		else if (IsA(stmt, PrepareStmt))
		{
			PrepareStmt *pstmt = (PrepareStmt *) stmt;

			numParams = list_length(pstmt->argtypes);
			if (numParams > 0)
			{
				int			i = 0;

				paramTypes = palloc_array(Oid, numParams);
				foreach_node(TypeName, tn, pstmt->argtypes)
					paramTypes[i++] = typenameTypeId(NULL, tn);
			}
			rawstmt->stmt = pstmt->query;
		}
		else if (IsA(stmt, DeclareCursorStmt))
		{
			DeclareCursorStmt *cstmt = (DeclareCursorStmt *) stmt;

			*cursorOptions = cstmt->options;
			rawstmt->stmt = cstmt->query;
		}
		else if (IsA(stmt, CreateTableAsStmt))
			rawstmt->stmt = ((CreateTableAsStmt *) stmt)->query;
		else if (IsA(stmt, CopyStmt) && ((CopyStmt *) stmt)->query != NULL)
			rawstmt->stmt = ((CopyStmt *) stmt)->query;
		else
			break;
	}

	query_list = pg_analyze_and_rewrite_varparams(rawstmt, sql_text,
												  &paramTypes, &numParams,
												  NULL);
	if (list_length(query_list) != 1)
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("statements rewritten into multiple queries are not supported"));

	query = linitial_node(Query, query_list);
	if (query->commandType == CMD_UTILITY)
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("utility statements cannot have plans"));
	if (query->queryId == 0)
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("could not compute a query ID"),
				errhint("Make sure \"compute_query_id\" is not set to \"off\"."));

	return query;
}

/*
 * Parse a statement and plan it with an outline, in a subtransaction so that
 * any error (say, a dropped table) is caught and reported through *error.
 * Returns true if planning succeeded.
 */
static bool
pgpm_try_plan(const char *sql_text, char *outline, int64 *queryId,
			  pgpm_planned *planned, char **error)
{
	MemoryContext oldcontext = CurrentMemoryContext;
	ResourceOwner oldowner = CurrentResourceOwner;
	volatile bool ok = false;

	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(oldcontext);

	PG_TRY();
	{
		int			cursorOptions;
		Query	   *query = pgpm_parse_statement(sql_text, &cursorOptions);

		*queryId = query->queryId;
		pgpm_plan_once(query, sql_text, cursorOptions, NULL, NULL, outline,
					   planned);
		ok = true;

		/* Roll back to release locks; the results are in oldcontext. */
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcontext);
		CurrentResourceOwner = oldowner;
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(oldcontext);
		edata = CopyErrorData();
		FlushErrorState();

		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcontext);
		CurrentResourceOwner = oldowner;

		*error = edata->message;
	}
	PG_END_TRY();

	return ok;
}

static void
pgpm_plan_not_found(int64 queryId, int64 plan_hash)
{
	ereport(ERROR,
			errcode(ERRCODE_UNDEFINED_OBJECT),
			errmsg("plan %" PRId64 " of query %" PRId64 " does not exist",
				   plan_hash, queryId));
}
