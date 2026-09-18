Run the endpoint concurrency regression separately with its test-only Supabase
client mapping. It exercises the real submit handler without network access or
production credentials:

```powershell
deno test --import-map=supabase/tests/import_map.json --allow-env=SUPABASE_URL,OPENDOJO_SECRET_KEY supabase/tests/submit_race.test.ts
```

The shared request-body and existing validation suites run normally:

```powershell
deno test --allow-read --allow-env=OPENDOJO_NAMED_DRILL supabase/functions/_shared supabase/functions/submit_drill supabase/functions/update_drill
```

Set `OPENDOJO_NAMED_DRILL` to the C++ compatibility test's generated
`named-drill.txt` to include the encoder/cloud round-trip fixtures.

Deploy the updated submit/update functions before releasing the DLL: the
description-line allowance is a server fix. Existing client request/response
formats and previously valid drills remain supported; those function fixes need no migration.

Version-targeted service messages require the separate
`20260918000001_message_version_ranges.sql` migration (`supabase db push` on the
linked project). Apply it before using the updated admin panel's Messages tab.
The new DLL sends its build version in `X-OpenDojo-Version`; the existing proxy
forwards that header, so neither a proxy nor an Edge Function deployment is
needed for message targeting. A new DLL against the old database still receives
global messages normally.

Choose Global or an inclusive Version range in the admin panel. Bounds use
`major.minor.patch` (for example `0.6.0`); leave either end blank for an open range,
or set both to the same version to target one release. The newest active,
unexpired matching message wins, including global messages. Deactivating a
message reveals the next matching message. A missing version header is treated
as `0.5.0`, the only released cloud client without version reporting. Set both
bounds to `0.5.0` to target those installations. A present but malformed version
receives only global messages.

Exercise the actual migration and view using embedded PostgreSQL, without a
running database or production credentials:

```powershell
deno test --allow-read --allow-env --allow-sys supabase/tests/message_versions.test.ts
```

Update banners additionally require `20260918000002_update_notices.sql`.
In Messages, set "Update notice for release" to the published version (for
example `0.7.0`). Only clients older than that release and within the selected
audience receive it. Message text is optional for update notices. New DLLs show a banner above the tabs with a Download update
button pointing to the fixed project `/releases/latest` URL. Version 0.5 still
shows announcement text in its title bar; include text when notifying that release.
Leave the release field blank for a general announcement. The newest matching
nonempty text and update notice are selected independently, so a general
announcement does not hide the update banner and a banner-only notice does not
erase the title message. Deactivating either reveals the next match for that field.

For local UI testing, configure with `-DOPENDOJO_PREVIEW_UPDATE=ON` and rebuild.
This displays a preview banner without contacting or changing the cloud.
The option defaults to OFF for clean builds, including the release workflow.
Turn it OFF and rebuild when finished testing.
