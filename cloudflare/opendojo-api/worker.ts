// opendojo-api — Cloudflare Worker that fronts the Supabase project.
//
// The mod talks ONLY to this Worker, and the Worker is the single trusted
// front door to the backend:
//
//   * It holds the Supabase anon key as a SECRET and injects it upstream.
//     The DLL never ships the anon key, so the backend (<ref>.supabase.co)
//     is unreachable without coming through here. That is what makes the
//     per-IP rate limits below an actual boundary instead of a suggestion —
//     an attacker can't skip the Worker and hit Supabase directly, because
//     they don't have (and can't forge) the anon key.
//
//   * It requires a shared proxy access key (X-OpenDojo-Key) so the Worker
//     isn't a public anon-key-injecting API. The key is baked into the DLL,
//     so it isn't a hard secret — but rotating it cuts off old/abusive
//     clients without touching the backend.
//
//   * It pins every upstream request to our Supabase host. The path can
//     never redirect the request elsewhere (see the host-pin note below).
//
// Configuration (see wrangler.toml for the non-secret bits):
//
//   Secrets — `wrangler secret put <NAME>` (never commit these):
//     SUPABASE_ANON_KEY   project anon key, injected upstream
//     PROXY_KEY           shared access key the DLL sends as X-OpenDojo-Key
//
//   Plain var — wrangler.toml [vars] / dashboard:
//     SUPABASE_REF        project ref (the <ref> in <ref>.supabase.co)
//
//   Rate-limit bindings (all keyed by cf-connecting-ip) — see wrangler.toml.
//
// Deploy with `wrangler deploy` (compiles the TS for you).

// Cloudflare's `RateLimit` and `ExportedHandler` globals come from
// @cloudflare/workers-types (see tsconfig.json `types`).
export interface Env {
    SUPABASE_REF:       string;
    SUPABASE_ANON_KEY:  string;
    PROXY_KEY:          string;
    RL_SIGNUP:      RateLimit;
    RL_SUBMIT:      RateLimit;
    RL_DOWNLOAD:    RateLimit;
    RL_REPORT:      RateLimit;
    RL_LIKE:        RateLimit;
    RL_AUTH_OTHER:  RateLimit;
    RL_GENERAL:     RateLimit;
}

const JSON_HEADERS: HeadersInit = { "content-type": "application/json" };

function deny(status: number, msg: string, extra?: HeadersInit): Response {
    return new Response(JSON.stringify({ error: msg }), {
        status,
        headers: { ...JSON_HEADERS, ...(extra ?? {}) },
    });
}

// Pick the binding that matches the URL path. null = no specific limit
// (Supabase still applies its own upstream caps; this is our layer).
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

// Length-independent equality so a wrong PROXY_KEY can't be recovered
// byte-by-byte via response-timing. (It isn't a hard secret — it ships in
// the DLL — but there's no reason to leak it faster than necessary.)
function safeEqual(a: string, b: string): boolean {
    if (a.length !== b.length) return false;
    let diff = 0;
    for (let i = 0; i < a.length; ++i) diff |= a.charCodeAt(i) ^ b.charCodeAt(i);
    return diff === 0;
}

const handler: ExportedHandler<Env> = {
    async fetch(request, env) {
        if (!env.SUPABASE_REF || !env.SUPABASE_ANON_KEY || !env.PROXY_KEY) {
            return deny(500, "proxy is misconfigured");
        }

        // Gate 1 — only our client may use the proxy. Without this the
        // Worker would be a public API that injects the anon key for anyone.
        const presented = request.headers.get("x-opendojo-key") ?? "";
        if (!safeEqual(presented, env.PROXY_KEY)) {
            return deny(403, "forbidden");
        }

        const url  = new URL(request.url);
        const path = url.pathname;
        const ip   = request.headers.get("cf-connecting-ip") ?? "unknown";

        // Only forward ordinary single-slash absolute paths. A path like
        // "//evil.com/x" is a scheme-relative reference that would let the
        // upstream URL resolve to another host — and we'd ship the anon key
        // there. Reject it outright (the host-pin below is the real guard;
        // this is belt-and-suspenders).
        if (!path.startsWith("/") || path.startsWith("//")) {
            return deny(400, "bad path");
        }

        // Gate 2 — per-IP rate limit. Non-bypassable: the backend can only
        // be reached through this Worker.
        const rl = bindingFor(path, env);
        if (rl) {
            const { success } = await rl.limit({ key: ip });
            if (!success) {
                return deny(429, "Too many requests. Slow down.", { "retry-after": "60" });
            }
        }

        // Build the upstream URL from a FIXED origin. Assigning pathname /
        // search can never change the authority, so the request is pinned
        // to our Supabase project regardless of what the caller sent.
        const target = new URL(`https://${env.SUPABASE_REF}.supabase.co`);
        target.pathname = path;
        target.search   = url.search;

        // Inject the backend key server-side and strip the proxy key so it
        // never leaves the edge. Leave a user Bearer JWT (authenticated
        // calls) intact; supply the anon key as the bearer for the
        // unauthenticated calls (e.g. anonymous signup).
        const headers = new Headers(request.headers);
        headers.delete("host");
        headers.delete("x-opendojo-key");
        headers.set("apikey", env.SUPABASE_ANON_KEY);
        if (!headers.has("authorization")) {
            headers.set("authorization", `Bearer ${env.SUPABASE_ANON_KEY}`);
        }

        return fetch(target, {
            method: request.method,
            headers,
            body:   ["GET", "HEAD"].includes(request.method) ? null : request.body,
        });
    },
};

export default handler;
