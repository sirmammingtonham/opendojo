-- OpenDojo Cloud — initial schema.
--
-- Design notes:
--   * Drill text is stored inline in the `drills.content` column. Drills
--     average a few KB; Postgres TOAST handles large rows transparently
--     and lz/zstd-compresses them. See supabase/README.md for why.
--   * No client role ever SELECTs from `drills` directly. The only read
--     paths exposed to `anon`/`authenticated` are the `drill_summaries`
--     view (metadata only, no content) and the `get_drill` SECURITY
--     DEFINER function (single-row read, rate-limited, bumps counters).
--   * No client role ever writes to `drills`. The only write path is
--     the `submit_drill` Edge Function, which uses the service-role key
--     and validates+rate-limits in TypeScript before inserting.
--   * Rate limiting uses a single `rate_limits` table keyed on
--     (user_id, day) and incremented atomically via INSERT ... ON
--     CONFLICT. This caps storage at O(users * active_days) and is
--     trivially aged out by a weekly cron.

set search_path = public, extensions;

-- pg_cron is preinstalled in Supabase; it lives in the `extensions`
-- schema. We use it for the weekly rate-limit cleanup.
create extension if not exists pg_cron with schema extensions;
create extension if not exists pgcrypto;  -- for gen_random_uuid + sha256

-- =============================================================
-- drills
-- =============================================================

create table drills (
    id                uuid primary key default gen_random_uuid(),

    -- Human-facing fields. `name` is required; the rest are best-effort.
    name              text not null check (char_length(name) between 1 and 96),
    description       text check (char_length(description) <= 240),
    character         text not null check (char_length(character) between 1 and 32),
    cpu_side          text check (cpu_side in ('p1', 'p2', '')),
    recordings_count  int  not null check (recordings_count between 1 and 8),
    author_handle     text check (char_length(author_handle) <= 32),

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
create        index drills_uploader_idx     on drills (uploader_id);
create        index drills_search_idx       on drills using gin (search_tsv);

-- =============================================================
-- drill_summaries view — the ONLY shape anon/authenticated can list.
-- Excludes the `content` column, so even a hostile client iterating
-- the whole view cannot pull drill bodies in bulk.
-- =============================================================

create view drill_summaries
with (security_invoker = true)  -- run as the calling role; RLS still applies
as
select
    id,
    name,
    description,
    character,
    cpu_side,
    recordings_count,
    author_handle,
    size_bytes,
    downloads,
    created_at
from drills
where status = 'public';

-- =============================================================
-- rate_limits — daily counters per user. Both upload and download
-- limits are tracked here so the table itself stays tiny
-- (O(users * active_days)). pg_cron prunes weekly.
-- =============================================================

create table rate_limits (
    user_id    uuid not null references auth.users(id) on delete cascade,
    day        date not null default current_date,
    uploads    int  not null default 0,
    downloads  int  not null default 0,
    primary key (user_id, day)
);

create index rate_limits_day_idx on rate_limits (day);

-- Weekly: drop counters older than 7 days. The window we care about
-- is "today" — keeping a week of slack just gives a small buffer for
-- clock-skew edge cases without letting the table grow unbounded.
select cron.schedule(
    'opendojo-rate-limits-cleanup',
    '0 4 * * 1',  -- Mondays 04:00 UTC
    $$ delete from public.rate_limits where day < current_date - 7 $$
);

-- =============================================================
-- get_drill(uuid) — single-drill read.
-- Bumps the download counter, enforces 100 downloads/day per user,
-- returns the content. SECURITY DEFINER because the function does
-- the auth.uid() bookkeeping while running with elevated privileges
-- to bypass the table-level RLS (anon has no SELECT on drills).
-- =============================================================

create function get_drill(p_drill_id uuid)
returns table (
    id                uuid,
    name              text,
    character         text,
    content           text,
    size_bytes        int,
    recordings_count  int
)
language plpgsql
security definer
set search_path = public
as $$
declare
    v_user_id   uuid := auth.uid();
    v_downloads int;
begin
    if v_user_id is null then
        raise exception 'authentication required' using errcode = '42501';
    end if;

    -- Atomically increment today's download count for this user.
    insert into rate_limits as r (user_id, day, downloads)
    values (v_user_id, current_date, 1)
    on conflict (user_id, day)
        do update set downloads = r.downloads + 1
    returning r.downloads into v_downloads;

    if v_downloads > 100 then
        raise exception 'download rate limit exceeded (100/day)'
            using errcode = 'P0001';
    end if;

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
-- try_record_submission(uuid) — atomic upload rate-limit check.
-- Called by the submit_drill Edge Function (service_role) before it
-- inserts a new drill. Returns the user's current daily upload count
-- AFTER incrementing; the Edge Function rolls back if the count
-- exceeds the limit. Wrapping the check in a function keeps the
-- whole upload path in one transaction.
-- =============================================================

create function try_record_submission(p_user_id uuid)
returns int
language plpgsql
security definer
set search_path = public
as $$
declare
    v_uploads int;
begin
    insert into rate_limits as r (user_id, day, uploads)
    values (p_user_id, current_date, 1)
    on conflict (user_id, day)
        do update set uploads = r.uploads + 1
    returning r.uploads into v_uploads;
    return v_uploads;
end;
$$;

-- =============================================================
-- RLS — deny everything to anon/authenticated on `drills`. They
-- get to the data only through `drill_summaries` (filtered view)
-- and `get_drill` (SECURITY DEFINER). The Edge Function bypasses
-- RLS by virtue of using the service_role key.
-- =============================================================

alter table drills        enable row level security;
alter table rate_limits   enable row level security;

-- No policies = no rows visible. We intentionally do not grant any.
-- The view + function are the supported read paths.

-- Function/view grants. Revoke defaults first so we know exactly
-- what's exposed.
revoke all on drills              from anon, authenticated;
revoke all on rate_limits         from anon, authenticated;
revoke all on drill_summaries     from anon, authenticated;

grant  select on drill_summaries  to   anon, authenticated;
grant  execute on function get_drill(uuid)
                                  to   anon, authenticated;

-- try_record_submission is only ever called by the Edge Function
-- (service_role), so we do not grant it to anon/authenticated.

-- =============================================================
-- Cap PostgREST page size at the database level. Even with the
-- `api.max_rows` setting in config.toml, this provides defense in
-- depth — if anyone ever flips that off, this still applies.
-- =============================================================

alter role authenticator set pgrst.db_max_rows = '50';

-- Reload PostgREST so the role setting takes effect immediately.
notify pgrst, 'reload config';
