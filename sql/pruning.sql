-- An approved outline is enforced only if the plan it produces regenerates
-- that outline. Plan-time partition pruning is the common way for an outline
-- not to apply: its items name partitions that are absent this time, so
-- pg_plan_advice reports no problems, but nothing was constrained.
CREATE TABLE pm_part (k int, v int) PARTITION BY LIST (k);
CREATE TABLE pm_part1 PARTITION OF pm_part FOR VALUES IN (1)
	WITH (autovacuum_enabled = false);
CREATE TABLE pm_part2 PARTITION OF pm_part FOR VALUES IN (2)
	WITH (autovacuum_enabled = false);
INSERT INTO pm_part SELECT 1 + g % 2, g % 100 FROM generate_series(1, 20000) g;
CREATE INDEX pm_part_v_idx ON pm_part (v);
VACUUM ANALYZE pm_part1, pm_part2;

SET pg_plan_mgmt.capture_plan_baselines = manual;
SELECT explain_masked('SELECT count(*) FROM pm_part WHERE k = 1 AND v = 1');
SET pg_plan_mgmt.capture_plan_baselines = off;
SELECT status, plan_outline FROM plan_mgmt.plans;

SET enable_indexscan = off;
SET enable_bitmapscan = off;
SET pg_plan_mgmt.use_plan_baselines = on;

-- Same partition: the approved plan is enforced.
SELECT explain_masked('SELECT count(*) FROM pm_part WHERE k = 1 AND v = 1');

-- Other partition: the outline produces a different plan, which must not be
-- passed off as the approved one.
SELECT explain_masked('SELECT count(*) FROM pm_part WHERE k = 2 AND v = 1');
SELECT status, plan_count, enforcement_failures FROM plan_mgmt.plans;

RESET enable_indexscan;
RESET enable_bitmapscan;
RESET pg_plan_mgmt.use_plan_baselines;
SELECT plan_mgmt.reset();
DROP TABLE pm_part;
