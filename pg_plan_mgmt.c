/*-------------------------------------------------------------------------
 *
 * pg_plan_mgmt.c
 *	  module initialization, GUCs and EXPLAIN integration
 *
 * Copyright (c) 2026, Duboce Labs, Inc. (pganalyze) <team@pganalyze.com>
 *
 *	  pg_plan_mgmt.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <float.h>

#include "commands/explain.h"
#include "commands/explain_format.h"
#include "common/hashfn.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/queryjumble.h"
#include "pg_plan_advice.h"
#include "pg_plan_mgmt.h"
#include "postmaster/bgworker.h"
#include "utils/guc.h"

PG_MODULE_MAGIC_EXT(
					.name = "pg_plan_mgmt",
					.version = "1.0"
);

static const struct config_enum_entry capture_options[] = {
	{"off", PGPM_CAPTURE_OFF, false},
	{"manual", PGPM_CAPTURE_MANUAL, false},
	{"automatic", PGPM_CAPTURE_AUTOMATIC, false},
	{NULL, 0, false}
};

/* GUC variables */
int			pgpm_capture_plan_baselines = PGPM_CAPTURE_OFF;
bool		pgpm_use_plan_baselines = false;
double		pgpm_unapproved_plan_execution_threshold = 0;
int			pgpm_max_plans = 10000;
int			pgpm_max_statements = 10000;
int			pgpm_plan_retention_period = 32;
bool		pgpm_persist = true;
int			pgpm_persist_interval = 30;

/* Entry point into pg_plan_advice, resolved at load time */
void		(*pgpm_request_advice_generation) (bool activate);

/* Saved hook value */
static explain_per_plan_hook_type prev_explain_per_plan = NULL;

static void pgpm_explain_per_plan(PlannedStmt *plannedstmt,
								  IntoClause *into,
								  ExplainState *es,
								  const char *queryString,
								  ParamListInfo params,
								  QueryEnvironment *queryEnv);

/*
 * Initialize this module.
 */
void
_PG_init(void)
{
	void		(*add_advisor_fn) (pg_plan_advice_advisor_hook hook);

	/* We key everything on query IDs. */
	EnableQueryId();

	DefineCustomEnumVariable("pg_plan_mgmt.capture_plan_baselines",
							 "Captures plans of planned statements into their plan baselines.",
							 "\"manual\" captures every statement, \"automatic\" captures statements planned at least twice.",
							 &pgpm_capture_plan_baselines,
							 PGPM_CAPTURE_OFF,
							 capture_options,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("pg_plan_mgmt.use_plan_baselines",
							 "Restricts the planner to approved plans of managed statements.",
							 NULL,
							 &pgpm_use_plan_baselines,
							 false,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomRealVariable("pg_plan_mgmt.unapproved_plan_execution_threshold",
							 "Allows an unapproved minimum-cost plan to run if its estimated cost is below this value.",
							 "Zero disables this.",
							 &pgpm_unapproved_plan_execution_threshold,
							 0,
							 0,
							 DBL_MAX,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomIntVariable("pg_plan_mgmt.max_plans",
							"Maximum number of plans stored across all statements.",
							NULL,
							&pgpm_max_plans,
							10000,
							0,
							INT_MAX / 2,
							PGC_SIGHUP,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomIntVariable("pg_plan_mgmt.max_statements",
							"Maximum number of managed statements.",
							NULL,
							&pgpm_max_statements,
							10000,
							0,
							INT_MAX / 2,
							PGC_SIGHUP,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomIntVariable("pg_plan_mgmt.plan_retention_period",
							"Number of days after which unused plans are deleted.",
							"Zero keeps plans forever. Requires the background worker.",
							&pgpm_plan_retention_period,
							32,
							0,
							INT_MAX / 86400,
							PGC_SIGHUP,
							0,
							NULL,
							NULL,
							NULL);

	if (process_shared_preload_libraries_in_progress)
		DefineCustomBoolVariable("pg_plan_mgmt.persist",
								 "Saves and restores plan baselines across restarts.",
								 NULL,
								 &pgpm_persist,
								 true,
								 PGC_POSTMASTER,
								 0,
								 NULL,
								 NULL,
								 NULL);
	else
		pgpm_persist = false;

	DefineCustomIntVariable("pg_plan_mgmt.persist_interval",
							"Interval between saves of plan baselines, in seconds.",
							"Zero saves only at shutdown.",
							&pgpm_persist_interval,
							30,
							0,
							3600,
							PGC_SIGHUP,
							GUC_UNIT_S,
							NULL,
							NULL,
							NULL);

	MarkGUCPrefixReserved("pg_plan_mgmt");

	/* Start the background worker for persistence, if enabled. */
	if (pgpm_persist)
		pgpm_start_worker();

	/* Register as an advisor and look up the advice generation switch. */
	add_advisor_fn =
		load_external_function("pg_plan_advice", "pg_plan_advice_add_advisor",
							   true, NULL);
	(*add_advisor_fn) (pgpm_advisor);
	pgpm_request_advice_generation =
		load_external_function("pg_plan_advice",
							   "pg_plan_advice_request_advice_generation",
							   true, NULL);

	/* Install hooks. */
	pgpm_planner_install_hooks();
	prev_explain_per_plan = explain_per_plan_hook;
	explain_per_plan_hook = pgpm_explain_per_plan;
}

/*
 * Display name of a plan status.
 */
const char *
pgpm_status_name(char status)
{
	switch (status)
	{
		case PGPM_STATUS_APPROVED:
			return "Approved";
		case PGPM_STATUS_UNAPPROVED:
			return "Unapproved";
		case PGPM_STATUS_PREFERRED:
			return "Preferred";
		case PGPM_STATUS_REJECTED:
			return "Rejected";
	}
	return "Unknown";
}

/*
 * Parse a plan status name, case-insensitively. Errors out if invalid.
 */
char
pgpm_parse_status(const char *name)
{
	if (pg_strcasecmp(name, "approved") == 0)
		return PGPM_STATUS_APPROVED;
	if (pg_strcasecmp(name, "unapproved") == 0)
		return PGPM_STATUS_UNAPPROVED;
	if (pg_strcasecmp(name, "preferred") == 0)
		return PGPM_STATUS_PREFERRED;
	if (pg_strcasecmp(name, "rejected") == 0)
		return PGPM_STATUS_REJECTED;
	ereport(ERROR,
			errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			errmsg("invalid plan status \"%s\"", name),
			errhint("Valid statuses are \"Approved\", \"Unapproved\", \"Preferred\", and \"Rejected\"."));
	pg_unreachable();
}

/*
 * Display name of a plan origin.
 */
const char *
pgpm_origin_name(char origin)
{
	switch (origin)
	{
		case PGPM_ORIGIN_MANUAL:
			return "Manual";
		case PGPM_ORIGIN_AUTOMATIC:
			return "Automatic";
		case PGPM_ORIGIN_USER:
			return "User";
	}
	return "Unknown";
}

/*
 * Parse a plan origin name, case-insensitively. Returns 0 if invalid.
 */
char
pgpm_parse_origin(const char *name)
{
	if (pg_strcasecmp(name, "manual") == 0)
		return PGPM_ORIGIN_MANUAL;
	if (pg_strcasecmp(name, "automatic") == 0)
		return PGPM_ORIGIN_AUTOMATIC;
	if (pg_strcasecmp(name, "user") == 0)
		return PGPM_ORIGIN_USER;
	return 0;
}

/*
 * Compute the plan hash of an outline.
 *
 * Zero is reserved to mean "no plan", so map it to something else.
 */
int64
pgpm_hash_outline(const char *outline)
{
	uint64		h;

	h = hash_bytes_extended((const unsigned char *) outline,
							strlen(outline), 0);
	if (h == 0)
		h = 1;
	return (int64) h;
}

/*
 * Start our worker process.
 */
void
pgpm_start_worker(void)
{
	BackgroundWorker worker = {0};

	worker.bgw_flags = BGWORKER_SHMEM_ACCESS;
	worker.bgw_start_time = BgWorkerStart_ConsistentState;
	worker.bgw_restart_time = BGW_DEFAULT_RESTART_INTERVAL;
	strcpy(worker.bgw_library_name, "pg_plan_mgmt");
	strcpy(worker.bgw_function_name, "pg_plan_mgmt_worker_main");
	strcpy(worker.bgw_name, "pg_plan_mgmt worker");
	strcpy(worker.bgw_type, "pg_plan_mgmt worker");

	/*
	 * We only get here while processing shared_preload_libraries. In an
	 * EXEC_BACKEND child this silently does nothing, which is correct.
	 */
	RegisterBackgroundWorker(&worker);
}

/*
 * Search a list of DefElem objects for a given defname.
 */
static DefElem *
find_defelem_by_defname(List *deflist, const char *defname)
{
	foreach_node(DefElem, item, deflist)
	{
		if (strcmp(item->defname, defname) == 0)
			return item;
	}

	return NULL;
}

/*
 * Add the plan management decision, if any, to the EXPLAIN output.
 */
static void
pgpm_explain_per_plan(PlannedStmt *plannedstmt,
					  IntoClause *into,
					  ExplainState *es,
					  const char *queryString,
					  ParamListInfo params,
					  QueryEnvironment *queryEnv)
{
	DefElem    *pgpm_item;
	List	   *items;
	DefElem    *note;
	DefElem    *plan_hash;
	DefElem    *status;
	DefElem    *min_plan_hash;
	DefElem    *min_status;

	if (prev_explain_per_plan)
		prev_explain_per_plan(plannedstmt, into, es, queryString, params,
							  queryEnv);

	pgpm_item = find_defelem_by_defname(plannedstmt->extension_state,
										"pg_plan_mgmt");
	if (pgpm_item == NULL)
		return;
	items = (List *) pgpm_item->arg;

	note = find_defelem_by_defname(items, "note");
	plan_hash = find_defelem_by_defname(items, "plan_hash");
	status = find_defelem_by_defname(items, "status");
	min_plan_hash = find_defelem_by_defname(items, "min_cost_plan_hash");
	min_status = find_defelem_by_defname(items, "min_cost_status");

	if (note != NULL)
		ExplainPropertyText("Plan Management", strVal(note->arg), es);

	if (es->format == EXPLAIN_FORMAT_TEXT)
	{
		StringInfoData buf;

		initStringInfo(&buf);
		appendStringInfo(&buf, "%s (%s)",
						 strVal(plan_hash->arg), strVal(status->arg));
		if (min_plan_hash != NULL)
			appendStringInfo(&buf, ", Minimum-Cost Plan Hash: %s (%s)",
							 strVal(min_plan_hash->arg),
							 strVal(min_status->arg));
		ExplainPropertyText("Plan Hash", buf.data, es);
	}
	else
	{
		ExplainPropertyText("Plan Hash", strVal(plan_hash->arg), es);
		ExplainPropertyText("Plan Status", strVal(status->arg), es);
		if (min_plan_hash != NULL)
		{
			ExplainPropertyText("Minimum-Cost Plan Hash",
								strVal(min_plan_hash->arg), es);
			ExplainPropertyText("Minimum-Cost Plan Status",
								strVal(min_status->arg), es);
		}
	}
}
