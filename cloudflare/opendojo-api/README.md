# opendojo-api — Cloudflare Worker

Rate-limited proxy between the OpenDojo mod and the Supabase project.
The mod targets a custom domain (e.g. `https://opendojo.ethan.website`)
that resolves to this Worker; the Worker forwards every request to
`https://<SUPABASE_REF>.supabase.co`. Routing the mod through a Worker
we control means we can adjust rate limits, swap Supabase projects,
add caching, or insert a maintenance page later — none of which
require shipping a new DLL.

The Worker is written in **TypeScript**. `wrangler` compiles + bundles
via esbuild on every deploy; there's no separate build step. The
Cloudflare dashboard editor only accepts JavaScript, so live-editing
in the browser is no longer the supported path — use `wrangler deploy`.

## Files

- `worker.ts` — the Worker source.
- `wrangler.toml` — config for the Cloudflare CLI (`wrangler`).
  Drives `wrangler deploy`.
- `tsconfig.json` — `strict` TS with `@cloudflare/workers-types`
  pulled in for the `RateLimit` / `ExportedHandler` globals.
- `package.json` — declares `wrangler` + `@cloudflare/workers-types`
  as dev deps. `npm run deploy` is the one-liner.

## First-time setup (CLI / wrangler)

```powershell
cd cloudflare/opendojo-api
npm install
npx wrangler login

# Edit wrangler.toml first: set SUPABASE_REF to your project ref
# (the <ref> prefix in <ref>.supabase.co).

npx wrangler deploy
```

`wrangler deploy` creates the Worker (if needed), uploads the bundled
JS, and applies bindings declared in `wrangler.toml`. Subsequent
deploys are the same single command.

To attach the custom domain, uncomment the `[[routes]]` block in
`wrangler.toml` (set your zone) before deploying, or do it once in the
dashboard: **Workers & Pages → opendojo-api → Triggers → Custom
Domains → Add Custom Domain**.

Smoke test:

```powershell
curl.exe -v "https://opendojo.ethan.website/rest/v1/drill_summaries?select=id&limit=1" `
    -H "apikey: <anon_key>" `
    -H "Authorization: Bearer <anon_key>"
```

200 with `[]` means the proxy is live. Hit `/auth/v1/signup` rapidly
to confirm the rate-limit binding returns a 429.

Rebuild the DLL with the new URL:

```powershell
cmake -B dll/build -S dll -A x64 `
    -DOPENDOJO_SUPABASE_URL="https://opendojo.ethan.website" `
    -DOPENDOJO_SUPABASE_ANON_KEY="<anon public key>"
cmake --build dll/build --config Release
```

## Editing in the dashboard (fallback only)

If you absolutely need to paste code into the dashboard editor,
generate a bundled JS file first:

```powershell
npx wrangler deploy --dry-run --outdir dist
```

…then paste `dist/worker.js` into **Workers & Pages → opendojo-api →
Edit code**. This is a one-way path — changes you make in the
dashboard will be overwritten the next time someone runs `wrangler
deploy`. Prefer editing `worker.ts` and deploying.

## Typecheck without deploying

```powershell
npm run typecheck
```

Runs `tsc --noEmit` against `worker.ts` so CI / pre-commit can catch
type errors without needing CF credentials.

## Bindings cheat sheet

All keyed by IP (`cf-connecting-ip`). Sliding windows, not fixed
buckets — an attacker can't game the period boundary.

| Binding name   | Endpoint family                          | Rate     |
|----------------|------------------------------------------|----------|
| `RL_SIGNUP`    | `/auth/v1/signup`                        | 5 / 1h   |
| `RL_SUBMIT`    | `/functions/v1/submit_drill`             | 20 / 1h  |
| `RL_DOWNLOAD`  | `/rest/v1/rpc/get_drill`                 | 100 / 1h |
| `RL_REPORT`    | `/rest/v1/rpc/report_drill`              | 20 / 1h  |
| `RL_LIKE`      | `/rest/v1/rpc/toggle_like`               | 200 / 1h |
| `RL_AUTH_OTHER`| `/auth/v1/*` (other than signup)         | 60 / 60s |
| `RL_GENERAL`   | `/rest/v1/*` (anything not matched above)| 600 / 60s|

To change a limit later, edit the binding in `wrangler.toml` (or in
the dashboard) and redeploy. The DLL never needs to know.

## What this doesn't do

- It doesn't cache anything. Adding `cf: { cacheTtl: 60 }` to the
  `fetch()` call in `worker.ts` would cache `drill_summaries` reads,
  but Supabase's responses include `Vary: Authorization` so cross-user
  caching needs a custom cache key. Defer until you actually see
  egress pressure.
- It doesn't strip the `Authorization` header before logging or
  caching — important to remember if you ever add a logging layer.
