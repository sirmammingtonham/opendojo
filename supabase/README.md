# OpenDojo Cloud — Supabase backend

This directory holds everything needed to stand up the drill-sharing
backend: SQL schema, RLS policies, and the single Edge Function that
gates uploads. It's structured for the official Supabase CLI.

## What you get

- A `drills` table with the drill text stored inline as `TEXT`.
- A `drill_summaries` view that exposes metadata only (no content).
- RPC functions `list_drills`, `get_drill`, `bump_download` — the only
  way the mod reads from the backend.
- A `submit_drill` Edge Function — the only way anyone writes to it.
- Anonymous-auth identity so every install gets a stable `user_id`
  the server can rate-limit against, with zero UX cost.

The mod (Windows DLL) ships with the project URL + anon public key
baked in at build time. The anon key is safe to ship — RLS is the
actual security boundary.

## Cost protections

The free tier covers a real userbase, but a malicious client with the
anon key could otherwise pull the whole table or spam uploads. The
hardening (all enforced server-side):

- `api.max_rows = 50` in `config.toml` caps every PostgREST response.
- `anon` and `authenticated` roles have **no direct access** to
  `drills` — only to the `drill_summaries` view (metadata) and the
  two RPC functions (read one drill, increment download counter).
- `get_drill` rate-limits downloads to 100/day per user via the
  `download_log` table.
- The `submit_drill` Edge Function is the only writer. It validates
  the JWT, parses the drill text server-side, enforces a 64 KB
  size cap + 8-recording cap, and rejects more than 5 uploads/day
  per user via the `submission_log` table. Duplicate uploads
  (same SHA-256 content hash) fail on the unique index.
- The service-role key is only used inside the Edge Function — never
  shipped in the DLL or any client.

## First-time setup

```powershell
# 1. Install the Supabase CLI (one-time).
scoop install supabase   # or: npm i -g supabase

# 2. Create a free project at https://supabase.com/dashboard.
#    Note the project ref (the subdomain in the dashboard URL).

# 3. Link this repo to your project.
supabase link --project-ref <YOUR_PROJECT_REF>

# 4. Push the schema + RLS policies.
supabase db push

# 5. Deploy the upload-gate Edge Function.
supabase functions deploy submit_drill

# 6. In the dashboard, enable anonymous auth:
#    Authentication → Providers → Anonymous Sign-Ins → on.
#    (config.toml has this flagged, but the dashboard is the source
#    of truth for free-tier projects.)

# 7. Grab your URL + anon key from Project Settings → API,
#    then rebuild the mod with them baked in:

cmake -B dll/build -S dll -A x64 `
  -DOPENDOJO_SUPABASE_URL="https://<YOUR_REF>.supabase.co" `
  -DOPENDOJO_SUPABASE_ANON_KEY="<YOUR_ANON_PUBLIC_KEY>"
cmake --build dll/build --config Release
```

## Iterating

Edit `migrations/*.sql`, then `supabase db push`. Edit
`functions/submit_drill/index.ts`, then `supabase functions deploy
submit_drill`. The CLI handles diffing and uploads.

## Manual moderation

There's no admin UI yet. To take a drill down:

```sql
update drills set status = 'removed' where id = '<uuid>';
```

The RLS filter on `drill_summaries` and `get_drill` immediately hides
it from clients.
