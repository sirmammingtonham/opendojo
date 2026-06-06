-- OpenDojo Cloud — initial schema.
--
-- Design notes:
--   * Drill text is stored inline in the `drills.content` column. Drills
--     average a few KB; Postgres TOAST handles large rows transparently
--     and lz/zstd-compresses them, so inline TEXT is cheaper than a blob
--     store at this size.
--   * No client role ever SELECTs from `drills` directly. The only read
--     paths exposed to `anon`/`authenticated` are the `drill_summaries`
--     view (metadata only, no content) and the `get_drill` SECURITY
--     DEFINER function (single-row read; bumps the lifetime download count).
--   * No client role ever writes to `drills`. The only write path is
--     the `submit_drill` Edge Function, which uses the service-role key
--     and validates in TypeScript before inserting.
--   * The database keeps no per-user rate-limit counters; `daily_stats`
--     below is a metrics-only rollup, never read to gate a request.

set search_path = public, extensions;

-- pg_cron is preinstalled in Supabase; it lives in the `extensions`
-- schema. We use it for the nightly stats rollup.
create extension if not exists pg_cron with schema extensions;
create extension if not exists pgcrypto;  -- for gen_random_uuid + sha256

-- =============================================================
-- drills
-- =============================================================

-- Allowed values for the categories array and the difficulty enum are
-- modeled as small lookup tables. We *could* hard-code the strings in
-- a CHECK constraint, but a table lets us tweak the menu without a
-- migration to alter constraints. The Edge Function and the mod both
-- validate against this set defensively.
create table drill_categories (
    id    text primary key,
    label text not null  -- shown in the UI
);
insert into drill_categories (id, label) values
    ('reaction',       'Reaction'),
    ('option_select',  'Option Select'),
    ('fuzzy_guard',    'Fuzzy Guard'),
    ('punishment',     'Punishment'),
    ('throw_break',    'Throw Break');

create table drill_difficulties (
    id    text primary key,
    label text not null,
    sort_order int not null
);
insert into drill_difficulties (id, label, sort_order) values
    ('beginner',     'Beginner',     1),
    ('intermediate', 'Intermediate', 2),
    ('advanced',     'Advanced',     3);

create table drills (
    id                uuid primary key default gen_random_uuid(),

    -- Human-facing fields. `name` is required; the rest are best-effort.
    name              text not null check (char_length(name) between 1 and 96),
    -- Description allows newlines and a few paragraphs. The UI uses a
    -- multi-line input; clients are not expected to enforce the cap,
    -- the server is.
    description       text check (char_length(description) <= 1000),
    character         text not null check (char_length(character) between 1 and 32),
    cpu_side          text check (cpu_side in ('p1', 'p2', '')),
    recordings_count  int  not null check (recordings_count between 1 and 8),
    author_handle     text check (char_length(author_handle) <= 32),

    -- Taxonomy. categories is a small array (0..5) of drill_categories.id;
    -- difficulty is a single optional drill_difficulties.id. Validation
    -- via subset check against the lookup tables happens in the Edge
    -- Function; the column-level CHECKs below are belt-and-suspenders.
    categories        text[] not null default '{}'
                      check (
                          array_length(categories, 1) is null
                          or array_length(categories, 1) <= 5
                      ),
    difficulty        text references drill_difficulties(id) on delete set null,

    -- Tekken patch version this drill was recorded against. The slot
    -- format and movelist IDs are version-sensitive — drills don't
    -- cleanly cross-load between versions. The mod fills this from a
    -- compile-time constant (OPENDOJO_TEKKEN_VERSION).
    game_version      text check (char_length(game_version) <= 24),

    -- The actual drill payload. Inline TEXT; Postgres TOAST handles
    -- compression + out-of-line storage. Hard-capped at 64 KB so a
    -- single row can't be a denial-of-service vector.
    content           text not null check (octet_length(content) <= 65536),
    size_bytes        int  not null check (size_bytes <= 65536),

    -- SHA-256 of content, stored as hex. Unique index below quietly
    -- kills duplicate uploads — the Edge Function reads the existing
    -- id and returns it instead of failing the user.
    content_hash      text not null check (char_length(content_hash) = 64),

    uploader_id       uuid references auth.users(id) on delete set null,
    downloads         bigint not null default 0,

    -- Denormalized like count. The `likes` table is the source of
    -- truth; this column is kept in sync by the toggle_like RPC so
    -- the Browse tab can sort by popularity without a join.
    likes             bigint not null default 0,

    -- Denormalized report count. The `drill_reports` table is the
    -- source of truth; report_drill bumps this column. Crossing the
    -- threshold (10) auto-flips status to 'flagged' so enough reports
    -- hide the drill without admin intervention.
    reports           bigint not null default 0,

    -- 'public': visible to everyone.
    -- 'flagged': hidden from listings but still readable by id (so the
    --   moderation queue UI can preview it).
    -- 'removed': fully invisible to clients.
    status            text not null default 'public'
                      check (status in ('public', 'flagged', 'removed')),

    created_at        timestamptz not null default now(),

    -- Full-text search vector built from human-facing fields only.
    -- The event-line content is low-information and adding it to the
    -- index would just balloon noise. Generated/stored so writes
    -- precompute it once.
    search_tsv        tsvector generated always as (
        to_tsvector(
            'simple',
            coalesce(name,          '') || ' ' ||
            coalesce(description,   '') || ' ' ||
            coalesce(character,     '') || ' ' ||
            coalesce(author_handle, '')
        )
    ) stored
);

-- Lookup indices.
create unique index drills_content_hash_idx on drills (content_hash);
create        index drills_browse_idx       on drills (character, status, created_at desc);
create        index drills_top_idx          on drills (status, downloads desc);
create        index drills_likes_idx        on drills (status, likes desc);
create        index drills_reports_idx      on drills (status, reports desc);
create        index drills_uploader_idx     on drills (uploader_id);
create        index drills_search_idx       on drills using gin (search_tsv);
create        index drills_categories_idx   on drills using gin (categories);
create        index drills_difficulty_idx   on drills (difficulty);
create        index drills_version_idx      on drills (game_version);

-- =============================================================
-- likes — one row per (user, drill) the user has liked. The
-- (user_id, drill_id) primary key prevents double-liking. The
-- denormalized drills.likes counter is maintained by toggle_like.
-- =============================================================

create table likes (
    user_id    uuid not null references auth.users(id) on delete cascade,
    drill_id   uuid not null references drills(id)     on delete cascade,
    created_at timestamptz not null default now(),
    primary key (user_id, drill_id)
);
create index likes_drill_idx on likes (drill_id);

-- =============================================================
-- user_bans — moderation. Presence of a row blocks the user from
-- the upload path (the submit_drill Edge Function consults this
-- table before inserting). Reads / likes are not affected; the
-- goal is to stop new harmful content, not nuke the account.
--
-- Only the service_role role can read/write this table — the
-- admin panel hits it directly with the secret key. The
-- Edge Function consults it via its service-role client.
-- =============================================================

create table user_bans (
    user_id    uuid primary key references auth.users(id) on delete cascade,
    reason     text,
    banned_at  timestamptz not null default now()
);

alter table user_bans enable row level security;

-- =============================================================
-- drill_reports — user-submitted complaints about a drill. Used
-- both to surface bad content to the admin panel and to auto-hide
-- drills that pile up enough reports.
--
-- Composite (drill_id, user_id) PK means one report per user per
-- drill — a single offended user can't inflate the count by
-- spamming. report_drill is the only write path; admin reads via
-- service_role. Anon/authenticated cannot read this table directly
-- (would leak who reported whom).
-- =============================================================

create table drill_reports (
    drill_id   uuid not null references drills(id)     on delete cascade,
    user_id    uuid not null references auth.users(id) on delete cascade,
    reason     text check (char_length(reason) <= 240),
    created_at timestamptz not null default now(),
    primary key (drill_id, user_id)
);
create index drill_reports_drill_idx on drill_reports (drill_id);
create index drill_reports_recent_idx on drill_reports (created_at desc);

alter table drill_reports enable row level security;
-- No client policies; service_role bypasses RLS for the admin panel.
-- No policies and no grants to anon/authenticated — service_role
-- bypasses RLS, so the admin panel works while clients can't touch
-- the table even with the publishable key.

-- =============================================================
-- drill_summaries view — the ONLY shape anon/authenticated can list.
-- Excludes the `content` column, so even a hostile client iterating
-- the whole view cannot pull drill bodies in bulk.
-- =============================================================

create view drill_summaries
-- Owner-rights view (security_invoker = false): it runs as the view owner, so
-- it can read `drills` to build this curated, metadata-only projection without
-- anon having ANY access to the base table. anon gets SELECT on this view only;
-- `content` and `uploader_id` never leave it (uploader_id is read solely to
-- compute is_mine). Direct anon access to `drills` stays closed by RLS + the
-- revokes below. (A security_invoker view would require granting anon access to
-- `drills` itself, which would expose content + uploader_id — so it's wrong here.)
with (security_invoker = false)
as
select
    id,
    name,
    description,
    character,
    cpu_side,
    recordings_count,
    author_handle,
    categories,
    difficulty,
    game_version,
    size_bytes,
    downloads,
    likes,
    -- "Is this drill mine?" computed from the calling user's JWT.
    -- We don't expose `uploader_id` itself (that would let any client
    -- correlate drills by uploader); just the boolean for the UI.
    (uploader_id = auth.uid()) as is_mine,
    created_at
from drills
where status = 'public';

-- =============================================================
-- daily_stats — PERMANENT, metrics-only per-day rollup. One row per active
-- day holds the counts the dashboard charts (uploads / likes / reports).
-- Never pruned and never read to gate a request: it's tiny (~365 rows/year)
-- and exists purely so historical counts survive later deletion of
-- drills / likes / reports. The nightly job freezes each completed day from
-- the live tables' created_at.
--
-- Downloads are NOT tracked per-day — only the lifetime running total in
-- drills.downloads (surfaced as admin_overview.total_downloads). That keeps
-- get_drill a pure read + a single counter bump, with no extra write.
-- =============================================================

create table daily_stats (
    day      date   primary key,
    uploads  bigint not null default 0,
    likes    bigint not null default 0,
    reports  bigint not null default 0
);

-- run_daily_maintenance — freeze completed days' counts into daily_stats so
-- they survive later deletions. Idempotent and self-healing:
--   * Only completed days (day < today) are frozen; today stays live.
--   * A day already in daily_stats is never recomputed (NOT EXISTS +
--     ON CONFLICT DO NOTHING), so later deletions can't rewrite history.
--   * Zero-activity days are skipped (the view gap-fills them as 0).
--   * The 14-day window backfills any night the job didn't run (e.g. a
--     free-tier project that was asleep).
-- Internal only: SECURITY DEFINER, execute revoked from the public roles
-- below — the pg_cron job is the sole caller.
create function run_daily_maintenance()
returns void
language plpgsql
security definer
set search_path = public
as $$
begin
    insert into daily_stats (day, uploads, likes, reports)
    select day, uploads, likes, reports
    from (
        select
            d.day,
            coalesce((select count(*) from drills        where created_at::date = d.day), 0) as uploads,
            coalesce((select count(*) from likes         where created_at::date = d.day), 0) as likes,
            coalesce((select count(*) from drill_reports where created_at::date = d.day), 0) as reports
        from (
            select generate_series(current_date - 14, current_date - 1, interval '1 day')::date as day
        ) d
        where not exists (select 1 from daily_stats s where s.day = d.day)
    ) rolled
    where uploads + likes + reports > 0
    on conflict (day) do nothing;
end;
$$;

revoke execute on function run_daily_maintenance() from public, anon, authenticated;

-- Nightly at 00:10 UTC: freeze completed days into daily_stats.
select cron.schedule(
    'opendojo-daily-maintenance',
    '10 0 * * *',
    $$ select public.run_daily_maintenance() $$
);

-- =============================================================
-- get_drill(uuid) — single-drill read. Bumps the lifetime download counter
-- on drills and returns the content. SECURITY DEFINER to bypass the
-- table-level RLS (anon has no SELECT on drills).
-- =============================================================

create function get_drill(p_drill_id uuid)
returns table (
    id                uuid,
    name              text,
    -- "character" is quoted: it's a reserved word in the RETURNS TABLE
    -- column grammar (unquoted is a syntax error). The output column is
    -- still named `character`, so PostgREST / the client are unaffected.
    "character"       text,
    content           text,
    size_bytes        int,
    recordings_count  int
)
language plpgsql
security definer
set search_path = public
as $$
begin
    return query
    update drills d
       set downloads = d.downloads + 1
     where d.id = p_drill_id
       and d.status = 'public'
    returning
        d.id,
        d.name,
        d.character,
        d.content,
        d.size_bytes,
        d.recordings_count;
end;
$$;

-- =============================================================
-- toggle_like(uuid) — atomic like/unlike.
-- Returns the new like count for the drill. If the user already
-- liked it, this removes the like; otherwise it adds one. Anon
-- users can call it (SECURITY DEFINER so the RPC owns the writes
-- to drills.likes; the table itself stays inaccessible).
-- =============================================================

create function toggle_like(p_drill_id uuid)
returns int
language plpgsql
security definer
set search_path = public
as $$
declare
    v_user_id    uuid := auth.uid();
    v_inserted   int;
    v_likes      bigint;
begin
    if v_user_id is null then
        raise exception 'authentication required' using errcode = '42501';
    end if;

    -- Try to insert; on PK conflict the row already exists.
    insert into likes (user_id, drill_id)
    values (v_user_id, p_drill_id)
    on conflict do nothing;
    get diagnostics v_inserted = ROW_COUNT;

    if v_inserted > 0 then
        update drills set likes = likes + 1
         where id = p_drill_id and status = 'public'
        returning likes into v_likes;
    else
        delete from likes
         where user_id = v_user_id and drill_id = p_drill_id;
        update drills set likes = greatest(0, likes - 1)
         where id = p_drill_id and status = 'public'
        returning likes into v_likes;
    end if;

    -- v_likes is null only if the drill was removed between the
    -- summary fetch and the toggle; return 0 in that case.
    return coalesce(v_likes, 0)::int;
end;
$$;

-- =============================================================
-- delete_my_drill(uuid) — author-initiated hard delete.
-- Only deletes if the caller is the uploader; silently returns
-- false otherwise (UI never shows the action to non-owners).
-- Cascades remove likes via the FK on delete cascade. We hard
-- delete rather than soft-mark removed so the user can re-upload
-- the same content later without the unique-content-hash index
-- treating it as a duplicate of their own removed row.
-- =============================================================

create function delete_my_drill(p_drill_id uuid)
returns boolean
language plpgsql
security definer
set search_path = public
as $$
declare
    v_user_id uuid := auth.uid();
    v_deleted int;
begin
    if v_user_id is null then
        raise exception 'authentication required' using errcode = '42501';
    end if;
    delete from drills
     where id = p_drill_id and uploader_id = v_user_id;
    get diagnostics v_deleted = ROW_COUNT;
    return v_deleted > 0;
end;
$$;

-- =============================================================
-- report_drill(uuid, text) — file a moderation complaint.
-- One report per user per drill (composite PK). Auto-flips status to
-- 'flagged' when the report counter crosses 10 — flagged drills disappear
-- from drill_summaries (filtered by status = 'public') so further harm is
-- contained until admin reviews. 10 distinct reporters is a strong enough
-- signal to auto-hide without letting a few throwaway anon accounts censor
-- a drill.
-- =============================================================

create function report_drill(p_drill_id uuid, p_reason text default null)
returns boolean
language plpgsql
security definer
set search_path = public
as $$
declare
    v_user_id  uuid := auth.uid();
    v_inserted int;
begin
    if v_user_id is null then
        raise exception 'authentication required' using errcode = '42501';
    end if;

    -- One report per (drill, user) — quiet conflict is the
    -- success path for "you already reported this".
    insert into drill_reports (drill_id, user_id, reason)
    values (p_drill_id, v_user_id, p_reason)
    on conflict (drill_id, user_id) do nothing;
    get diagnostics v_inserted = ROW_COUNT;

    if v_inserted > 0 then
        update drills
           set reports = reports + 1,
               status  = case
                            when reports + 1 >= 10 and status = 'public'
                              then 'flagged'
                            else status
                         end
         where id = p_drill_id;
    end if;
    return v_inserted > 0;
end;
$$;

-- =============================================================
-- RLS — deny everything to anon/authenticated on `drills`. They
-- get to the data only through `drill_summaries` (filtered view)
-- and `get_drill` (SECURITY DEFINER). The Edge Function bypasses
-- RLS by virtue of using the secret key.
-- =============================================================

alter table drills        enable row level security;
alter table likes         enable row level security;

-- No policies = no rows visible. We intentionally do not grant any.
-- The view + functions are the supported read/write paths.

-- Function/view grants. Revoke defaults first so we know exactly
-- what's exposed.
revoke all on drills              from anon, authenticated;
revoke all on likes               from anon, authenticated;
revoke all on drill_summaries     from anon, authenticated;

-- The lookup tables are public reference data, safe to expose.
-- If the project has "automatically enable RLS on new tables"
-- turned on (recommended), those tables come up with RLS enabled
-- and no policies — which means SELECT returns zero rows even
-- with the GRANT below. The explicit read policies make them
-- queryable regardless of the project-level setting.
alter table drill_categories   enable row level security;
alter table drill_difficulties enable row level security;
create policy drill_categories_public_read   on drill_categories
    for select to anon, authenticated using (true);
create policy drill_difficulties_public_read on drill_difficulties
    for select to anon, authenticated using (true);
grant  select on drill_categories     to anon, authenticated;
grant  select on drill_difficulties   to anon, authenticated;

grant  select on drill_summaries  to   anon, authenticated;
grant  execute on function get_drill(uuid)
                                  to   anon, authenticated;
grant  execute on function toggle_like(uuid)
                                  to   anon, authenticated;
grant  execute on function delete_my_drill(uuid)
                                  to   anon, authenticated;
grant  execute on function report_drill(uuid, text)
                                  to   anon, authenticated;

-- =============================================================
-- Admin dashboard aggregations.
--
-- These views are read-only and exposed ONLY to service_role (the admin
-- panel hits them with the secret key, which bypasses RLS). We
-- explicitly revoke anon/authenticated so an "expose all tables" default
-- in Supabase can't let the public query them. They're owned by `postgres`
-- and run as the owner (not security_invoker) so underlying-table RLS
-- doesn't apply when service_role queries them — fine, because the revokes
-- keep public roles out entirely.
-- =============================================================

-- admin_daily_stats — per-day metrics for the dashboard chart (uploads /
-- likes / reports). Reads the frozen daily_stats rollup for completed days
-- and computes "today" live (today isn't frozen until tonight's run).
-- Windowed to the last 90 days; daily_stats keeps the full history.
-- Downloads aren't a daily series — see admin_overview.total_downloads for
-- the lifetime total.
create view admin_daily_stats as
with days as (
    select generate_series(
        (current_date - interval '89 days')::date,
        current_date,
        interval '1 day'
    )::date as day
),
today as (
    select
        (select count(*)::bigint from drills        where created_at::date = current_date) as uploads,
        (select count(*)::bigint from likes         where created_at::date = current_date) as likes,
        (select count(*)::bigint from drill_reports where created_at::date = current_date) as reports
)
select
    d.day,
    case when d.day = current_date then t.uploads else coalesce(s.uploads, 0) end as uploads,
    case when d.day = current_date then t.likes   else coalesce(s.likes,   0) end as likes,
    case when d.day = current_date then t.reports else coalesce(s.reports, 0) end as reports
from days d
cross join today t
left join daily_stats s on s.day = d.day
order by d.day;

-- admin_character_counts — drill distribution by character, including
-- flagged (admin wants to see those) but excluding removed. Sorted desc so
-- the dashboard can `limit 10` for a Top-N chart.
create view admin_character_counts as
select character, count(*)::bigint as n
from drills
where status in ('public', 'flagged')
group by character
order by count(*) desc;

-- admin_overview — one-row snapshot consolidating the stat cards into a
-- single round-trip.
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
-- anon/authenticated via PostgREST. These aggregate every user's behavior,
-- so they must stay admin-only.
revoke all on admin_daily_stats        from anon, authenticated;
revoke all on admin_character_counts   from anon, authenticated;
revoke all on admin_overview           from anon, authenticated;

grant select on admin_daily_stats      to service_role;
grant select on admin_character_counts to service_role;
grant select on admin_overview         to service_role;

-- =============================================================
-- Cap PostgREST page size at the database level. Even with the
-- `api.max_rows` setting in config.toml, this provides defense in
-- depth — if anyone ever flips that off, this still applies.
-- =============================================================

alter role authenticator set pgrst.db_max_rows = '50';

-- Reload PostgREST so the role setting takes effect immediately.
notify pgrst, 'reload config';
