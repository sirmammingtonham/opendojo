-- Inclusive numeric DLL version ranges. NULL bounds mean unbounded;
-- both NULL means global, including clients that do not report a version.
create function public.message_version_key(version text)
returns integer[] language sql immutable strict
set search_path = ''
as $$
    select case when version ~ '^[0-9]{1,6}\.[0-9]{1,6}\.[0-9]{1,6}$'
        then string_to_array(version, '.')::integer[] end;
$$;

-- Already applied in production with immediate constraint validation.
-- Preserve the executed SQL; suppress only the already-applied constraints.
alter table public.service_messages
    add column min_version text,
    add column max_version text,
    -- squawk-ignore constraint-missing-not-valid
    add constraint service_messages_min_version_valid
        check (min_version is null or public.message_version_key(min_version) is not null),
    -- squawk-ignore constraint-missing-not-valid
    add constraint service_messages_max_version_valid
        check (max_version is null or public.message_version_key(max_version) is not null),
    -- squawk-ignore constraint-missing-not-valid
    add constraint service_messages_version_order
        check (min_version is null or max_version is null or
            public.message_version_key(min_version) <= public.message_version_key(max_version));

-- Keep the existing endpoint and output columns for published clients.
-- The proxy already forwards custom headers. Version 0.5.0 is the only
-- released cloud client without this header. Missing means 0.5.0; a present
-- but malformed header remains unknown and receives only global messages.
create or replace view public.active_service_messages
with (security_invoker = false)
as
select m.id, m.message, m.severity, m.created_at
from public.service_messages m
cross join lateral (
    select public.message_version_key(
        coalesce(nullif(current_setting('request.headers', true), '')::jsonb ->> 'x-opendojo-version', '0.5.0')
    ) as version
) client
where m.active
  and (m.expires_at is null or m.expires_at > now())
  and (
    (m.min_version is null and m.max_version is null)
    or (client.version is not null
        and (m.min_version is null or client.version >= public.message_version_key(m.min_version))
        and (m.max_version is null or client.version <= public.message_version_key(m.max_version)))
  )
order by m.created_at desc;

notify pgrst, 'reload schema';
