import { assertEquals } from "https://deno.land/std@0.220.0/assert/mod.ts";
import { database } from "./supabase_mock.ts";

// Run with --import-map=supabase/tests/import_map.json and
// --allow-env=SUPABASE_URL,OPENDOJO_SECRET_KEY. No network or real credentials.
Deno.test("concurrent duplicate submit preserves first ownership and response compatibility", async () => {
    const originalServe = Object.getOwnPropertyDescriptor(Deno, "serve")!;
    const envNames = ["SUPABASE_URL", "OPENDOJO_SECRET_KEY"];
    const previous = envNames.map((name) => Deno.env.get(name));
    let handle!: (request: Request) => Promise<Response>;
    try {
        Deno.env.set("SUPABASE_URL", "https://test.invalid");
        Deno.env.set("OPENDOJO_SECRET_KEY", "test-only");
        Object.defineProperty(Deno, "serve", { configurable: true, value: (handler: typeof handle) => handle = handler });
        await import("../functions/submit_drill/index.ts");
        const content = "# OpenDojo drill\nname: shared\ncharacter: jin\nrecordings: 1\n--- recording 1\nn . 1\n";
        const request = (user: string) => new Request("https://test.invalid/submit", {
            method: "POST", headers: { Authorization: `Bearer ${user}` },
            body: JSON.stringify({ name: "Shared", character: "jin", author_handle: user,
                recordings_count: 1, content }),
        });
        const responses = await Promise.all([handle(request("first")), handle(request("second"))]);
        assertEquals(responses.map((response) => response.status), [200, 200]);
        const bodies = await Promise.all(responses.map((response) => response.json()));
        assertEquals(bodies.map((body) => body.id), ["same-drill", "same-drill"]);
        assertEquals(bodies.filter((body) => body.deduped).length, 1);
        assertEquals(database.row!.uploader_id, database.firstOwner);
        assertEquals(database.row!.author_handle, database.firstOwner);
    } finally {
        Object.defineProperty(Deno, "serve", originalServe);
        envNames.forEach((name, i) => previous[i] === undefined ? Deno.env.delete(name) : Deno.env.set(name, previous[i]!));
    }
});
