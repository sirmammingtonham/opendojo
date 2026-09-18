-- Update notices are explicit metadata, never inferred from announcement text.
-- Already applied in production with immediate constraint validation.
-- Preserve the executed SQL; suppress only the already-applied constraints.
alter table public.service_messages
    add column update_version text,
    drop constraint service_messages_message_check,
    -- squawk-ignore constraint-missing-not-valid
    add constraint service_messages_message_check
        check (char_length(message) <= 200 and
            (char_length(btrim(message)) > 0 or update_version is not null)),
    -- squawk-ignore constraint-missing-not-valid
    add constraint service_messages_update_version_valid
        check (update_version is null or public.message_version_key(update_version) is not null);

-- Old clients still request just message. Current/newer clients never receive
-- an update notice for a version they already have.
create or replace view public.active_service_messages
with (security_invoker = false)
as
with eligible as (
select m.*
from public.service_messages m
cross join lateral (
    select public.message_version_key(
        coalesce(nullif(current_setting('request.headers', true), '')::jsonb ->> 'x-opendojo-version', '0.5.0')
    ) as version
) client
where m.active
  and (m.update_version is null or
       client.version < public.message_version_key(m.update_version))
  and (m.expires_at is null or m.expires_at > now())
  and (
    (m.min_version is null and m.max_version is null)
    or (client.version is not null
        and (m.min_version is null or client.version >= public.message_version_key(m.min_version))
        and (m.max_version is null or client.version <= public.message_version_key(m.max_version)))
  )
)
-- Select text and update state independently: a newer announcement must not
-- hide an update, and a banner-only update must not erase existing title text.
select announcement.id, coalesce(announcement.message, '') as message,
       announcement.severity, announcement.created_at, updates.update_version
from (
    select id, message, severity, created_at from eligible
    where btrim(message) <> ''
    order by created_at desc, id desc limit 1
) announcement
full join (
    select update_version from eligible
    where update_version is not null
    order by created_at desc, id desc limit 1
) updates on true;

notify pgrst, 'reload schema';
