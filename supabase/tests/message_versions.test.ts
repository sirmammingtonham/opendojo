// Run the actual migrations and view in embedded PostgreSQL; no production access.
import { PGlite } from "npm:@electric-sql/pglite@0.5.8";

Deno.test("message audiences, version boundaries, and legacy clients", async () => {
    const db = new PGlite();
    try {
        await db.exec("create role anon; create role authenticated; create role service_role;");
        for (const migration of [
            "20260608000001_service_messages.sql",
            "20260918000001_message_version_ranges.sql",
            "20260918000002_update_notices.sql",
        ]) {
            await db.exec(await Deno.readTextFile(new URL(`../migrations/${migration}`, import.meta.url)));
        }
        await db.exec(`
            insert into service_messages (message, min_version, max_version, created_at) values
            ('global', null, null, '2026-01-01'),
            ('range', '0.6.0', '0.9.0', '2026-01-02'),
            ('exact', '0.7.0', '0.7.0', '2026-01-03'),
            ('no upper bound', '0.10.0', null, '2026-01-04'),
            ('no lower bound', null, '0.5.0', '2026-01-05');
            insert into service_messages (message, active, created_at)
                values ('inactive', false, '2026-02-01');
            insert into service_messages (message, expires_at, created_at)
                values ('expired', '2020-01-01', '2026-02-01');
        `);
        async function expect(version: string | undefined, expected: string, update?: string | null) {
            const headers = version === undefined ? {} : { "x-opendojo-version": version };
            await db.query("select set_config('request.headers', $1, false)", [JSON.stringify(headers)]);
            await db.exec("set role anon");
            try {
                const result = await db.query<{ message: string; update_version: string | null }>(
                    "select message, update_version from active_service_messages limit 1",
                );
                if (result.rows[0]?.message !== expected) {
                    throw new Error(`${version}: expected ${expected}, got ${JSON.stringify(result.rows)}`);
                }
                if (update !== undefined && result.rows[0]?.update_version !== update) {
                    throw new Error(`${version}: expected update ${update}, got ${JSON.stringify(result.rows)}`);
                }
            } finally { await db.exec("reset role"); }
        }
        await expect(undefined, "no lower bound");
        await expect("", "global");
        await expect("0.6.0", "range");
        await expect("0.9.0", "range");
        await expect("0.7.0", "exact");
        await expect("0.9.1", "global");
        await expect("0.10.0", "no upper bound");
        await expect("1.0.0", "no upper bound");
        await expect("0.5.0", "no lower bound");
        await expect("0.4.0", "no lower bound");
        for (const invalid of ["v0.6.0", "0.6", "0.6.0-beta", "999999999999.0.0", "'; drop table service_messages; --"]) {
            await expect(invalid, "global");
        }
        await db.exec(`insert into service_messages (message, min_version, max_version, created_at)
            values ('update 0.5', '0.5.0', '0.5.0', '2026-03-01')`);
        await expect(undefined, "update 0.5");
        await expect("0.5.0", "update 0.5");
        await expect("0.6.0", "range");
        await expect("0.4.0", "no lower bound");
        await expect("", "global");
        await expect("bad", "global");
        await db.exec("update service_messages set active = false where message in ('update 0.5', 'no lower bound')");
        await expect(undefined, "global");
        for (const bounds of [["bad", null], [null, "bad"], ["1.0.0", "0.9.0"]]) {
            let rejected = false;
            try {
                await db.query("insert into service_messages(message, min_version, max_version) values ('bad', $1, $2)", bounds);
            } catch { rejected = true; }
            if (!rejected) throw new Error(`Invalid bounds accepted: ${bounds}`);
        }
        await db.exec("update service_messages set active = false where message = 'exact'");
        await expect("0.7.0", "range");
        await db.exec("update service_messages set active = false where message = 'range'");
        await expect("0.7.0", "global");
        await db.exec(`insert into service_messages (message, update_version, created_at)
            values ('new release', '0.6.0', '2026-04-01')`);
        await expect(undefined, "new release");
        await expect("0.5.0", "new release");
        await expect("0.6.0", "global");
        await expect("0.7.0", "global");
        await expect("bad", "global");
        await db.exec("update service_messages set min_version = '0.5.0' where message = 'new release'");
        await expect("0.4.0", "global");
        await expect(undefined, "new release");
        let invalidUpdateRejected = false;
        try {
            await db.exec("insert into service_messages(message, update_version) values ('bad', '0.6.x')");
        } catch { invalidUpdateRejected = true; }
        if (!invalidUpdateRejected) throw new Error("Invalid update version accepted");
        await db.exec(`insert into service_messages (message, created_at)
            values ('maintenance', '2026-05-01')`);
        await expect(undefined, "maintenance", "0.6.0");
        await expect("0.6.0", "maintenance", null);
        await db.exec(`insert into service_messages (message, update_version, created_at)
            values ('', '0.7.0', '2026-06-01')`);
        await expect("0.6.0", "maintenance", "0.7.0");
        await expect("0.7.0", "maintenance", null);
        let emptyAnnouncementRejected = false;
        try { await db.exec("insert into service_messages(message) values ('   ')"); }
        catch { emptyAnnouncementRejected = true; }
        if (!emptyAnnouncementRejected) throw new Error("Empty general announcement accepted");
        await db.exec("update service_messages set active = false where message <> ''");
        await expect(undefined, "", "0.7.0");
        await db.exec("update service_messages set active = false");
        const cleared = await db.query("select * from active_service_messages");
        if (cleared.rows.length !== 0) throw new Error("Deactivated notices still returned");
        // Client roles can read announcements, but cannot read or mutate the base table.
        await db.exec("set role authenticated");
        for (const sql of ["select * from service_messages", "delete from service_messages"]) {
            let rejected = false;
            try { await db.exec(sql); } catch { rejected = true; }
            if (!rejected) throw new Error(`Client unexpectedly allowed: ${sql}`);
        }
        await db.exec("reset role");
    } finally { await db.close(); }
});
