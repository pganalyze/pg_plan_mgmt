/* pg_plan_mgmt--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_plan_mgmt" to load this file. \quit

CREATE FUNCTION @extschema@.get_plans(
	OUT dbid oid,
	OUT query_id bigint,
	OUT plan_hash bigint,
	OUT status text,
	OUT enabled boolean,
	OUT valid boolean,
	OUT origin text,
	OUT sql_text text,
	OUT plan_outline text,
	OUT estimated_total_cost float8,
	OUT created timestamptz,
	OUT last_used timestamptz,
	OUT last_validated timestamptz,
	OUT plan_count bigint,
	OUT enforcement_failures bigint
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_plan_mgmt_get_plans'
LANGUAGE C STRICT;

CREATE VIEW @extschema@.dba_plans AS
	SELECT d.datname, p.*
	  FROM @extschema@.get_plans() p
	  LEFT JOIN pg_catalog.pg_database d ON d.oid = p.dbid;

CREATE FUNCTION @extschema@.set_plan_status(query_id bigint, plan_hash bigint,
											status text)
RETURNS void
AS 'MODULE_PATHNAME', 'pg_plan_mgmt_set_plan_status'
LANGUAGE C STRICT;

CREATE FUNCTION @extschema@.set_plan_enabled(query_id bigint, plan_hash bigint,
											 enabled boolean)
RETURNS void
AS 'MODULE_PATHNAME', 'pg_plan_mgmt_set_plan_enabled'
LANGUAGE C STRICT;

CREATE FUNCTION @extschema@.delete_plan(query_id bigint,
										plan_hash bigint DEFAULT NULL)
RETURNS integer
AS 'MODULE_PATHNAME', 'pg_plan_mgmt_delete_plan'
LANGUAGE C;

CREATE FUNCTION @extschema@.add_plan(
	sql_text text,
	plan_outline text,
	status text DEFAULT 'Approved',
	enabled boolean DEFAULT true,
	OUT query_id bigint,
	OUT plan_hash bigint
)
AS 'MODULE_PATHNAME', 'pg_plan_mgmt_add_plan'
LANGUAGE C;

CREATE FUNCTION @extschema@.validate_plans(
	action text DEFAULT 'update',
	OUT query_id bigint,
	OUT plan_hash bigint,
	OUT valid boolean,
	OUT estimated_total_cost float8,
	OUT detail text
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_plan_mgmt_validate_plans'
LANGUAGE C STRICT;

CREATE FUNCTION @extschema@.reset()
RETURNS void
AS 'MODULE_PATHNAME', 'pg_plan_mgmt_reset'
LANGUAGE C STRICT;

REVOKE ALL ON FUNCTION @extschema@.get_plans() FROM PUBLIC;
REVOKE ALL ON @extschema@.dba_plans FROM PUBLIC;
REVOKE ALL ON FUNCTION @extschema@.set_plan_status(bigint, bigint, text) FROM PUBLIC;
REVOKE ALL ON FUNCTION @extschema@.set_plan_enabled(bigint, bigint, boolean) FROM PUBLIC;
REVOKE ALL ON FUNCTION @extschema@.delete_plan(bigint, bigint) FROM PUBLIC;
REVOKE ALL ON FUNCTION @extschema@.add_plan(text, text, text, boolean) FROM PUBLIC;
REVOKE ALL ON FUNCTION @extschema@.validate_plans(text) FROM PUBLIC;
REVOKE ALL ON FUNCTION @extschema@.reset() FROM PUBLIC;
