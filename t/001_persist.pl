# Copyright (c) 2026, PostgreSQL Global Development Group

# Plan baselines survive a restart, and the retention period deletes plans
# that were not used.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf('postgresql.conf', qq{
shared_preload_libraries = 'pg_plan_advice,pg_plan_mgmt'
compute_query_id = on
max_parallel_workers_per_gather = 0
pg_plan_mgmt.persist_interval = 0
});
$node->start;

$node->safe_psql('postgres', q{
CREATE EXTENSION pg_plan_mgmt;
CREATE TABLE t (id int PRIMARY KEY, v int);
INSERT INTO t SELECT g, g % 10 FROM generate_series(1, 1000) g;
ANALYZE t;
SET pg_plan_mgmt.capture_plan_baselines = manual;
SELECT count(*) FROM t WHERE id < 900;
SET enable_seqscan = off;
SELECT count(*) FROM t WHERE id < 900;
});

my $before = $node->safe_psql('postgres', q{
SELECT query_id, plan_no, status, enabled, origin, sql_text, plan_outline
  FROM plan_mgmt.plans ORDER BY plan_no});
is($node->safe_psql('postgres', 'SELECT count(*) FROM plan_mgmt.plans'),
	'2', 'two plans captured');

# Change something so we can tell it was persisted.
$node->safe_psql('postgres', q{
SELECT plan_mgmt.set_plan_status(query_id, plan_no,
		CASE status WHEN 'Approved' THEN 'Rejected' ELSE 'Preferred' END)
  FROM plan_mgmt.plans});
$before =~ s/\|Approved\|/|Rejected|/;
$before =~ s/\|Unapproved\|/|Preferred|/;

$node->restart;
ok(-f $node->data_dir . '/pg_plan_mgmt.tsv', 'dump file written at shutdown');

my $after = $node->safe_psql('postgres', q{
SELECT query_id, plan_no, status, enabled, origin, sql_text, plan_outline
  FROM plan_mgmt.plans ORDER BY plan_no});
is($after, $before, 'plans restored after restart');

# A restored Preferred plan is enforced.
my $plan = $node->safe_psql('postgres', q{
SET pg_plan_mgmt.use_plan_baselines = on;
EXPLAIN (COSTS OFF) SELECT count(*) FROM t WHERE id < 900});
like($plan, qr/Preferred plan used instead of minimum-cost plan/,
	'restored Preferred plan enforced');

# A corrupt dump file is reported and keeps the baselines locked.
$node->stop;
my $file = $node->data_dir . '/pg_plan_mgmt.tsv';
open(my $fh, '>>', $file) or die;
print $fh "bogus\tline\n";
close($fh);
my $log_offset = -s $node->logfile;
$node->start;
$node->wait_for_log(qr/syntax error in file "pg_plan_mgmt.tsv" line \d+: unrecognized line type/,
	$log_offset);
my ($ret, $stdout, $stderr) = $node->psql('postgres', q{
SELECT plan_mgmt.reset()});
like($stderr, qr/cannot be modified because "pg_plan_mgmt.tsv" has not been loaded yet/,
	'modifications locked out until the file loads');

# Fix the file; the restarted worker loads it.
$node->stop;
my $contents = slurp_file($file);
$contents =~ s/bogus\tline\n//;
open($fh, '>', $file) or die;
print $fh $contents;
close($fh);

# Make every plan look unused for 40 days, then let retention delete them.
$contents = slurp_file($file);
my $old = 0;    # the PostgreSQL epoch is far enough in the past
$contents =~ s/^(plan(?:\t[^\t]*){8})\t[^\t]*\t[^\t]*/$1\t$old\t0/mg;
$contents =~ s/^(stmt\t[^\t]*\t[^\t]*)\t[^\t]*/$1\t$old/mg;
open($fh, '>', $file) or die;
print $fh $contents;
close($fh);
$log_offset = -s $node->logfile;
$node->start;
$node->wait_for_log(qr/pg_plan_mgmt deleted 2 plans not used in the last 32 days/,
	$log_offset);
is($node->safe_psql('postgres', 'SELECT count(*) FROM plan_mgmt.plans'),
	'0', 'unused plans deleted');

$node->stop;
done_testing();
