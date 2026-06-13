-- OpenDojo Cloud — per-user like state + download de-duplication.
--
-- Two fixes:
--   1. Likes: the toggle_like RPC already prevents net inflation (a second
--      toggle removes the like), but the client had no way to know whether
--      the current user already liked a drill, so the button reset to "Like"
--      every fresh session. Expose `liked_by_me` on drill_summaries so the
--      client renders the correct state and the toggle stays predictable.
--   2. Downloads: get_drill bumped drills.downloads on *every* fetch, so a
--      user re-downloading the same drill inflated its count without bound.
--      Track unique (user, drill) downloads and only count the first one.

set search_path = public, extensions;

-- =============================================================
-- drill_downloads — one row per (user, drill) the user has downloaded.
-- The (user_id, drill_id) PK makes get_drill's counter increment idempotent
-- per user. Mirrors the likes table; the denormalized drills.downloads
-- counter remains the public tally, maintained by get_drill.
-- =============================================================
create table drill_downloads (
    user_id    uuid not null references auth.users(id) on delete cascade,
    drill_id   uuid not null references drills(id)     on delete cascade,
    created_at timestamptz not null default now(),
    primary key (user_id, drill_id)
);
create index drill_downloads_drill_idx on drill_downloads (drill_id);

alter table drill_downloads enable row level security;
-- No client policies: get_drill (SECURITY DEFINER) is the only writer and
-- nothing reads this table from the client. service_role (admin) may prune.
revoke all on drill_downloads from anon, authenticated;
grant select, delete on drill_downloads to service_role;

-- =============================================================
-- get_drill(uuid) — single-drill read, now de-duplicating the download
-- counter. Content is always returned (re-downloads are allowed, e.g. if the
-- local copy was deleted); only the *first* download per user bumps
-- drills.downloads. Replaces the original definition (same signature +
-- return type, so CREATE OR REPLACE keeps existing grants).
-- =============================================================
create or replace function get_drill(p_drill_id uuid)
returns table (
    id                uuid,
    name              text,
    "character"       text,
    content           text,
    size_bytes        int,
    recordings_count  int
)
language plpgsql
security definer
set search_path = public
as $$
declare
    v_user uuid := auth.uid();
    v_new  int  := 0;
begin
    -- Record a unique download. Only a freshly-inserted row (this user's
    -- first download of this drill) counts toward the public tally.
    if v_user is not null then
        insert into drill_downloads (user_id, drill_id)
        values (v_user, p_drill_id)
        on conflict do nothing;
        get diagnostics v_new = row_count;
    end if;

    return query
    update drills d
       set downloads = d.downloads + (case when v_new > 0 then 1 else 0 end)
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

-- Re-grant defensively (CREATE OR REPLACE keeps grants, but a fresh function
-- object on a clean DB would have none).
grant execute on function get_drill(uuid) to anon, authenticated;

-- =============================================================
-- drill_summaries — add `liked_by_me` so the client can render the like
-- button in the correct state for the current user. CREATE OR REPLACE keeps
-- the existing grants; the new column is appended at the end of the list
-- (Postgres only allows additions at the end for OR REPLACE).
-- =============================================================
create or replace view drill_summaries
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
    size_bytes,
    downloads,
    likes,
    (uploader_id = auth.uid()) as is_mine,
    created_at,
    exists (
        select 1 from likes l
        where l.drill_id = drills.id
          and l.user_id = auth.uid()
    ) as liked_by_me
from drills
where status = 'public';

-- Reload PostgREST so the new table + the view's new column land in its
-- schema cache immediately.
notify pgrst, 'reload schema';
