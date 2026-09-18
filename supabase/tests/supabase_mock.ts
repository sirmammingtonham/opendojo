// Endpoint regression fixture: both duplicate lookups finish before insertion.
type Row = Record<string, unknown> & { id: string };
export const database: { row: Row | null; firstOwner: unknown; reads: number } = {
    row: null, firstOwner: null, reads: 0,
};
let release!: () => void;
const bothRead = new Promise<void>((resolve) => release = resolve);

class Query {
    private values: Record<string, unknown> | null = null;
    private ignoreDuplicates = false;
    constructor(private table: string) {}
    select(_fields: string) { return this; }
    eq(_field: string, _value: unknown) { return this; }
    upsert(values: Record<string, unknown>, options: { onConflict: string; ignoreDuplicates: boolean }) {
        this.values = values;
        this.ignoreDuplicates = options.ignoreDuplicates;
        return this;
    }
    async maybeSingle(): Promise<{ data: Row | null; error: null }> {
        if (this.table === "user_bans") return { data: null, error: null };
        if (!this.values) {
            if (++database.reads <= 2) {
                if (database.reads === 2) release();
                await bothRead;
                return { data: null, error: null };
            }
            return { data: database.row, error: null };
        }
        if (database.row && this.ignoreDuplicates) return { data: null, error: null };
        if (!database.row) database.firstOwner = this.values.uploader_id;
        database.row = { ...this.values, id: "same-drill" };
        return { data: database.row, error: null };
    }
}

export function createClient(_url: string, _key: string, _options: unknown) {
    return {
        auth: { getUser: (token: string) => Promise.resolve({ data: { user: { id: token } }, error: null }) },
        from: (table: string) => new Query(table),
    };
}
