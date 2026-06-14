// opendojo-api — rate-limited request proxy. Deploy with `wrangler deploy`.

export interface Env {
    SUPABASE_REF:             string;
    SUPABASE_PUBLISHABLE_KEY: string;
    PROXY_KEY:                string;
    RL_SIGNUP:      RateLimit;
    RL_SUBMIT:      RateLimit;
    RL_UPDATE:      RateLimit;
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

// Allowlist: returns a bucket for a recognized path, else null (denied, not
// proxied).
function bindingFor(path: string, env: Env): RateLimit | null {
    if (path.startsWith("/auth/v1/signup"))             return env.RL_SIGNUP;
    if (path.startsWith("/functions/v1/submit_drill"))  return env.RL_SUBMIT;
    if (path.startsWith("/functions/v1/update_drill"))  return env.RL_UPDATE;
    if (path.startsWith("/rest/v1/rpc/get_drill"))      return env.RL_DOWNLOAD;
    if (path.startsWith("/rest/v1/rpc/report_drill"))   return env.RL_REPORT;
    if (path.startsWith("/rest/v1/rpc/toggle_like"))    return env.RL_LIKE;
    if (path.startsWith("/auth/v1/"))                   return env.RL_AUTH_OTHER;
    if (path.startsWith("/rest/v1/"))                   return env.RL_GENERAL;
    return null;
}

function safeEqual(a: string, b: string): boolean {
    if (a.length !== b.length) return false;
    let diff = 0;
    for (let i = 0; i < a.length; ++i) diff |= a.charCodeAt(i) ^ b.charCodeAt(i);
    return diff === 0;
}

const handler: ExportedHandler<Env> = {
    async fetch(request, env) {
        if (!env.SUPABASE_REF || !env.SUPABASE_PUBLISHABLE_KEY || !env.PROXY_KEY) {
            return deny(500, "misconfigured");
        }

        const presented = request.headers.get("x-opendojo-key") ?? "";
        if (!safeEqual(presented, env.PROXY_KEY)) {
            return deny(403, "forbidden");
        }

        const url  = new URL(request.url);
        const path = url.pathname;
        const ip   = request.headers.get("cf-connecting-ip") ?? "unknown";

        if (!path.startsWith("/") || path.startsWith("//")) {
            return deny(400, "bad path");
        }

        // No binding = not on the allowlist; deny rather than proxy unthrottled.
        const rl = bindingFor(path, env);
        if (!rl) {
            return deny(403, "forbidden");
        }
        const { success } = await rl.limit({ key: ip });
        if (!success) {
            return deny(429, "Too many requests. Slow down.", { "retry-after": "60" });
        }

        const target = new URL(`https://${env.SUPABASE_REF}.supabase.co`);
        target.pathname = path;
        target.search   = url.search;

        // Publishable key goes on `apikey` only — never as a Bearer token
        // (it isn't a JWT). A user's Bearer JWT, when present, is left intact.
        const headers = new Headers(request.headers);
        headers.delete("host");
        headers.delete("x-opendojo-key");
        headers.set("apikey", env.SUPABASE_PUBLISHABLE_KEY);

        return fetch(target, {
            method: request.method,
            headers,
            body:   ["GET", "HEAD"].includes(request.method) ? null : request.body,
        });
    },
};

export default handler;
