// Exercise the actual inline request helper without a browser or credentials.
Deno.test("admin publish preserves authentication with custom request headers", async () => {
    const html = await Deno.readTextFile(new URL("../admin/index.html", import.meta.url));
    const start = html.indexOf("function headers(extra)");
    const end = html.indexOf("// Specialized: PostgREST counts", start);
    if (start < 0 || end < 0) throw new Error("Admin request helper not found");
    let called = false;
    const fakeFetch = async (url: string, options: RequestInit) => {
        called = true;
        const headers = new Headers(options.headers);
        if (url !== "http://localhost/rest/v1/service_messages" || options.method !== "POST" ||
            headers.get("apikey") !== "test-key" ||
            headers.get("Content-Type") !== "application/json" ||
            headers.get("Prefer") !== "return=minimal" || options.body !== '{"message":"test"}') {
            throw new Error("Admin request lost its authentication, headers, or payload");
        }
        return new Response(null, { status: 201 });
    };
    const supa = new Function("fetch", "baseUrl", "svcKey",
        html.slice(start, end) + "\nreturn supa;")(fakeFetch, "http://localhost", "test-key");
    await supa("/rest/v1/service_messages", {
        method: "POST",
        headers: { Prefer: "return=minimal" },
        body: '{"message":"test"}',
    });
    if (!called) throw new Error("Request was not sent");
});
