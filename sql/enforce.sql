SET pg_plan_mgmt.capture_plan_baselines = manual;

-- Capture the default plan (Approved) and an index-heavy one (Unapproved).
SELECT explain_masked('SELECT * FROM pm_fact f JOIN pm_dim d ON f.dim_id = d.id WHERE d.val = 1');
SET random_page_cost = 0.1;
SET cpu_index_tuple_cost = 0;
SELECT explain_masked('SELECT * FROM pm_fact f JOIN pm_dim d ON f.dim_id = d.id WHERE d.val = 1');
SET pg_plan_mgmt.capture_plan_baselines = off;

SELECT query_id AS qid FROM plan_mgmt.plans LIMIT 1 \gset
SELECT plan_no AS approved FROM plan_mgmt.plans WHERE status = 'Approved' \gset
SELECT plan_no AS unapproved FROM plan_mgmt.plans WHERE status = 'Unapproved' \gset

-- Under the changed costs, the Approved plan is enforced.
SET pg_plan_mgmt.use_plan_baselines = on;
SELECT explain_masked('SELECT * FROM pm_fact f JOIN pm_dim d ON f.dim_id = d.id WHERE d.val = 1');
SELECT count(*) FROM pm_fact f JOIN pm_dim d ON f.dim_id = d.id WHERE d.val = 1;

-- The same decision in a structured format.
DO $$
DECLARE
	j jsonb;
BEGIN
	EXECUTE 'EXPLAIN (COSTS OFF, FORMAT JSON) SELECT * FROM pm_fact f JOIN pm_dim d ON f.dim_id = d.id WHERE d.val = 1' INTO j;
	RAISE NOTICE '%', jsonb_pretty((j -> 0) - 'Plan' - 'Supplied Plan Advice');
END;
$$;

-- A cheap enough Unapproved plan may run.
SET pg_plan_mgmt.unapproved_plan_execution_threshold = 1e9;
SELECT explain_masked('SELECT * FROM pm_fact f JOIN pm_dim d ON f.dim_id = d.id WHERE d.val = 1');
RESET pg_plan_mgmt.unapproved_plan_execution_threshold;

-- Once approved, the minimum-cost plan is used as is.
SELECT plan_mgmt.set_plan_status(:qid, :unapproved, 'approved');
SELECT explain_masked('SELECT * FROM pm_fact f JOIN pm_dim d ON f.dim_id = d.id WHERE d.val = 1');

-- A Preferred plan wins over the minimum-cost plan unless that is approved.
SELECT plan_mgmt.set_plan_status(:qid, :unapproved, 'Rejected');
SELECT plan_mgmt.set_plan_status(:qid, :approved, 'Preferred');
SELECT explain_masked('SELECT * FROM pm_fact f JOIN pm_dim d ON f.dim_id = d.id WHERE d.val = 1');

-- Disabled plans are not used.
SELECT plan_mgmt.set_plan_enabled(:qid, :approved, false);
SELECT explain_masked('SELECT * FROM pm_fact f JOIN pm_dim d ON f.dim_id = d.id WHERE d.val = 1');
SELECT plan_mgmt.set_plan_enabled(:qid, :approved, true);

-- Prepared statements have their own query ID, and so their own baseline.
PREPARE pm_stmt(int) AS
	SELECT * FROM pm_fact f JOIN pm_dim d ON f.dim_id = d.id WHERE d.val = $1;
SELECT explain_masked('EXECUTE pm_stmt(1)');
SELECT plan_mgmt.add_plan(
	'SELECT * FROM pm_fact f JOIN pm_dim d ON f.dim_id = d.id WHERE d.val = $1',
	plan_outline, 'Approved') IS NOT NULL
  FROM plan_mgmt.plans WHERE plan_no = :approved;
SELECT explain_masked('EXECUTE pm_stmt(1)');
DEALLOCATE pm_stmt;

-- If the approved plans can't be enforced any more, fall back.
SELECT plan_mgmt.set_plan_status(:qid, :approved, 'Rejected');
SELECT plan_mgmt.set_plan_status(:qid, :unapproved, 'Preferred');
SELECT explain_masked('SELECT * FROM pm_fact f JOIN pm_dim d ON f.dim_id = d.id WHERE d.val = 1');
DROP INDEX pm_fact_dim_id_idx;
SELECT explain_masked('SELECT * FROM pm_fact f JOIN pm_dim d ON f.dim_id = d.id WHERE d.val = 1');
SELECT status, enforcement_failures > 0 AS failed FROM plan_mgmt.plans
 WHERE query_id = :qid ORDER BY status;
CREATE INDEX pm_fact_dim_id_idx ON pm_fact (dim_id);

RESET random_page_cost;
RESET cpu_index_tuple_cost;
RESET pg_plan_mgmt.use_plan_baselines;
SELECT plan_mgmt.reset();
