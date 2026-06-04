// opendojo-api - Cloudflare Worker that fronts the Supabase project.
//
// The mod hits this Worker at https://opendojo.ethan.website (or
// whatever custom domain is bound to the deployment). The Worker
// rate-limits per-IP via configured bindings, then forwards the
// request verbatim to https://<SUPABASE_REF>.supabase.co. Keeping
// this code stable is what lets us change routing/limits/caching
// later without shipping a new DLL.
//
// Deploy via wrangler (preferred — handles the TS compile for you):
//
//     wrangler deploy
//
// The Cloudflare dashboard editor only accepts JavaScript, so editing
// inline there is not supported. If you need a JS bundle to paste,
// run `wrangler deploy --dry-run --outdir dist` and the bundled
// worker.js shows up in dist/.
//
// Required configuration (Settings → Variables, Settings → Bindings):
//
//   Variable
//     SUPABASE_REF   plain text  — project ref ("abcd..." prefix of
//                                  <ref>.supabase.co)
//
//   Rate-limit bindings (all keyed by IP via cf-connecting-ip):
//     RL_SIGNUP       5 / 3600 s
//     RL_SUBMIT      20 / 3600 s
//     RL_DOWNLOAD   100 / 3600 s
//     RL_REPORT      20 / 3600 s
//     RL_LIKE       200 / 3600 s
//     RL_AUTH_OTHER  60 /   60 s
//     RL_GENERAL    600 /   60 s
//
// See cloudflare/opendojo-api/README.md for a full walkthrough.

// Cloudflare's `RateLimit` and `ExportedHandler` globals come from
// @cloudflare/workers-types (see tsconfig.json `types`).
export interface Env {
    SUPABASE_REF:   string;
    RL_SIGNUP:      RateLimit;
    RL_SUBMIT:      RateLimit;
    RL_DOWNLOAD:    RateLimit;
    RL_REPORT:      RateLimit;
    RL_LIKE:        RateLimit;
    RL_AUTH_OTHER:  RateLimit;
    RL_GENERAL:     RateLimit;
}

const RL_HEADERS: HeadersInit = {
    "content-type": "application/json",
    "retry-after":  "60",
};

function tooMany(): Response {
    return new Response(
        JSON.stringify({ error: "Too many requests. Slow down." }),
        { status: 429, headers: RL_HEADERS },
    );
}

// Pick the binding that matches the URL path. null = no rate limit
// (most paths that don't match still get an upstream-side cap from
// Supabase itself; this is just our defense-in-depth layer).
function bindingFor(path: string, env: Env): RateLimit | null {
    if (path.startsWith("/auth/v1/signup"))             return env.RL_SIGNUP;
    if (path.startsWith("/functions/v1/submit_drill"))  return env.RL_SUBMIT;
    if (path.startsWith("/rest/v1/rpc/get_drill"))      return env.RL_DOWNLOAD;
    if (path.startsWith("/rest/v1/rpc/report_drill"))   return env.RL_REPORT;
    if (path.startsWith("/rest/v1/rpc/toggle_like"))    return env.RL_LIKE;
    if (path.startsWith("/auth/v1/"))                   return env.RL_AUTH_OTHER;
    if (path.startsWith("/rest/v1/"))                   return env.RL_GENERAL;
    return null;
}

const handler: ExportedHandler<Env> = {
    async fetch(request, env) {
        if (!env.SUPABASE_REF) {
            return new Response("SUPABASE_REF env var not set", { status: 500 });
        }

        const url  = new URL(request.url);
        const path = url.pathname;
        const ip   = request.headers.get("cf-connecting-ip") ?? "unknown";

        // Rate-limit gate.
        const rl = bindingFor(path, env);
        if (rl) {
            const { success } = await rl.limit({ key: ip });
            if (!success) return tooMany();
        }

        // Forward to Supabase. Strip Host so the upstream sees its
        // own hostname; keep Authorization / apikey / everything else.
        const target = new URL(
            path + url.search,
            `https://${env.SUPABASE_REF}.supabase.co`,
        );
        const headers = new Headers(request.headers);
        headers.delete("host");

        return fetch(target, {
            method: request.method,
            headers,
            body:   ["GET", "HEAD"].includes(request.method) ? null : request.body,
        });
    },
};

export default handler;
