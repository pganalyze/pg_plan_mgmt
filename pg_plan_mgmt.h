/*-------------------------------------------------------------------------
 *
 * pg_plan_mgmt.h
 *	  main header for pg_plan_mgmt
 *
 * pg_plan_mgmt maintains a plan baseline for each managed statement: every
 * plan the optimizer produced for it, each with a status that decides
 * whether it may be used. Plans are identified by the advice string
 * pg_plan_advice generates for them (the "outline") and are enforced by
 * supplying that outline back to pg_plan_advice.
 *
 * Copyright (c) 2026, Duboce Labs, Inc. (pganalyze) <team@pganalyze.com>
 *
 *	  pg_plan_mgmt.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_PLAN_MGMT_H
#define PG_PLAN_MGMT_H

#include "commands/explain_state.h"
#include "lib/dshash.h"
#include "nodes/params.h"
#include "nodes/parsenodes.h"
#include "nodes/pathnodes.h"
#include "nodes/plannodes.h"
#include "port/atomics.h"
#include "storage/lwlock.h"
#include "utils/timestamp.h"

#define PGPM_DUMP_FILE		"pg_plan_mgmt.tsv"

/* Plan status, stored as a single character. */
#define PGPM_STATUS_APPROVED	'a'
#define PGPM_STATUS_UNAPPROVED	'u'
#define PGPM_STATUS_PREFERRED	'p'
#define PGPM_STATUS_REJECTED	'r'

/* Plan origin, stored as a single character. */
#define PGPM_ORIGIN_MANUAL		'm'
#define PGPM_ORIGIN_AUTOMATIC	'a'
#define PGPM_ORIGIN_USER		'u'

/* Values of pg_plan_mgmt.capture_plan_baselines */
typedef enum
{
	PGPM_CAPTURE_OFF,
	PGPM_CAPTURE_MANUAL,
	PGPM_CAPTURE_AUTOMATIC
} pgpm_capture_mode;

/*
 * Key of a managed statement.
 */
typedef struct pgpm_stmt_key
{
	Oid			dbid;
	int64		queryId;
} pgpm_stmt_key;

/*
 * A plan in shared memory. Plans live in a DSA-allocated array owned by
 * their statement entry. The atomics are bumped during planning while only
 * holding the entry's lock in shared mode; everything else is modified only
 * while holding it exclusively.
 */
typedef struct pgpm_plan
{
	int64		plan_hash;
	char		status;
	char		origin;
	bool		enabled;
	bool		valid;
	Cost		total_cost;
	TimestampTz created;
	TimestampTz last_validated;
	pg_atomic_uint64 last_used;
	pg_atomic_uint64 plan_count;
	pg_atomic_uint64 failures;
	dsa_pointer outline;
} pgpm_plan;

/*
 * A managed statement in shared memory.
 *
 * In automatic capture mode a statement entry is created the first time a
 * statement is planned, with no plans; "plannings" counts how often it was
 * planned so that capture can begin with the second planning.
 */
typedef struct pgpm_stmt
{
	pgpm_stmt_key key;
	dsa_pointer sql_text;
	TimestampTz created;
	pg_atomic_uint64 plannings;
	int			nplans;
	int			maxplans;
	dsa_pointer plans;
} pgpm_stmt;

/*
 * Top-level shared state.
 */
typedef struct pgpm_shared_state
{
	LWLock		lock;
	int			dsa_tranche;
	int			stmt_tranche;
	dsa_handle	area;
	dshash_table_handle stmt_hash;
	pid_t		bgworker_pid;
	pg_atomic_flag plans_ready;
	pg_atomic_uint64 change_count;
	pg_atomic_uint32 nstatements;
	pg_atomic_uint32 nplans;
} pgpm_shared_state;

/*
 * Backend-local copy of a plan.
 */
typedef struct pgpm_plan_info
{
	int64		plan_hash;
	char		status;
	char		origin;
	bool		enabled;
	bool		valid;
	Cost		total_cost;
	TimestampTz created;
	TimestampTz last_used;		/* 0 if never used */
	TimestampTz last_validated; /* 0 if never validated */
	uint64		plan_count;
	uint64		failures;
	char	   *outline;
} pgpm_plan_info;

/*
 * Backend-local copy of a statement and its plans.
 */
typedef struct pgpm_stmt_info
{
	pgpm_stmt_key key;
	char	   *sql_text;
	TimestampTz created;
	uint64		plannings;
	int			nplans;
	pgpm_plan_info *plans;
} pgpm_stmt_info;

/*
 * Result of capturing a plan.
 */
typedef enum
{
	PGPM_CAPTURE_SKIPPED,		/* not captured (sightings, limits, OOM) */
	PGPM_CAPTURE_EXISTING,		/* plan was already in the baseline */
	PGPM_CAPTURE_ADDED			/* plan was added */
} pgpm_capture_result;

/*
 * Result of planning a query once, optionally with a supplied outline.
 */
typedef struct pgpm_planned
{
	PlannedStmt *pstmt;
	char	   *outline;		/* generated advice for the plan, or NULL */
	int64		plan_hash;		/* hash of outline, 0 if no outline */
	bool		clean;			/* supplied outline was fully enforced */
	char	   *problems;		/* if not clean, description of why */
} pgpm_planned;

/* Shared memory pointers */
extern pgpm_shared_state *pgpm_state;
extern dsa_area *pgpm_dsa_area;
extern dshash_table *pgpm_stmt_dshash;

/* GUC variables */
extern int	pgpm_capture_plan_baselines;
extern bool pgpm_use_plan_baselines;
extern double pgpm_unapproved_plan_execution_threshold;
extern int	pgpm_max_plans;
extern int	pgpm_max_statements;
extern int	pgpm_plan_retention_period;
extern bool pgpm_persist;
extern int	pgpm_persist_interval;

/* pg_plan_mgmt.c */
extern void (*pgpm_request_advice_generation) (bool activate);
extern const char *pgpm_status_name(char status);
extern char pgpm_parse_status(const char *name);
extern const char *pgpm_origin_name(char origin);
extern char pgpm_parse_origin(const char *name);
extern int64 pgpm_hash_outline(const char *outline);
extern void pgpm_start_worker(void);

/* pgpm_planner.c */
extern void pgpm_planner_install_hooks(void);
extern char *pgpm_advisor(PlannerGlobal *glob, Query *parse,
						  const char *query_string, int cursorOptions,
						  ExplainState *es);
extern void pgpm_plan_once(Query *parse, const char *query_string,
						   int cursorOptions, ParamListInfo boundParams,
						   ExplainState *es, char *outline,
						   pgpm_planned *result);
extern bool pgpm_query_is_manageable(Query *parse);
extern const char *pgpm_skip_explain(const char *sql_text);

/* pgpm_store.c */
extern void pgpm_attach(void);
extern void pgpm_check_lockout(void);
extern pgpm_stmt_info *pgpm_store_lookup(Oid dbid, int64 queryId);
extern List *pgpm_store_get_all(Oid dbid);
extern pgpm_capture_result pgpm_store_capture(Oid dbid, int64 queryId,
											  const char *sql_text,
											  int64 plan_hash,
											  const char *outline,
											  Cost total_cost, char origin,
											  int min_plannings,
											  char *status);
extern void pgpm_store_note_use(Oid dbid, int64 queryId, int64 plan_hash);
extern void pgpm_store_note_failure(Oid dbid, int64 queryId, int64 plan_hash);
extern bool pgpm_store_set_status(Oid dbid, int64 queryId, int64 plan_hash,
								  char status);
extern bool pgpm_store_set_enabled(Oid dbid, int64 queryId, int64 plan_hash,
								   bool enabled);
extern bool pgpm_store_set_validation(Oid dbid, int64 queryId,
									  int64 plan_hash, bool valid,
									  Cost total_cost, TimestampTz when);
extern int	pgpm_store_delete(Oid dbid, int64 queryId, int64 plan_hash,
							  bool all_plans);
extern void pgpm_store_put_stmt(pgpm_stmt_key *key, const char *sql_text,
								TimestampTz created, uint64 plannings,
								bool enforce_limits);
extern bool pgpm_store_put_plan(pgpm_stmt_key *key, pgpm_plan_info *plan,
								bool enforce_limits);
extern void pgpm_store_reset(Oid dbid);
extern int	pgpm_store_purge(TimestampTz cutoff);

#endif
