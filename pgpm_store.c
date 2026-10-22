/*-------------------------------------------------------------------------
 *
 * pgpm_store.c
 *	  shared memory store of plan baselines
 *
 * All statements live in a single dshash table keyed by (database OID, query
 * ID). Each entry owns a DSA-allocated array of plans. The dshash partition
 * lock of an entry protects the entry and its plan array; the per-plan
 * counters that are bumped during planning are atomics so that this only
 * needs the lock in shared mode.
 *
 * Functions used from the planner must never throw for lack of shared
 * memory, since that would fail the query being planned; they use the
 * NO_OOM variants and skip the work instead.
 *
 * Copyright (c) 2026, Duboce Labs, Inc. (pganalyze) <team@pganalyze.com>
 *
 *	  pgpm_store.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "pg_plan_mgmt.h"
#include "storage/dsm_registry.h"
#include "utils/memutils.h"

/* Shared memory hash table parameters */
static dshash_parameters pgpm_stmt_dshash_parameters = {
	sizeof(pgpm_stmt_key),
	sizeof(pgpm_stmt),
	dshash_memcmp,
	dshash_memhash,
	dshash_memcpy,
	LWTRANCHE_INVALID			/* gets set at runtime */
};

/* Shared memory pointers */
pgpm_shared_state *pgpm_state;
dsa_area   *pgpm_dsa_area;
dshash_table *pgpm_stmt_dshash;

static MemoryContext pgpm_mcxt;
static bool warned_limit = false;

static void pgpm_init_shared_state(void *ptr, void *arg);
static void pgpm_init_key(pgpm_stmt_key *key, Oid dbid, int64 queryId);
static pgpm_stmt *pgpm_insert_stmt(pgpm_stmt_key *key, const char *sql_text,
								   const char *search_path,
								   const char *role_name,
								   bool enforce_limits, bool *found);
static pgpm_plan *pgpm_stmt_plans(pgpm_stmt *stmt);
static int	pgpm_find_plan(pgpm_stmt *stmt, int plan_no);
static int	pgpm_find_plan_by_outline(pgpm_stmt *stmt, const char *outline);
static pgpm_plan *pgpm_append_plan(pgpm_stmt *stmt, int plan_no,
								   bool enforce_limits);
static void pgpm_remove_plan(pgpm_stmt *stmt, int idx);
static void pgpm_free_stmt(pgpm_stmt *stmt);
static dsa_pointer pgpm_dsa_strdup(const char *str);
static void pgpm_copy_stmt(pgpm_stmt *stmt, pgpm_stmt_info *info);
static void pgpm_warn_limit(const char *what, int limit);

/*
 * Attach to our structures in dynamic shared memory.
 *
 * Like pg_stash_advice, this is safe to retry after a failure partway
 * through.
 */
void
pgpm_attach(void)
{
	bool		found;
	MemoryContext oldcontext;

	if (pgpm_mcxt == NULL)
		pgpm_mcxt = AllocSetContextCreate(TopMemoryContext,
										  "pg_plan_mgmt",
										  ALLOCSET_DEFAULT_SIZES);
	oldcontext = MemoryContextSwitchTo(pgpm_mcxt);

	if (pgpm_state == NULL)
		pgpm_state = GetNamedDSMSegment("pg_plan_mgmt",
										sizeof(pgpm_shared_state),
										pgpm_init_shared_state,
										&found, NULL);

	if (pgpm_dsa_area == NULL)
	{
		dsa_handle	area_handle;

		LWLockAcquire(&pgpm_state->lock, LW_EXCLUSIVE);
		area_handle = pgpm_state->area;
		if (area_handle == DSA_HANDLE_INVALID)
		{
			pgpm_dsa_area = dsa_create(pgpm_state->dsa_tranche);
			dsa_pin(pgpm_dsa_area);
			pgpm_state->area = dsa_get_handle(pgpm_dsa_area);
			LWLockRelease(&pgpm_state->lock);
		}
		else
		{
			LWLockRelease(&pgpm_state->lock);
			pgpm_dsa_area = dsa_attach(area_handle);
		}
		dsa_pin_mapping(pgpm_dsa_area);
	}

	if (pgpm_stmt_dshash == NULL)
	{
		dshash_table_handle stmt_handle;

		LWLockAcquire(&pgpm_state->lock, LW_EXCLUSIVE);
		pgpm_stmt_dshash_parameters.tranche_id = pgpm_state->stmt_tranche;
		stmt_handle = pgpm_state->stmt_hash;
		if (stmt_handle == DSHASH_HANDLE_INVALID)
		{
			pgpm_stmt_dshash = dshash_create(pgpm_dsa_area,
											 &pgpm_stmt_dshash_parameters,
											 NULL);
			pgpm_state->stmt_hash =
				dshash_get_hash_table_handle(pgpm_stmt_dshash);
			LWLockRelease(&pgpm_state->lock);
		}
		else
		{
			LWLockRelease(&pgpm_state->lock);
			pgpm_stmt_dshash = dshash_attach(pgpm_dsa_area,
											 &pgpm_stmt_dshash_parameters,
											 stmt_handle, NULL);
		}
	}

	MemoryContextSwitchTo(oldcontext);
}

/*
 * Error out if the baselines have not been loaded from disk yet.
 */
void
pgpm_check_lockout(void)
{
	if (pg_atomic_unlocked_test_flag(&pgpm_state->plans_ready))
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("plan baselines cannot be modified because \"%s\" has not been loaded yet",
					   PGPM_DUMP_FILE));
}

/*
 * Return a backend-local copy of a statement and its plans, or NULL if the
 * statement is not known.
 */
pgpm_stmt_info *
pgpm_store_lookup(Oid dbid, int64 queryId)
{
	pgpm_stmt_key key;
	pgpm_stmt  *stmt;
	pgpm_stmt_info *info;

	pgpm_init_key(&key, dbid, queryId);
	stmt = dshash_find(pgpm_stmt_dshash, &key, false);
	if (stmt == NULL)
		return NULL;
	info = palloc_object(pgpm_stmt_info);
	pgpm_copy_stmt(stmt, info);
	dshash_release_lock(pgpm_stmt_dshash, stmt);

	return info;
}

/*
 * Return backend-local copies of all statements of the given database, or of
 * all databases if dbid is InvalidOid.
 */
List *
pgpm_store_get_all(Oid dbid)
{
	dshash_seq_status iter;
	pgpm_stmt  *stmt;
	List	   *result = NIL;

	dshash_seq_init(&iter, pgpm_stmt_dshash, false);
	while ((stmt = dshash_seq_next(&iter)) != NULL)
	{
		pgpm_stmt_info *info;

		if (OidIsValid(dbid) && stmt->key.dbid != dbid)
			continue;
		info = palloc_object(pgpm_stmt_info);
		pgpm_copy_stmt(stmt, info);
		result = lappend(result, info);
	}
	dshash_seq_term(&iter);

	return result;
}

/*
 * Record that a statement was planned and produced the given plan.
 *
 * The statement entry is created if necessary. The plan is added unless it
 * is already known, or the statement has no plans yet and has been planned
 * fewer than min_plannings times. The first plan of a statement is
 * Approved, later ones Unapproved. If the plan is (now) in the baseline,
 * *plan_no and *status are set to its number and status.
 *
 * Called from the planner, so this never throws for lack of shared memory.
 */
pgpm_capture_result
pgpm_store_capture(Oid dbid, int64 queryId, const char *sql_text,
				   const char *search_path, const char *role_name,
				   const char *outline, Cost total_cost, char origin,
				   int min_plannings, int *plan_no, char *status)
{
	pgpm_stmt_key key;
	pgpm_stmt  *stmt;
	pgpm_plan  *plan;
	bool		found;
	uint64		plannings;
	dsa_pointer outline_dp;
	int			idx;

	pgpm_init_key(&key, dbid, queryId);
	stmt = pgpm_insert_stmt(&key, sql_text, search_path, role_name, true,
							&found);
	if (stmt == NULL)
		return PGPM_CAPTURE_SKIPPED;
	if (!found)
		pg_atomic_add_fetch_u64(&pgpm_state->change_count, 1);

	plannings = pg_atomic_add_fetch_u64(&stmt->plannings, 1);

	/* Already known? */
	idx = pgpm_find_plan_by_outline(stmt, outline);
	if (idx >= 0)
	{
		*plan_no = pgpm_stmt_plans(stmt)[idx].plan_no;
		*status = pgpm_stmt_plans(stmt)[idx].status;
		dshash_release_lock(pgpm_stmt_dshash, stmt);
		return PGPM_CAPTURE_EXISTING;
	}

	/* Not planned often enough to be worth capturing yet? */
	if (stmt->nplans == 0 && plannings < min_plannings)
	{
		dshash_release_lock(pgpm_stmt_dshash, stmt);
		return PGPM_CAPTURE_SKIPPED;
	}

	outline_dp = pgpm_dsa_strdup(outline);
	if (!DsaPointerIsValid(outline_dp))
	{
		dshash_release_lock(pgpm_stmt_dshash, stmt);
		return PGPM_CAPTURE_SKIPPED;
	}
	plan = pgpm_append_plan(stmt, 0, true);
	if (plan == NULL)
	{
		dshash_release_lock(pgpm_stmt_dshash, stmt);
		dsa_free(pgpm_dsa_area, outline_dp);
		return PGPM_CAPTURE_SKIPPED;
	}

	plan->status = stmt->nplans == 1 ? PGPM_STATUS_APPROVED :
		PGPM_STATUS_UNAPPROVED;
	plan->origin = origin;
	plan->enabled = true;
	plan->valid = true;
	plan->total_cost = total_cost;
	plan->created = GetCurrentTimestamp();
	plan->outline = outline_dp;
	*plan_no = plan->plan_no;
	*status = plan->status;
	dshash_release_lock(pgpm_stmt_dshash, stmt);

	pg_atomic_add_fetch_u64(&pgpm_state->change_count, 1);
	return PGPM_CAPTURE_ADDED;
}

/*
 * Record that a plan was chosen during planning.
 */
void
pgpm_store_note_use(Oid dbid, int64 queryId, int plan_no)
{
	pgpm_stmt_key key;
	pgpm_stmt  *stmt;
	int			idx;

	pgpm_init_key(&key, dbid, queryId);
	stmt = dshash_find(pgpm_stmt_dshash, &key, false);
	if (stmt == NULL)
		return;
	idx = pgpm_find_plan(stmt, plan_no);
	if (idx >= 0)
	{
		pgpm_plan  *plan = &pgpm_stmt_plans(stmt)[idx];

		pg_atomic_add_fetch_u64(&plan->plan_count, 1);
		pg_atomic_write_u64(&plan->last_used,
							(uint64) GetCurrentTimestamp());
	}
	dshash_release_lock(pgpm_stmt_dshash, stmt);

	if (idx >= 0)
		pg_atomic_add_fetch_u64(&pgpm_state->change_count, 1);
}

/*
 * Record that a plan could not be enforced.
 */
void
pgpm_store_note_failure(Oid dbid, int64 queryId, int plan_no)
{
	pgpm_stmt_key key;
	pgpm_stmt  *stmt;
	int			idx;

	pgpm_init_key(&key, dbid, queryId);
	stmt = dshash_find(pgpm_stmt_dshash, &key, false);
	if (stmt == NULL)
		return;
	idx = pgpm_find_plan(stmt, plan_no);
	if (idx >= 0)
		pg_atomic_add_fetch_u64(&pgpm_stmt_plans(stmt)[idx].failures, 1);
	dshash_release_lock(pgpm_stmt_dshash, stmt);

	if (idx >= 0)
		pg_atomic_add_fetch_u64(&pgpm_state->change_count, 1);
}

/*
 * Change the status of a plan. Returns false if there is no such plan.
 */
bool
pgpm_store_set_status(Oid dbid, int64 queryId, int plan_no, char status)
{
	pgpm_stmt_key key;
	pgpm_stmt  *stmt;
	int			idx;

	pgpm_init_key(&key, dbid, queryId);
	stmt = dshash_find(pgpm_stmt_dshash, &key, true);
	if (stmt == NULL)
		return false;
	idx = pgpm_find_plan(stmt, plan_no);
	if (idx >= 0)
		pgpm_stmt_plans(stmt)[idx].status = status;
	dshash_release_lock(pgpm_stmt_dshash, stmt);

	if (idx < 0)
		return false;
	pg_atomic_add_fetch_u64(&pgpm_state->change_count, 1);
	return true;
}

/*
 * Enable or disable a plan. Returns false if there is no such plan.
 */
bool
pgpm_store_set_enabled(Oid dbid, int64 queryId, int plan_no, bool enabled)
{
	pgpm_stmt_key key;
	pgpm_stmt  *stmt;
	int			idx;

	pgpm_init_key(&key, dbid, queryId);
	stmt = dshash_find(pgpm_stmt_dshash, &key, true);
	if (stmt == NULL)
		return false;
	idx = pgpm_find_plan(stmt, plan_no);
	if (idx >= 0)
		pgpm_stmt_plans(stmt)[idx].enabled = enabled;
	dshash_release_lock(pgpm_stmt_dshash, stmt);

	if (idx < 0)
		return false;
	pg_atomic_add_fetch_u64(&pgpm_state->change_count, 1);
	return true;
}

/*
 * Record the outcome of validating a plan. Returns false if there is no
 * such plan.
 */
bool
pgpm_store_set_validation(Oid dbid, int64 queryId, int plan_no, bool valid,
						  Cost total_cost, TimestampTz when)
{
	pgpm_stmt_key key;
	pgpm_stmt  *stmt;
	int			idx;

	pgpm_init_key(&key, dbid, queryId);
	stmt = dshash_find(pgpm_stmt_dshash, &key, true);
	if (stmt == NULL)
		return false;
	idx = pgpm_find_plan(stmt, plan_no);
	if (idx >= 0)
	{
		pgpm_plan  *plan = &pgpm_stmt_plans(stmt)[idx];

		plan->valid = valid;
		if (valid)
			plan->total_cost = total_cost;
		plan->last_validated = when;
	}
	dshash_release_lock(pgpm_stmt_dshash, stmt);

	if (idx < 0)
		return false;
	pg_atomic_add_fetch_u64(&pgpm_state->change_count, 1);
	return true;
}

/*
 * Replace the outline of a plan. Returns false if there is no such plan, or
 * another plan of the statement already has that outline.
 */
bool
pgpm_store_set_outline(Oid dbid, int64 queryId, int plan_no,
					   const char *outline)
{
	pgpm_stmt_key key;
	pgpm_stmt  *stmt;
	dsa_pointer outline_dp;
	int			idx;
	bool		ok = false;

	outline_dp = pgpm_dsa_strdup(outline);
	if (!DsaPointerIsValid(outline_dp))
		ereport(ERROR,
				errcode(ERRCODE_OUT_OF_MEMORY),
				errmsg("out of memory"),
				errdetail("Could not allocate shared memory for a plan outline."));

	pgpm_init_key(&key, dbid, queryId);
	stmt = dshash_find(pgpm_stmt_dshash, &key, true);
	if (stmt == NULL)
	{
		dsa_free(pgpm_dsa_area, outline_dp);
		return false;
	}
	idx = pgpm_find_plan(stmt, plan_no);
	if (idx >= 0)
	{
		int			other = pgpm_find_plan_by_outline(stmt, outline);

		if (other < 0 || other == idx)
		{
			pgpm_plan  *plan = &pgpm_stmt_plans(stmt)[idx];

			dsa_free(pgpm_dsa_area, plan->outline);
			plan->outline = outline_dp;
			ok = true;
		}
	}
	dshash_release_lock(pgpm_stmt_dshash, stmt);

	if (!ok)
	{
		dsa_free(pgpm_dsa_area, outline_dp);
		return false;
	}
	pg_atomic_add_fetch_u64(&pgpm_state->change_count, 1);
	return true;
}

/*
 * Delete one plan, or all plans of a statement if all_plans is true.
 * A statement left without plans is deleted as well. Returns the number of
 * plans deleted.
 */
int
pgpm_store_delete(Oid dbid, int64 queryId, int plan_no, bool all_plans)
{
	pgpm_stmt_key key;
	pgpm_stmt  *stmt;
	int			ndeleted = 0;

	pgpm_init_key(&key, dbid, queryId);
	stmt = dshash_find(pgpm_stmt_dshash, &key, true);
	if (stmt == NULL)
		return 0;

	if (all_plans)
		ndeleted = stmt->nplans;
	else
	{
		int			idx = pgpm_find_plan(stmt, plan_no);

		if (idx >= 0)
		{
			pgpm_remove_plan(stmt, idx);
			ndeleted = 1;
		}
	}

	if (all_plans || (ndeleted > 0 && stmt->nplans == 0))
	{
		pgpm_free_stmt(stmt);
		dshash_delete_entry(pgpm_stmt_dshash, stmt);
	}
	else
		dshash_release_lock(pgpm_stmt_dshash, stmt);

	pg_atomic_add_fetch_u64(&pgpm_state->change_count, 1);
	return ndeleted;
}

/*
 * Create a statement entry, or update the existing one. An existing entry
 * keeps its text and environment.
 *
 * Unlike pgpm_store_capture, this throws errors; it's used by SQL functions
 * and when restoring from disk.
 */
void
pgpm_store_put_stmt(pgpm_stmt_key *key, const char *sql_text,
					const char *search_path, const char *role_name,
					TimestampTz created, uint64 plannings, int next_plan_no,
					bool enforce_limits)
{
	pgpm_stmt  *stmt;
	bool		found;

	stmt = pgpm_insert_stmt(key, sql_text, search_path, role_name,
							enforce_limits, &found);
	if (stmt == NULL)
		ereport(ERROR,
				errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				errmsg("could not store statement"),
				errdetail("pg_plan_mgmt.max_statements (%d) was reached or shared memory is exhausted.",
						  pgpm_max_statements));
	if (created != 0)
		stmt->created = created;
	if (plannings > pg_atomic_read_u64(&stmt->plannings))
		pg_atomic_write_u64(&stmt->plannings, plannings);
	if (next_plan_no > stmt->next_plan_no)
		stmt->next_plan_no = next_plan_no;
	dshash_release_lock(pgpm_stmt_dshash, stmt);

	pg_atomic_add_fetch_u64(&pgpm_state->change_count, 1);
}

/*
 * Store a plan for an existing statement. If the statement already has a
 * plan with the same outline, that plan is replaced and keeps its number;
 * otherwise the plan is added under info->plan_no if that is set (restoring
 * from disk), else under the next free number. On success info->plan_no is
 * the number the plan ended up with. Returns false if the statement does not
 * exist or a limit was hit.
 */
bool
pgpm_store_put_plan(pgpm_stmt_key *key, pgpm_plan_info *info,
					bool enforce_limits)
{
	pgpm_stmt  *stmt;
	pgpm_plan  *plan;
	dsa_pointer outline_dp;
	int			idx;

	outline_dp = pgpm_dsa_strdup(info->outline);
	if (!DsaPointerIsValid(outline_dp))
		ereport(ERROR,
				errcode(ERRCODE_OUT_OF_MEMORY),
				errmsg("out of memory"),
				errdetail("Could not allocate shared memory for a plan outline."));

	stmt = dshash_find(pgpm_stmt_dshash, key, true);
	if (stmt == NULL)
	{
		dsa_free(pgpm_dsa_area, outline_dp);
		return false;
	}

	idx = pgpm_find_plan_by_outline(stmt, info->outline);
	if (idx >= 0)
	{
		plan = &pgpm_stmt_plans(stmt)[idx];
		dsa_free(pgpm_dsa_area, plan->outline);
	}
	else if ((plan = pgpm_append_plan(stmt, info->plan_no,
									  enforce_limits)) == NULL)
	{
		dshash_release_lock(pgpm_stmt_dshash, stmt);
		dsa_free(pgpm_dsa_area, outline_dp);
		return false;
	}

	info->plan_no = plan->plan_no;
	plan->status = info->status;
	plan->origin = info->origin;
	plan->enabled = info->enabled;
	plan->valid = info->valid;
	plan->total_cost = info->total_cost;
	plan->created = info->created;
	plan->last_validated = info->last_validated;
	pg_atomic_write_u64(&plan->last_used, (uint64) info->last_used);
	pg_atomic_write_u64(&plan->plan_count, info->plan_count);
	pg_atomic_write_u64(&plan->failures, info->failures);
	plan->outline = outline_dp;
	dshash_release_lock(pgpm_stmt_dshash, stmt);

	pg_atomic_add_fetch_u64(&pgpm_state->change_count, 1);
	return true;
}

/*
 * Delete all statements of the given database, or of all databases if dbid
 * is InvalidOid.
 */
void
pgpm_store_reset(Oid dbid)
{
	dshash_seq_status iter;
	pgpm_stmt  *stmt;

	dshash_seq_init(&iter, pgpm_stmt_dshash, true);
	while ((stmt = dshash_seq_next(&iter)) != NULL)
	{
		if (OidIsValid(dbid) && stmt->key.dbid != dbid)
			continue;
		pgpm_free_stmt(stmt);
		dshash_delete_current(&iter);
	}
	dshash_seq_term(&iter);

	pg_atomic_add_fetch_u64(&pgpm_state->change_count, 1);
}

/*
 * Delete plans that have not been used since cutoff (or, if never used,
 * were created before it), and statements left without plans that were
 * created before it. Returns the number of plans deleted.
 */
int
pgpm_store_purge(TimestampTz cutoff)
{
	dshash_seq_status iter;
	pgpm_stmt  *stmt;
	int			ndeleted = 0;
	bool		changed = false;

	dshash_seq_init(&iter, pgpm_stmt_dshash, true);
	while ((stmt = dshash_seq_next(&iter)) != NULL)
	{
		int			i = 0;

		while (i < stmt->nplans)
		{
			pgpm_plan  *plan = &pgpm_stmt_plans(stmt)[i];
			TimestampTz last_used;

			last_used = (TimestampTz) pg_atomic_read_u64(&plan->last_used);
			if (last_used == 0)
				last_used = plan->created;
			if (last_used < cutoff)
			{
				pgpm_remove_plan(stmt, i);
				ndeleted++;
				changed = true;
			}
			else
				i++;
		}

		if (stmt->nplans == 0 && stmt->created < cutoff)
		{
			pgpm_free_stmt(stmt);
			dshash_delete_current(&iter);
			changed = true;
		}
	}
	dshash_seq_term(&iter);

	if (changed)
		pg_atomic_add_fetch_u64(&pgpm_state->change_count, 1);
	return ndeleted;
}

/*
 * Initialize shared state when first created.
 */
static void
pgpm_init_shared_state(void *ptr, void *arg)
{
	pgpm_shared_state *state = (pgpm_shared_state *) ptr;

	LWLockInitialize(&state->lock, LWLockNewTrancheId("pg_plan_mgmt_lock"));
	state->dsa_tranche = LWLockNewTrancheId("pg_plan_mgmt_dsa");
	state->stmt_tranche = LWLockNewTrancheId("pg_plan_mgmt_stmt");
	state->area = DSA_HANDLE_INVALID;
	state->stmt_hash = DSHASH_HANDLE_INVALID;
	state->bgworker_pid = InvalidPid;
	pg_atomic_init_flag(&state->plans_ready);
	pg_atomic_init_u64(&state->change_count, 0);
	pg_atomic_init_u32(&state->nstatements, 0);
	pg_atomic_init_u32(&state->nplans, 0);

	/*
	 * If persistence is on, the worker sets plans_ready once it has loaded
	 * the dump file. Otherwise there's nothing to wait for. (When not loaded
	 * via shared_preload_libraries, pgpm_persist is always false.)
	 */
	if (!pgpm_persist)
		pg_atomic_test_set_flag(&state->plans_ready);
}

static void
pgpm_init_key(pgpm_stmt_key *key, Oid dbid, int64 queryId)
{
	memset(key, 0, sizeof(pgpm_stmt_key));
	key->dbid = dbid;
	key->queryId = queryId;
}

/*
 * Find or create a statement entry and return it locked exclusively, or
 * return NULL if it would exceed max_statements (when enforce_limits) or
 * shared memory is exhausted. The strings may be NULL, meaning empty.
 */
static pgpm_stmt *
pgpm_insert_stmt(pgpm_stmt_key *key, const char *sql_text,
				 const char *search_path, const char *role_name,
				 bool enforce_limits, bool *found)
{
	pgpm_stmt  *stmt;
	dsa_pointer sql_dp = InvalidDsaPointer;
	dsa_pointer path_dp = InvalidDsaPointer;
	dsa_pointer role_dp = InvalidDsaPointer;

	stmt = dshash_find(pgpm_stmt_dshash, key, true);
	if (stmt != NULL)
	{
		*found = true;
		return stmt;
	}

	/* Reserve a slot. */
	if (pg_atomic_fetch_add_u32(&pgpm_state->nstatements, 1) >=
		(uint32) pgpm_max_statements && enforce_limits)
	{
		pg_atomic_fetch_sub_u32(&pgpm_state->nstatements, 1);
		pgpm_warn_limit("pg_plan_mgmt.max_statements", pgpm_max_statements);
		return NULL;
	}

	sql_dp = pgpm_dsa_strdup(sql_text ? sql_text : "");
	if (DsaPointerIsValid(sql_dp))
		path_dp = pgpm_dsa_strdup(search_path ? search_path : "");
	if (DsaPointerIsValid(path_dp))
		role_dp = pgpm_dsa_strdup(role_name ? role_name : "");
	if (DsaPointerIsValid(role_dp))
		stmt = dshash_find_or_insert_extended(pgpm_stmt_dshash, key, found,
											  DSHASH_INSERT_NO_OOM);
	else
		stmt = NULL;

	if (stmt == NULL || *found)
	{
		/* Out of memory, or somebody else inserted it concurrently. */
		pg_atomic_fetch_sub_u32(&pgpm_state->nstatements, 1);
		if (DsaPointerIsValid(sql_dp))
			dsa_free(pgpm_dsa_area, sql_dp);
		if (DsaPointerIsValid(path_dp))
			dsa_free(pgpm_dsa_area, path_dp);
		if (DsaPointerIsValid(role_dp))
			dsa_free(pgpm_dsa_area, role_dp);
		return stmt;
	}

	stmt->sql_text = sql_dp;
	stmt->search_path = path_dp;
	stmt->role_name = role_dp;
	stmt->created = GetCurrentTimestamp();
	pg_atomic_init_u64(&stmt->plannings, 0);
	stmt->next_plan_no = 1;
	stmt->nplans = 0;
	stmt->maxplans = 0;
	stmt->plans = InvalidDsaPointer;
	return stmt;
}

static pgpm_plan *
pgpm_stmt_plans(pgpm_stmt *stmt)
{
	if (stmt->nplans == 0)
		return NULL;
	return (pgpm_plan *) dsa_get_address(pgpm_dsa_area, stmt->plans);
}

static int
pgpm_find_plan(pgpm_stmt *stmt, int plan_no)
{
	pgpm_plan  *plans = pgpm_stmt_plans(stmt);

	for (int i = 0; i < stmt->nplans; i++)
	{
		if (plans[i].plan_no == plan_no)
			return i;
	}
	return -1;
}

static int
pgpm_find_plan_by_outline(pgpm_stmt *stmt, const char *outline)
{
	pgpm_plan  *plans = pgpm_stmt_plans(stmt);

	for (int i = 0; i < stmt->nplans; i++)
	{
		if (strcmp(dsa_get_address(pgpm_dsa_area, plans[i].outline),
				   outline) == 0)
			return i;
	}
	return -1;
}

/*
 * Make room for one more plan and return a pointer to it, zero-initialized
 * apart from the atomics and the plan number. The plan gets number plan_no
 * if that is positive (the caller must know it is unused), else the next
 * free one; either way the statement's counter moves past it. Returns NULL
 * if that would exceed max_plans (when enforce_limits) or shared memory is
 * exhausted. The caller must hold the entry lock exclusively.
 */
static pgpm_plan *
pgpm_append_plan(pgpm_stmt *stmt, int plan_no, bool enforce_limits)
{
	pgpm_plan  *plan;

	if (pg_atomic_fetch_add_u32(&pgpm_state->nplans, 1) >=
		(uint32) pgpm_max_plans && enforce_limits)
	{
		pg_atomic_fetch_sub_u32(&pgpm_state->nplans, 1);
		pgpm_warn_limit("pg_plan_mgmt.max_plans", pgpm_max_plans);
		return NULL;
	}

	if (stmt->nplans >= stmt->maxplans)
	{
		int			newmax = Max(4, stmt->maxplans * 2);
		dsa_pointer newdp;

		newdp = dsa_allocate_extended(pgpm_dsa_area,
									  newmax * sizeof(pgpm_plan),
									  DSA_ALLOC_NO_OOM);
		if (!DsaPointerIsValid(newdp))
		{
			pg_atomic_fetch_sub_u32(&pgpm_state->nplans, 1);
			return NULL;
		}
		if (stmt->nplans > 0)
		{
			memcpy(dsa_get_address(pgpm_dsa_area, newdp),
				   dsa_get_address(pgpm_dsa_area, stmt->plans),
				   stmt->nplans * sizeof(pgpm_plan));
		}
		if (DsaPointerIsValid(stmt->plans))
			dsa_free(pgpm_dsa_area, stmt->plans);
		stmt->plans = newdp;
		stmt->maxplans = newmax;
	}

	plan = &((pgpm_plan *) dsa_get_address(pgpm_dsa_area,
											stmt->plans))[stmt->nplans++];
	memset(plan, 0, sizeof(pgpm_plan));
	if (plan_no <= 0)
		plan_no = stmt->next_plan_no;
	plan->plan_no = plan_no;
	if (plan_no >= stmt->next_plan_no)
		stmt->next_plan_no = plan_no + 1;
	pg_atomic_init_u64(&plan->last_used, 0);
	pg_atomic_init_u64(&plan->plan_count, 0);
	pg_atomic_init_u64(&plan->failures, 0);
	plan->outline = InvalidDsaPointer;
	return plan;
}

/*
 * Remove the plan at index idx. The caller must hold the entry lock
 * exclusively.
 */
static void
pgpm_remove_plan(pgpm_stmt *stmt, int idx)
{
	pgpm_plan  *plans = pgpm_stmt_plans(stmt);

	if (DsaPointerIsValid(plans[idx].outline))
		dsa_free(pgpm_dsa_area, plans[idx].outline);
	memmove(&plans[idx], &plans[idx + 1],
			(stmt->nplans - idx - 1) * sizeof(pgpm_plan));
	stmt->nplans--;
	pg_atomic_fetch_sub_u32(&pgpm_state->nplans, 1);
}

/*
 * Free everything a statement entry owns, prior to deleting the entry.
 */
static void
pgpm_free_stmt(pgpm_stmt *stmt)
{
	pgpm_plan  *plans = pgpm_stmt_plans(stmt);

	for (int i = 0; i < stmt->nplans; i++)
	{
		if (DsaPointerIsValid(plans[i].outline))
			dsa_free(pgpm_dsa_area, plans[i].outline);
	}
	pg_atomic_fetch_sub_u32(&pgpm_state->nplans, stmt->nplans);
	pg_atomic_fetch_sub_u32(&pgpm_state->nstatements, 1);
	if (DsaPointerIsValid(stmt->plans))
		dsa_free(pgpm_dsa_area, stmt->plans);
	if (DsaPointerIsValid(stmt->sql_text))
		dsa_free(pgpm_dsa_area, stmt->sql_text);
	if (DsaPointerIsValid(stmt->search_path))
		dsa_free(pgpm_dsa_area, stmt->search_path);
	if (DsaPointerIsValid(stmt->role_name))
		dsa_free(pgpm_dsa_area, stmt->role_name);
	stmt->nplans = 0;
	stmt->plans = InvalidDsaPointer;
	stmt->sql_text = InvalidDsaPointer;
	stmt->search_path = InvalidDsaPointer;
	stmt->role_name = InvalidDsaPointer;
}

/*
 * Copy a string into DSA memory. Returns InvalidDsaPointer if out of memory.
 */
static dsa_pointer
pgpm_dsa_strdup(const char *str)
{
	size_t		len = strlen(str) + 1;
	dsa_pointer dp;

	dp = dsa_allocate_extended(pgpm_dsa_area, len, DSA_ALLOC_NO_OOM);
	if (DsaPointerIsValid(dp))
		memcpy(dsa_get_address(pgpm_dsa_area, dp), str, len);
	return dp;
}

/*
 * Copy a statement entry and its plans into backend-local memory.
 */
static void
pgpm_copy_stmt(pgpm_stmt *stmt, pgpm_stmt_info *info)
{
	pgpm_plan  *plans = pgpm_stmt_plans(stmt);

	info->key = stmt->key;
	info->sql_text = pstrdup(dsa_get_address(pgpm_dsa_area, stmt->sql_text));
	info->search_path = pstrdup(dsa_get_address(pgpm_dsa_area,
												stmt->search_path));
	info->role_name = pstrdup(dsa_get_address(pgpm_dsa_area,
											  stmt->role_name));
	info->created = stmt->created;
	info->plannings = pg_atomic_read_u64(&stmt->plannings);
	info->next_plan_no = stmt->next_plan_no;
	info->nplans = stmt->nplans;
	info->plans = stmt->nplans == 0 ? NULL :
		palloc_array(pgpm_plan_info, stmt->nplans);

	for (int i = 0; i < stmt->nplans; i++)
	{
		pgpm_plan  *p = &plans[i];
		pgpm_plan_info *pi = &info->plans[i];

		pi->plan_no = p->plan_no;
		pi->status = p->status;
		pi->origin = p->origin;
		pi->enabled = p->enabled;
		pi->valid = p->valid;
		pi->total_cost = p->total_cost;
		pi->created = p->created;
		pi->last_used = (TimestampTz) pg_atomic_read_u64(&p->last_used);
		pi->last_validated = p->last_validated;
		pi->plan_count = pg_atomic_read_u64(&p->plan_count);
		pi->failures = pg_atomic_read_u64(&p->failures);
		pi->outline = pstrdup(dsa_get_address(pgpm_dsa_area, p->outline));
	}
}

/*
 * Warn, once per backend, that capture stopped because of a limit.
 */
static void
pgpm_warn_limit(const char *what, int limit)
{
	if (warned_limit)
		return;
	warned_limit = true;
	ereport(WARNING,
			errmsg("pg_plan_mgmt is not capturing new plans because %s (%d) was reached",
				   what, limit));
}
