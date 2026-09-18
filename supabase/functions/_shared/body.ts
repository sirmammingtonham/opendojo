export class BodyTooLarge extends Error {}

// Bound bytes before decoding JSON, including chunked requests and unused keys.
export async function readJsonBody(req: Request, limit: number): Promise<unknown> {
    if (!req.body) return JSON.parse("");
    const reader = req.body.getReader();
    const chunks: Uint8Array[] = [];
    let length = 0;
    try {
        while (true) {
            const { value, done } = await reader.read();
            if (done) break;
            length += value.byteLength;
            if (length > limit) {
                await reader.cancel().catch(() => {});
                throw new BodyTooLarge("request body too large");
            }
            if (value.byteLength) chunks.push(value);
        }
    } finally {
        reader.releaseLock();
    }
    const bytes = new Uint8Array(length);
    let offset = 0;
    for (const chunk of chunks) {
        bytes.set(chunk, offset);
        offset += chunk.byteLength;
    }
    return JSON.parse(new TextDecoder().decode(bytes));
}
