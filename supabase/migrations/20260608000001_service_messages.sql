-- OpenDojo Cloud — service messages.
--
-- A small operator-controlled broadcast channel surfaced in the mod's
-- title bar ("New update available", maintenance notices, etc.). The
-- admin panel writes rows with the service-role key; clients read the
-- single most-recent active, non-expired message through the
-- active_service_messages view. Same shape as the rest of the schema:
-- the base table stays locked behind RLS with no client policies, and
-- the only read path exposed to anon/authenticated is a curated view.

set search_path = public, extensions;

create table service_messages (
    id         uuid primary key default gen_random_uuid(),
    message    text not null check (char_length(message) between 1 and 200),
    -- Severity is operator metadata for now (the title bar renders the
    -- text the same regardless). Constrained to a small known set so a
    -- typo in the admin panel can't slip through; the client ignores it.
    severity   text not null default 'info'
               check (severity in ('info', 'warning', 'urgent')),
    -- Toggled off from the admin panel to retire a message without
    -- deleting it (keeps a history of what was broadcast).
    active     boolean not null default true,
    -- Optional auto-expiry. Null = show until explicitly deactivated.
    expires_at timestamptz,
    created_at timestamptz not null default now()
);
-- Drives the "newest active" lookup the view does.
create index service_messages_active_idx on service_messages (active, created_at desc);

-- Owner-rights view (security_invoker = false): clients get the curated
-- active message without any access to the base table, mirroring
-- drill_summaries. Returns active, non-expired rows newest-first; the
-- client reads with limit=1 to take just the most recent.
create view active_service_messages
with (security_invoker = false)
as
select
    id,
    message,
    severity,
    created_at
from service_messages
where active
  and (expires_at is null or expires_at > now())
order by created_at desc;

-- =============================================================
-- RLS + grants. Base table is closed to the public roles entirely;
-- reads go through the view, writes through the admin panel
-- (service_role bypasses RLS but still needs the explicit table GRANT).
-- =============================================================

alter table service_messages enable row level security;
-- No client policies = no rows visible. The view + service_role are the
-- supported read/write paths.

revoke all on service_messages       from anon, authenticated;
revoke all on active_service_messages from anon, authenticated;

grant select on active_service_messages to anon, authenticated;
-- Admin panel CRUD (publish / activate-deactivate / delete).
grant select, insert, update, delete on service_messages to service_role;

-- Reload PostgREST so the new view + table land in its schema cache
-- immediately rather than on the next periodic refresh.
notify pgrst, 'reload schema';
