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
#include "catalog/pg_authid.h"
#include "fmgr.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/parsenodes.h"
#include "parser/parse_type.h"
#include "pg_plan_mgmt.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/guc.h"
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
static bool pgpm_resolve_role(const char *role_name, Oid *roleid,
							  char **reason, int *sqlerrcode);
static bool pgpm_try_plan(const char *sql_text, char *outline,
						  const char *search_path, Oid roleid,
						  int64 *queryId, pgpm_planned *planned,
						  ErrorData **edata);
static void pgpm_plan_not_found(int64 queryId, int plan_no);
static Datum pgpm_text_datum(const char *str, bool *isnull);

/*
 * SQL-callable function returning every plan of every statement.
 *
 * Statement text is stored as submitted, constants included, so like
 * pg_stat_statements we only show it to members of pg_read_all_stats and to
 * callers with the privileges of the role the statement was captured under.
 */
Datum
pg_plan_mgmt_get_plans(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Oid			userid = GetUserId();
	bool		show_all_text;
	List	   *stmts;

	InitMaterializedSRF(fcinfo, 0);

	if (unlikely(pgpm_stmt_dshash == NULL))
		pgpm_attach();
	if (pg_atomic_unlocked_test_flag(&pgpm_state->plans_ready))
		return (Datum) 0;

	show_all_text = has_privs_of_role(userid, ROLE_PG_READ_ALL_STATS);

	stmts = pgpm_store_get_all(InvalidOid);
	foreach_ptr(pgpm_stmt_info, stmt, stmts)
	{
		bool		show_text = show_all_text;

		if (!show_text)
		{
			Oid			roleid = get_role_oid(stmt->role_name, true);

			show_text = OidIsValid(roleid) && has_privs_of_role(userid, roleid);
		}

		for (int i = 0; i < stmt->nplans; i++)
		{
			pgpm_plan_info *plan = &stmt->plans[i];
			Datum		values[17];
			bool		nulls[17] = {0};

			CHECK_FOR_INTERRUPTS();

			values[0] = ObjectIdGetDatum(stmt->key.dbid);
			values[1] = Int64GetDatum(stmt->key.queryId);
			values[2] = Int32GetDatum(plan->plan_no);
			values[3] = CStringGetTextDatum(pgpm_status_name(plan->status));
			values[4] = BoolGetDatum(plan->enabled);
			values[5] = BoolGetDatum(plan->valid);
			values[6] = CStringGetTextDatum(pgpm_origin_name(plan->origin));
			if (show_text)
				values[7] = pgpm_text_datum(stmt->sql_text, &nulls[7]);
			else
				values[7] = CStringGetTextDatum("<insufficient privilege>");
			values[8] = pgpm_text_datum(stmt->search_path, &nulls[8]);
			values[9] = pgpm_text_datum(stmt->role_name, &nulls[9]);
			values[10] = pgpm_text_datum(plan->outline, &nulls[10]);
			values[11] = Float8GetDatum(plan->total_cost);
			values[12] = TimestampTzGetDatum(plan->created);
			if (plan->last_used == 0)
				nulls[13] = true;
			else
				values[13] = TimestampTzGetDatum(plan->last_used);
			if (plan->last_validated == 0)
				nulls[14] = true;
			else
				values[14] = TimestampTzGetDatum(plan->last_validated);
			values[15] = Int64GetDatum((int64) plan->plan_count);
			values[16] = Int64GetDatum((int64) plan->failures);

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
	int			plan_no = PG_GETARG_INT32(1);
	char		status = pgpm_parse_status(text_to_cstring(PG_GETARG_TEXT_PP(2)));

	pgpm_prepare(true);
	if (!pgpm_store_set_status(MyDatabaseId, queryId, plan_no, status))
		pgpm_plan_not_found(queryId, plan_no);
	PG_RETURN_VOID();
}

/*
 * SQL-callable function to enable or disable a plan.
 */
Datum
pg_plan_mgmt_set_plan_enabled(PG_FUNCTION_ARGS)
{
	int64		queryId = PG_GETARG_INT64(0);
	int			plan_no = PG_GETARG_INT32(1);
	bool		enabled = PG_GETARG_BOOL(2);

	pgpm_prepare(true);
	if (!pgpm_store_set_enabled(MyDatabaseId, queryId, plan_no, enabled))
		pgpm_plan_not_found(queryId, plan_no);
	PG_RETURN_VOID();
}

/*
 * SQL-callable function to delete a plan, or all plans of a statement if
 * plan_no is NULL. Returns the number of plans deleted.
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
									  PG_ARGISNULL(1) ? 0 : PG_GETARG_INT32(1),
									  PG_ARGISNULL(1)));
}

/*
 * SQL-callable function to add a plan given the statement text and an
 * outline.
 *
 * The statement is parsed in the current database to determine its query
 * ID, and planned with the outline, in the given environment (search_path
 * and role; by default the session's). The outline must be enforced
 * cleanly; what we store is the outline pg_plan_advice generates for the
 * resulting plan, so that hand-written or partial outlines are
 * canonicalized.
 */
Datum
pg_plan_mgmt_add_plan(PG_FUNCTION_ARGS)
{
	char	   *sql_text;
	char	   *outline;
	char		status;
	bool		enabled;
	const char *search_path;
	char	   *role_name;
	Oid			roleid;
	char	   *reason;
	int			sqlerrcode;
	int64		queryId;
	pgpm_planned planned;
	ErrorData  *edata;
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
	search_path = PG_ARGISNULL(4) ? GetConfigOption("search_path", false, false) :
		text_to_cstring(PG_GETARG_TEXT_PP(4));
	role_name = PG_ARGISNULL(5) ? GetUserNameFromId(GetUserId(), false) :
		text_to_cstring(PG_GETARG_TEXT_PP(5));
	if (role_name[0] == '\0')
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("role name must not be empty"));

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	pgpm_prepare(true);

	if (!pgpm_resolve_role(role_name, &roleid, &reason, &sqlerrcode))
		ereport(ERROR,
				errcode(sqlerrcode),
				errmsg("%s", reason));
	if (!pgpm_try_plan(sql_text, outline, search_path, roleid, &queryId,
					   &planned, &edata))
		ReThrowError(edata);
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
			if (strcmp(existing->plans[i].outline, planned.outline) == 0)
				info = existing->plans[i];
		}
	}
	if (info.plan_no == 0)
	{
		info.origin = PGPM_ORIGIN_USER;
		info.created = GetCurrentTimestamp();
	}
	info.status = status;
	info.enabled = enabled;
	info.valid = true;
	info.total_cost = planned.pstmt->planTree->total_cost;
	info.outline = planned.outline;

	pgpm_store_put_stmt(&key, pgpm_skip_explain(sql_text), search_path,
						role_name, 0, 0, 0, true);
	if (!pgpm_store_put_plan(&key, &info, true))
		ereport(ERROR,
				errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				errmsg("could not store plan"),
				errdetail("pg_plan_mgmt.max_plans (%d) was reached or the statement was deleted concurrently.",
						  pgpm_max_plans));

	values[0] = Int64GetDatum(queryId);
	values[1] = Int32GetDatum(info.plan_no);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values,
													  nulls)));
}

/*
 * SQL-callable function to re-plan every plan of the current database with
 * its outline, in the environment the statement was captured in, and record
 * whether it can still be enforced. Statements whose role no longer exists
 * or that the caller may not plan as are reported but left alone.
 *
 * If a statement's text now has a different query ID, the statement is
 * re-keyed. If a plan's outline now regenerates differently, the stored
 * outline is updated, or, if the statement already has a plan with the new
 * outline, the plan is deleted as a duplicate. Depending on the action,
 * invalid plans are also disabled or deleted.
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
		Oid			roleid;
		char	   *skip_reason = NULL;
		int			sqlerrcode;
		bool		can_plan;

		can_plan = pgpm_resolve_role(stmt->role_name, &roleid, &skip_reason,
									 &sqlerrcode);

		for (int i = 0; i < stmt->nplans; i++)
		{
			pgpm_plan_info *plan = &stmt->plans[i];
			pgpm_planned planned;
			ErrorData  *edata;
			int64		queryId = stmt->key.queryId;
			bool		valid;
			const char *detail = NULL;
			pgpm_stmt_key newkey;
			int			plan_no = plan->plan_no;
			char	   *new_outline = plan->outline;
			bool		deleted = false;
			Datum		values[5];
			bool		nulls[5] = {0};

			CHECK_FOR_INTERRUPTS();

			if (!can_plan)
			{
				/* No verdict possible; report why and leave the plan alone. */
				values[0] = Int64GetDatum(queryId);
				values[1] = Int32GetDatum(plan_no);
				nulls[2] = true;
				nulls[3] = true;
				values[4] = CStringGetTextDatum(skip_reason);
				tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
									 values, nulls);
				continue;
			}

			if (!pgpm_try_plan(stmt->sql_text, plan->outline,
							   stmt->search_path, roleid, &queryId,
							   &planned, &edata))
			{
				valid = false;
				detail = edata->message;
			}
			else
			{
				valid = planned.clean;
				if (!valid)
					detail = planned.problems;
				else if (planned.outline != NULL)
					new_outline = planned.outline;
			}

			memset(&newkey, 0, sizeof(newkey));
			newkey.dbid = stmt->key.dbid;
			newkey.queryId = queryId;

			if (newkey.queryId != stmt->key.queryId)
			{
				/* Move the plan to the statement's new key. */
				pgpm_plan_info info = *plan;

				pgpm_store_put_stmt(&newkey, stmt->sql_text,
									stmt->search_path, stmt->role_name,
									stmt->created, stmt->plannings, 0,
									false);
				info.plan_no = 0;
				info.outline = new_outline;
				info.valid = valid;
				if (valid)
					info.total_cost = planned.pstmt->planTree->total_cost;
				info.last_validated = now;
				if (pgpm_store_put_plan(&newkey, &info, false))
					plan_no = info.plan_no;
				(void) pgpm_store_delete(stmt->key.dbid, stmt->key.queryId,
										 plan->plan_no, false);
				detail = psprintf("re-keyed from query ID %" PRId64,
								  stmt->key.queryId);
			}
			else
			{
				if (strcmp(new_outline, plan->outline) != 0)
				{
					int			dup = -1;

					for (int j = 0; j < stmt->nplans && dup < 0; j++)
					{
						if (j != i &&
							strcmp(stmt->plans[j].outline, new_outline) == 0)
							dup = j;
					}
					if (dup >= 0)
					{
						(void) pgpm_store_delete(newkey.dbid, newkey.queryId,
												 plan_no, false);
						deleted = true;
						detail = psprintf("now produces the same plan as plan %d, deleted",
										  stmt->plans[dup].plan_no);
					}
					else if (pgpm_store_set_outline(newkey.dbid, newkey.queryId,
													plan_no, new_outline))
						detail = "outline updated";
				}
				if (!deleted)
					(void) pgpm_store_set_validation(newkey.dbid, newkey.queryId,
													 plan_no, valid,
													 valid ? planned.pstmt->planTree->total_cost : 0,
													 now);
			}

			if (!deleted && !valid && action == PGPM_VALIDATE_DISABLE)
				(void) pgpm_store_set_enabled(newkey.dbid, newkey.queryId,
											  plan_no, false);
			else if (!deleted && !valid && action == PGPM_VALIDATE_DELETE)
				(void) pgpm_store_delete(newkey.dbid, newkey.queryId,
										 plan_no, false);

			values[0] = Int64GetDatum(newkey.queryId);
			values[1] = Int32GetDatum(plan_no);
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
 * Resolve the role named by a statement's environment. Returns true with
 * *roleid set if the caller may plan as that role; otherwise false, with
 * *reason and *sqlerrcode describing why.
 *
 * A statement without a role is never planned: planning evaluates functions
 * (constant folding, inlining), so planning it as the caller would run
 * whatever the statement's author put there with the caller's privileges.
 */
static bool
pgpm_resolve_role(const char *role_name, Oid *roleid, char **reason,
				  int *sqlerrcode)
{
	*roleid = InvalidOid;
	if (role_name == NULL || role_name[0] == '\0')
	{
		*reason = pstrdup("statement has no recorded role");
		*sqlerrcode = ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE;
		return false;
	}

	*roleid = get_role_oid(role_name, true);
	if (!OidIsValid(*roleid))
	{
		*reason = psprintf("role \"%s\" does not exist", role_name);
		*sqlerrcode = ERRCODE_UNDEFINED_OBJECT;
		return false;
	}
	if (!has_privs_of_role(GetUserId(), *roleid))
	{
		*reason = psprintf("insufficient privilege to plan as role \"%s\"",
						   role_name);
		*sqlerrcode = ERRCODE_INSUFFICIENT_PRIVILEGE;
		return false;
	}
	return true;
}

/*
 * Parse a statement and plan it with an outline, in a subtransaction so that
 * any error (say, a dropped table) is caught and returned through *edata
 * rather than thrown. Returns true if planning succeeded.
 *
 * Planning happens in the given environment: with search_path set as given
 * (unless NULL or empty) and as the given role, which the caller must have
 * resolved with pgpm_resolve_role. Both matter to the planner, through name
 * resolution, row-level security policies and function inlining. The role
 * is assumed with SECURITY_RESTRICTED_OPERATION, as ANALYZE does for index
 * expressions, since planning can execute functions. Rolling back the
 * subtransaction undoes both, on success and on error.
 */
static bool
pgpm_try_plan(const char *sql_text, char *outline, const char *search_path,
			  Oid roleid, int64 *queryId, pgpm_planned *planned,
			  ErrorData **edata)
{
	MemoryContext oldcontext = CurrentMemoryContext;
	ResourceOwner oldowner = CurrentResourceOwner;
	volatile bool ok = false;

	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(oldcontext);

	PG_TRY();
	{
		int			cursorOptions;
		Oid			save_userid;
		int			save_sec_context;
		Query	   *query;

		if (search_path != NULL && search_path[0] != '\0')
			(void) set_config_option("search_path", search_path,
									 PGC_USERSET, PGC_S_SESSION,
									 GUC_ACTION_SET, true, 0, false);
		Assert(OidIsValid(roleid));
		GetUserIdAndSecContext(&save_userid, &save_sec_context);
		SetUserIdAndSecContext(roleid,
							   save_sec_context | SECURITY_RESTRICTED_OPERATION);

		query = pgpm_parse_statement(sql_text, &cursorOptions);
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
		MemoryContextSwitchTo(oldcontext);
		*edata = CopyErrorData();
		FlushErrorState();

		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcontext);
		CurrentResourceOwner = oldowner;
	}
	PG_END_TRY();

	return ok;
}

static void
pgpm_plan_not_found(int64 queryId, int plan_no)
{
	ereport(ERROR,
			errcode(ERRCODE_UNDEFINED_OBJECT),
			errmsg("plan %d of query %" PRId64 " does not exist",
				   plan_no, queryId));
}

/*
 * Make a text datum of a string stored in shared memory, or NULL if it is
 * not valid in this database's encoding (it may have come from a database
 * with a different one).
 */
static Datum
pgpm_text_datum(const char *str, bool *isnull)
{
	if (!pg_verify_mbstr(GetDatabaseEncoding(), str, strlen(str), true))
	{
		*isnull = true;
		return (Datum) 0;
	}
	return CStringGetTextDatum(str);
}
