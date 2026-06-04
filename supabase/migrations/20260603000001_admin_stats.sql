-- Admin dashboard aggregations.
--
-- Views below are read-only and exposed ONLY to service_role. The
-- admin panel hits them with the service_role key (which bypasses
-- RLS) to render the Dashboard tab. We explicitly revoke anon /
-- authenticated so an enabled "expose all tables" default in
-- Supabase can't accidentally let the public query them.
--
-- These views are owned by `postgres` and run as the owner (not
-- security_invoker) so the underlying-table RLS doesn't apply when
-- service_role queries them - that's fine because the revokes below
-- keep public roles out.

set search_path = public, extensions;

-- =============================================================
-- admin_daily_stats - per-day metrics for the last 90 days. The
-- generate_series CTE pins the row count to "today minus 90" rather
-- than letting gaps in the source tables produce gaps in the chart.
-- Downloads are aggregated from rate_limits (one row per user-day,
-- so summing gives total daily downloads). We extend the
-- rate_limits cleanup window to 90 days below so this column isn't
-- mostly zeros.
-- =============================================================

create view admin_daily_stats as
with days as (
    select generate_series(
        (current_date - interval '89 days')::date,
        current_date,
        interval '1 day'
    )::date as day
),
ups as (
    select created_at::date as day, count(*)::bigint as n
    from drills
    where created_at >= current_date - interval '90 days'
    group by 1
),
liks as (
    select created_at::date as day, count(*)::bigint as n
    from likes
    where created_at >= current_date - interval '90 days'
    group by 1
),
reps as (
    select created_at::date as day, count(*)::bigint as n
    from drill_reports
    where created_at >= current_date - interval '90 days'
    group by 1
),
dls as (
    select day, sum(downloads)::bigint as n
    from rate_limits
    where day >= current_date - interval '90 days'
    group by day
)
select
    d.day,
    coalesce(ups.n,  0) as uploads,
    coalesce(dls.n,  0) as downloads,
    coalesce(liks.n, 0) as likes,
    coalesce(reps.n, 0) as reports
from days d
left join ups  on ups.day  = d.day
left join liks on liks.day = d.day
left join reps on reps.day = d.day
left join dls  on dls.day  = d.day
order by d.day;

-- =============================================================
-- admin_character_counts - distribution of drills by character,
-- including flagged (admin wants to see those too) but excluding
-- removed. Sorted desc so the dashboard can `limit 10` for a Top-N
-- chart.
-- =============================================================

create view admin_character_counts as
select character, count(*)::bigint as n
from drills
where status in ('public', 'flagged')
group by character
order by count(*) desc;

-- =============================================================
-- admin_overview - one-row snapshot. Consolidates the four stat
-- cards we already had plus a few more (total downloads/likes,
-- distinct uploaders, removed count). One round-trip instead of
-- four parallel HEAD-count queries from the panel.
-- =============================================================

create view admin_overview as
select
    (select count(*)::bigint from drills)
        as total_drills,
    (select count(*)::bigint from drills where status = 'public')
        as public_drills,
    (select count(*)::bigint from drills where status = 'flagged')
        as flagged_drills,
    (select count(*)::bigint from drills where status = 'removed')
        as removed_drills,
    (select coalesce(sum(downloads), 0)::bigint from drills)
        as total_downloads,
    (select coalesce(sum(likes), 0)::bigint from drills)
        as total_likes,
    (select count(distinct uploader_id)::bigint from drills
        where uploader_id is not null)
        as distinct_uploaders,
    (select count(*)::bigint from user_bans)
        as banned_users,
    (select count(*)::bigint from drill_reports
        where created_at > now() - interval '24 hours')
        as reports_24h;

-- Belt-and-suspenders: by default Supabase exposes public views to
-- anon/authenticated via PostgREST. These views aggregate every
-- user's behavior, so they must stay admin-only.
revoke all on admin_daily_stats        from anon, authenticated;
revoke all on admin_character_counts   from anon, authenticated;
revoke all on admin_overview           from anon, authenticated;

-- service_role has BYPASSRLS and default privileges in Supabase,
-- but be explicit so a tightened default doesn't break the panel.
grant select on admin_daily_stats        to service_role;
grant select on admin_character_counts   to service_role;
grant select on admin_overview           to service_role;

-- =============================================================
-- Extend the rate_limits cleanup window to 90 days. Without this,
-- the cron job (defined in 20260528000001_initial_drills.sql) deletes
-- rows older than 7 days, which would leave admin_daily_stats.downloads
-- mostly zero. 90 days at ~5k DAU is ~450k rows * ~80 bytes = ~36 MB -
-- well within the Supabase free-tier 500 MB ceiling.
--
-- The schedule itself doesn't change; only the WHERE clause does.
-- pg_cron has no "alter" operation, so we unschedule + reschedule.
-- =============================================================

select cron.unschedule('opendojo-rate-limits-cleanup');
select cron.schedule(
    'opendojo-rate-limits-cleanup',
    '0 4 * * 1',
    $$ delete from public.rate_limits where day < current_date - 90 $$
);
