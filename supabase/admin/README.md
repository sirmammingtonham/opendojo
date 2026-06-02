# OpenDojo Cloud — admin panel

Single static HTML file that hits Supabase REST directly with your
project's `service_role` key. Use it to moderate drills, manage bans,
and bulk-upload seed content.

## Open it

Most browsers allow `fetch()` from `file://` to HTTPS origins, so:

1. Double-click `index.html`.
2. Paste your project URL (`https://<ref>.supabase.co`) and the
   `service_role` key from **Project Settings → API → service_role**.
3. Click **Connect**.

If your browser blocks the `file://`→`supabase.co` requests with a
CORS error, serve the directory locally:

```powershell
cd supabase/admin
python -m http.server 8765
# then open http://localhost:8765/
```

## What's in it

- **Overview**: live counts of public / flagged drills, banned users,
  unique uploaders active today.
- **Drills**: search + filter by status, view full content, set
  status (Public / Flag / Hide / Delete), one-click ban the uploader.
  Multi-select for bulk delete + bulk flag.
- **Bans**: paste a user UUID + reason → 403 on their next upload.
  Reads / likes still work — bans are upload blocks.
- **Bulk upload**: drag-drop `.drill.txt` files. Headers are parsed
  client-side; you can edit the name and add an author handle per
  row before submitting. SHA-256 dedupe still applies (re-uploading
  the same content is a no-op, not a duplicate row).

## Security

- The `service_role` key has **full read/write** on the database.
  Anyone with it can wipe everything. Do not paste it into shared
  machines or commit it anywhere.
- Keys live in this tab's `sessionStorage` only — closing the tab
  forgets them. Reload doesn't, so you can refresh without
  re-pasting. Click **Forget keys** to wipe immediately.
- The HTML file itself ships zero secrets and is safe to commit.
