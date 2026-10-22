/*-------------------------------------------------------------------------
 *
 * pgpm_persist.c
 *	  background worker: persistence and plan retention
 *
 * The worker loads the dump file at startup, writes it periodically when
 * something changed and once more at shutdown, and deletes plans that have
 * not been used for pg_plan_mgmt.plan_retention_period days. The approach
 * and file handling follow contrib/pg_stash_advice.
 *
 * The file is TSV, starting with a header line, then one line per
 * statement followed by one line per plan:
 *
 *   pg_plan_mgmt	1
 *   stmt	dbid	query_id	created	plannings	sql_text
 *   plan	dbid	query_id	plan_hash	status	origin	enabled	valid	cost
 *		created	last_used	last_validated	plan_count	failures	outline
 *
 * Copyright (c) 2026, Duboce Labs, Inc. (pganalyze) <team@pganalyze.com>
 *
 *	  pgpm_persist.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>

#include "miscadmin.h"
#include "pg_plan_mgmt.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "utils/backend_status.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#include "utils/wait_event.h"

#define PGPM_FILE_HEADER	"pg_plan_mgmt\t1"
#define PGPM_STMT_FIELDS	6
#define PGPM_PLAN_FIELDS	15

/* Delete unused plans at most this often. */
#define PGPM_PURGE_INTERVAL_MS	(3600 * 1000)

/*
 * A parsed line, with pointers into the slurp buffer.
 */
typedef struct pgpm_saved_line
{
	bool		is_plan;
	pgpm_stmt_key key;
	char	   *sql_text;
	TimestampTz created;
	uint64		plannings;
	pgpm_plan_info plan;
} pgpm_saved_line;

extern PGDLLEXPORT void pg_plan_mgmt_worker_main(Datum main_arg);
static void pgpm_detach_shmem(int code, Datum arg);
static void pgpm_read_from_disk(void);
static void pgpm_write_to_disk(void);
static void pgpm_purge(void);
static int	pgpm_split_line(char *line, char **fields, int maxfields);
static int64 pgpm_parse_int64(const char *str, unsigned lineno);
static bool pgpm_parse_bool(const char *str, unsigned lineno);
static void pgpm_append_tsv_escaped_string(StringInfo buf, const char *str);
static void pgpm_unescape_tsv_field(char *str, unsigned lineno);
pg_noreturn static void pgpm_syntax_error(unsigned lineno, const char *what);

/*
 * Background worker entry point.
 */
PGDLLEXPORT void
pg_plan_mgmt_worker_main(Datum main_arg)
{
	uint64		last_change_count;
	TimestampTz last_write_time = 0;
	TimestampTz last_purge_time = 0;

	pqsignal(SIGTERM, SignalHandlerForShutdownRequest);
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	BackgroundWorkerUnblockSignals();

	ereport(DEBUG1, errmsg("pg_plan_mgmt worker started"));

	InitializeSessionUserIdStandalone();
	pgstat_beinit();
	pgstat_bestart_initial();
	pgstat_bestart_final();

	pgpm_attach();
	before_shmem_exit(pgpm_detach_shmem, 0);

	LWLockAcquire(&pgpm_state->lock, LW_EXCLUSIVE);
	if (pgpm_state->bgworker_pid != InvalidPid)
	{
		LWLockRelease(&pgpm_state->lock);
		ereport(LOG,
				errmsg("pg_plan_mgmt worker is already running under PID %d",
					   (int) pgpm_state->bgworker_pid));
		return;
	}
	pgpm_state->bgworker_pid = MyProcPid;
	LWLockRelease(&pgpm_state->lock);

	if (pg_atomic_unlocked_test_flag(&pgpm_state->plans_ready))
	{
		pgpm_read_from_disk();
		pg_atomic_test_set_flag(&pgpm_state->plans_ready);
	}

	last_change_count = pg_atomic_read_u64(&pgpm_state->change_count);

	while (!ShutdownRequestPending)
	{
		TimestampTz now;
		long		delay_in_ms = PGPM_PURGE_INTERVAL_MS;

		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		now = GetCurrentTimestamp();

		/* Delete unused plans, if it's time. */
		if (pgpm_plan_retention_period > 0 &&
			TimestampDifferenceExceeds(last_purge_time, now,
									   PGPM_PURGE_INTERVAL_MS))
		{
			pgpm_purge();
			last_purge_time = now;
		}

		/* Write to disk, if it's time and something has changed. */
		if (pgpm_persist_interval > 0)
		{
			TimestampTz next_write_time;

			next_write_time =
				TimestampTzPlusMilliseconds(last_write_time,
											pgpm_persist_interval * 1000);
			if (now >= next_write_time)
			{
				uint64		current_change_count;

				current_change_count =
					pg_atomic_read_u64(&pgpm_state->change_count);
				if (current_change_count != last_change_count)
				{
					pgpm_write_to_disk();
					last_change_count = current_change_count;
				}
				last_write_time = now;
				next_write_time =
					TimestampTzPlusMilliseconds(now,
												pgpm_persist_interval * 1000);
			}
			delay_in_ms = Min(delay_in_ms,
							  TimestampDifferenceMilliseconds(now,
															  next_write_time));
		}

		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 Max(delay_in_ms, 1),
						 PG_WAIT_EXTENSION);
		ResetLatch(MyLatch);
	}

	/* Write one last time before exiting. */
	pgpm_write_to_disk();
}

/*
 * Clear our PID from shared memory on exit.
 */
static void
pgpm_detach_shmem(int code, Datum arg)
{
	LWLockAcquire(&pgpm_state->lock, LW_EXCLUSIVE);
	if (pgpm_state->bgworker_pid == MyProcPid)
		pgpm_state->bgworker_pid = InvalidPid;
	LWLockRelease(&pgpm_state->lock);
}

/*
 * Delete plans not used within the retention period.
 */
static void
pgpm_purge(void)
{
	TimestampTz cutoff;
	int			ndeleted;

	cutoff = TimestampTzPlusMilliseconds(GetCurrentTimestamp(),
										 -((int64) pgpm_plan_retention_period) *
										 86400 * 1000);
	ndeleted = pgpm_store_purge(cutoff);
	if (ndeleted > 0)
		ereport(LOG,
				errmsg("pg_plan_mgmt deleted %d plans not used in the last %d days",
					   ndeleted, pgpm_plan_retention_period));
}

/*
 * Load plan baselines from the dump file, if there is one.
 *
 * As in pg_stash_advice, the whole file is parsed before anything is applied
 * to shared memory, and anything already in shared memory (say, from a
 * previous failed attempt) is cleared first.
 */
static void
pgpm_read_from_disk(void)
{
	struct stat statbuf;
	FILE	   *file;
	char	   *filebuf;
	size_t		nread;
	char	   *p;
	unsigned	lineno;
	pgpm_saved_line *lines;
	int			num_lines = 0;
	int			max_lines = 64;
	int			num_stmts = 0;
	int			num_plans = 0;
	MemoryContext tmpcxt;
	MemoryContext oldcxt;

	pgpm_store_reset(InvalidOid);

	file = AllocateFile(PGPM_DUMP_FILE, "r");
	if (!file)
	{
		if (errno == ENOENT)
			return;
		ereport(ERROR,
				errcode_for_file_access(),
				errmsg("could not open file \"%s\": %m", PGPM_DUMP_FILE));
	}

	tmpcxt = AllocSetContextCreate(CurrentMemoryContext,
								   "pg_plan_mgmt load",
								   ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(tmpcxt);

	if (fstat(fileno(file), &statbuf) != 0)
		ereport(ERROR,
				errcode_for_file_access(),
				errmsg("could not stat file \"%s\": %m", PGPM_DUMP_FILE));

	filebuf = palloc_extended(statbuf.st_size + 1, MCXT_ALLOC_HUGE);
	nread = fread(filebuf, 1, statbuf.st_size, file);
	if (ferror(file))
		ereport(ERROR,
				errcode_for_file_access(),
				errmsg("could not read file \"%s\": %m", PGPM_DUMP_FILE));
	FreeFile(file);
	filebuf[nread] = '\0';

	lines = palloc_array(pgpm_saved_line, max_lines);

	for (p = filebuf, lineno = 1; *p != '\0'; lineno++)
	{
		char	   *line = p;
		char	   *eol;
		char	   *fields[PGPM_PLAN_FIELDS + 1];
		int			nfields;
		pgpm_saved_line *sl;

		eol = strchr(p, '\n');
		if (eol != NULL)
		{
			*eol = '\0';
			p = eol + 1;
			if (eol > line && eol[-1] == '\r')
				eol[-1] = '\0';
		}
		else
			p += strlen(p);

		if (lineno == 1)
		{
			if (strcmp(line, PGPM_FILE_HEADER) != 0)
				pgpm_syntax_error(lineno, "unrecognized file header");
			continue;
		}
		if (*line == '\0')
			continue;

		if (num_lines >= max_lines)
		{
			max_lines *= 2;
			lines = repalloc_array(lines, pgpm_saved_line, max_lines);
		}
		sl = &lines[num_lines];
		memset(sl, 0, sizeof(pgpm_saved_line));

		nfields = pgpm_split_line(line, fields, lengthof(fields));
		if (strcmp(fields[0], "stmt") == 0)
		{
			if (nfields != PGPM_STMT_FIELDS)
				pgpm_syntax_error(lineno, "wrong number of fields");
			sl->is_plan = false;
			sl->key.dbid = (Oid) pgpm_parse_int64(fields[1], lineno);
			sl->key.queryId = pgpm_parse_int64(fields[2], lineno);
			sl->created = (TimestampTz) pgpm_parse_int64(fields[3], lineno);
			sl->plannings = (uint64) pgpm_parse_int64(fields[4], lineno);
			pgpm_unescape_tsv_field(fields[5], lineno);
			sl->sql_text = fields[5];
			num_stmts++;
		}
		else if (strcmp(fields[0], "plan") == 0)
		{
			pgpm_plan_info *plan = &sl->plan;
			char	   *endptr;

			if (nfields != PGPM_PLAN_FIELDS)
				pgpm_syntax_error(lineno, "wrong number of fields");
			if (num_lines == 0)
				pgpm_syntax_error(lineno, "plan without statement");
			sl->is_plan = true;
			sl->key.dbid = (Oid) pgpm_parse_int64(fields[1], lineno);
			sl->key.queryId = pgpm_parse_int64(fields[2], lineno);
			plan->plan_hash = pgpm_parse_int64(fields[3], lineno);
			plan->status = fields[4][0];
			if (strlen(fields[4]) != 1 ||
				strcmp(pgpm_status_name(plan->status), "Unknown") == 0)
				pgpm_syntax_error(lineno, "invalid status");
			plan->origin = fields[5][0];
			if (strlen(fields[5]) != 1 ||
				strcmp(pgpm_origin_name(plan->origin), "Unknown") == 0)
				pgpm_syntax_error(lineno, "invalid origin");
			plan->enabled = pgpm_parse_bool(fields[6], lineno);
			plan->valid = pgpm_parse_bool(fields[7], lineno);
			errno = 0;
			plan->total_cost = strtod(fields[8], &endptr);
			if (*endptr != '\0' || errno != 0 || endptr == fields[8])
				pgpm_syntax_error(lineno, "invalid cost");
			plan->created = (TimestampTz) pgpm_parse_int64(fields[9], lineno);
			plan->last_used = (TimestampTz) pgpm_parse_int64(fields[10], lineno);
			plan->last_validated =
				(TimestampTz) pgpm_parse_int64(fields[11], lineno);
			plan->plan_count = (uint64) pgpm_parse_int64(fields[12], lineno);
			plan->failures = (uint64) pgpm_parse_int64(fields[13], lineno);
			pgpm_unescape_tsv_field(fields[14], lineno);
			plan->outline = fields[14];
			num_plans++;
		}
		else
			pgpm_syntax_error(lineno, "unrecognized line type");

		num_lines++;
	}

	/*
	 * Parsing succeeded, so apply everything. Limits are not enforced here:
	 * these plans were within limits when they were stored.
	 */
	for (int i = 0; i < num_lines; i++)
	{
		pgpm_saved_line *sl = &lines[i];

		if (!sl->is_plan)
			pgpm_store_put_stmt(&sl->key, sl->sql_text, sl->created,
								sl->plannings, false);
		else if (!pgpm_store_put_plan(&sl->key, &sl->plan, false))
			ereport(ERROR,
					errcode(ERRCODE_DATA_CORRUPTED),
					errmsg("plan %" PRId64 " in file \"%s\" belongs to unknown statement %" PRId64,
						   sl->plan.plan_hash, PGPM_DUMP_FILE,
						   sl->key.queryId));
	}

	ereport(LOG,
			errmsg("loaded %d statements and %d plans from \"%s\"",
				   num_stmts, num_plans, PGPM_DUMP_FILE));

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(tmpcxt);
}

/*
 * Write all plan baselines to disk.
 */
static void
pgpm_write_to_disk(void)
{
	char		pathname[MAXPGPATH];
	FILE	   *file;
	StringInfoData buf;
	List	   *stmts;
	int			num_stmts = 0;
	int			num_plans = 0;
	MemoryContext tmpcxt;
	MemoryContext oldcxt;

	tmpcxt = AllocSetContextCreate(CurrentMemoryContext,
								   "pg_plan_mgmt dump",
								   ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(tmpcxt);

	/* Take a snapshot first, so we don't hold locks while writing. */
	stmts = pgpm_store_get_all(InvalidOid);

	snprintf(pathname, MAXPGPATH, "%s.tmp", PGPM_DUMP_FILE);
	file = AllocateFile(pathname, "w");
	if (!file)
		ereport(ERROR,
				errcode_for_file_access(),
				errmsg("could not open file \"%s\": %m", pathname));

	initStringInfo(&buf);
	appendStringInfoString(&buf, PGPM_FILE_HEADER "\n");

	foreach_ptr(pgpm_stmt_info, stmt, stmts)
	{
		appendStringInfo(&buf, "stmt\t%u\t%" PRId64 "\t%" PRId64 "\t%" PRIu64 "\t",
						 stmt->key.dbid, stmt->key.queryId,
						 (int64) stmt->created, stmt->plannings);
		pgpm_append_tsv_escaped_string(&buf, stmt->sql_text);
		appendStringInfoChar(&buf, '\n');
		num_stmts++;

		for (int i = 0; i < stmt->nplans; i++)
		{
			pgpm_plan_info *plan = &stmt->plans[i];

			appendStringInfo(&buf,
							 "plan\t%u\t%" PRId64 "\t%" PRId64 "\t%c\t%c\t%c\t%c\t%.17g\t%" PRId64 "\t%" PRId64 "\t%" PRId64 "\t%" PRIu64 "\t%" PRIu64 "\t",
							 stmt->key.dbid, stmt->key.queryId,
							 plan->plan_hash, plan->status, plan->origin,
							 plan->enabled ? 't' : 'f',
							 plan->valid ? 't' : 'f',
							 plan->total_cost, (int64) plan->created,
							 (int64) plan->last_used,
							 (int64) plan->last_validated,
							 plan->plan_count, plan->failures);
			pgpm_append_tsv_escaped_string(&buf, plan->outline);
			appendStringInfoChar(&buf, '\n');
			num_plans++;
		}

		/* Flush now and then so the buffer doesn't get huge. */
		if (buf.len > 65536)
		{
			if (fwrite(buf.data, 1, buf.len, file) != buf.len)
				goto write_error;
			resetStringInfo(&buf);
		}
	}

	if (fwrite(buf.data, 1, buf.len, file) != buf.len)
		goto write_error;

	if (FreeFile(file) != 0)
	{
		file = NULL;
		goto write_error;
	}
	(void) durable_rename(pathname, PGPM_DUMP_FILE, ERROR);

	ereport(DEBUG1,
			errmsg("saved %d statements and %d plans to \"%s\"",
				   num_stmts, num_plans, PGPM_DUMP_FILE));

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(tmpcxt);
	return;

write_error:
	{
		int			save_errno = errno;

		if (file != NULL)
			FreeFile(file);
		unlink(pathname);
		errno = save_errno;
		ereport(ERROR,
				errcode_for_file_access(),
				errmsg("could not write file \"%s\": %m", pathname));
	}
}

/*
 * Split a line into tab-separated fields in place. Returns the number of
 * fields found, which may exceed maxfields; at most maxfields are stored.
 */
static int
pgpm_split_line(char *line, char **fields, int maxfields)
{
	int			n = 0;

	for (;;)
	{
		char	   *tab = strchr(line, '\t');

		if (n < maxfields)
			fields[n] = line;
		n++;
		if (tab == NULL)
			break;
		*tab = '\0';
		line = tab + 1;
	}
	return n;
}

static int64
pgpm_parse_int64(const char *str, unsigned lineno)
{
	char	   *endptr;
	int64		result;

	errno = 0;
	result = strtoi64(str, &endptr, 10);
	if (*endptr != '\0' || errno != 0 || endptr == str)
		pgpm_syntax_error(lineno, "invalid integer");
	return result;
}

static bool
pgpm_parse_bool(const char *str, unsigned lineno)
{
	if (strcmp(str, "t") == 0)
		return true;
	if (strcmp(str, "f") == 0)
		return false;
	pgpm_syntax_error(lineno, "invalid boolean");
}

/*
 * Append the TSV-escaped form of str to buf.
 */
static void
pgpm_append_tsv_escaped_string(StringInfo buf, const char *str)
{
	for (const char *p = str; *p != '\0'; p++)
	{
		switch (*p)
		{
			case '\\':
				appendStringInfoString(buf, "\\\\");
				break;
			case '\t':
				appendStringInfoString(buf, "\\t");
				break;
			case '\n':
				appendStringInfoString(buf, "\\n");
				break;
			case '\r':
				appendStringInfoString(buf, "\\r");
				break;
			default:
				appendStringInfoChar(buf, *p);
				break;
		}
	}
}

/*
 * Unescape a TSV field in place.
 */
static void
pgpm_unescape_tsv_field(char *str, unsigned lineno)
{
	char	   *src = str;
	char	   *dst = str;

	while (*src != '\0')
	{
		if (likely(*src != '\\'))
		{
			*dst++ = *src++;
			continue;
		}

		switch (src[1])
		{
			case '\\':
				*dst++ = '\\';
				break;
			case 't':
				*dst++ = '\t';
				break;
			case 'n':
				*dst++ = '\n';
				break;
			case 'r':
				*dst++ = '\r';
				break;
			default:
				pgpm_syntax_error(lineno, "invalid escape sequence");
		}
		src += 2;
	}
	*dst = '\0';
}

static void
pgpm_syntax_error(unsigned lineno, const char *what)
{
	ereport(ERROR,
			errcode(ERRCODE_DATA_CORRUPTED),
			errmsg("syntax error in file \"%s\" line %u: %s",
				   PGPM_DUMP_FILE, lineno, what));
}
