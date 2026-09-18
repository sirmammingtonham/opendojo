import { assertEquals, assertRejects } from "https://deno.land/std@0.220.0/assert/mod.ts";
import { BodyTooLarge, readJsonBody } from "./body.ts";

function request(chunks: string[]): Request {
    return new Request("https://example.invalid", {
        method: "POST",
        body: new ReadableStream({
            start(controller) {
                for (const chunk of chunks) controller.enqueue(new TextEncoder().encode(chunk));
                controller.close();
            },
        }),
    });
}

Deno.test("JSON body accepts exact limit without Content-Length", async () => {
    assertEquals(await readJsonBody(request(['{"a":', '1}']), 7), { a: 1 });
});
Deno.test("JSON body rejects oversized chunked unknown fields before parsing", async () => {
    await assertRejects(() => readJsonBody(request(['{"unused":"', "x".repeat(100), '"}']), 20), BodyTooLarge);
});
Deno.test("JSON body limit counts UTF-8 bytes", async () => {
    await assertRejects(() => readJsonBody(request(['"風"']), 4), BodyTooLarge);
    assertEquals(await readJsonBody(request(['"風"']), 5), "風");
});
Deno.test("Invalid and empty JSON retain parse failures", async () => {
    await assertRejects(() => readJsonBody(request(["{"]), 20), SyntaxError);
    await assertRejects(() => readJsonBody(request([]), 20), SyntaxError);
});
