import { assertEquals } from "https://deno.land/std@0.220.0/assert/mod.ts";
import { validate } from "./validate.ts";

// Run after the C++ renaming compatibility test. This validates the actual
// encoder output through the upload request's JSON serialization boundary.
// deno test --allow-read --allow-env=OPENDOJO_NAMED_DRILL renaming.test.ts
const envPermission = await Deno.permissions.query({ name: "env", variable: "OPENDOJO_NAMED_DRILL" });
const fixture = envPermission.state === "granted" ? Deno.env.get("OPENDOJO_NAMED_DRILL") : undefined;
Deno.test({
    name: "named C++ export survives cloud validation unchanged",
    ignore: !fixture,
    fn() {
        const content = Deno.readTextFileSync(fixture!);
        const request = JSON.parse(JSON.stringify({
            name: "Rename compatibility", character: "jin", author_handle: "Tester",
            recordings_count: 6, content,
        }));
        const result = validate(request);
        if ("err" in result) throw new Error(result.err);
        assertEquals(result.ok.content, content);
    },
});

Deno.test({
    name: "long multiline C++ export remains uploadable",
    ignore: !fixture,
    fn() {
        const content = Deno.readTextFileSync(fixture! + ".multiline");
        const result = validate({ name: "Audit", character: "jin", author_handle: "Tester",
            recordings_count: 6, description: Array(6).fill("a".repeat(120)).join("\n"), content });
        if ("err" in result) throw new Error(result.err);
        assertEquals(result.ok.content, content);
    },
});
