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
formats and previously valid drills remain supported; no migration is needed.
