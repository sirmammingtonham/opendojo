Recording names use the existing per-recording `name:` field. No new format,
version gate, cloud column, or server deployment is required. Blank labels remain
blank through export and import. Names are included in the content hash, so
changing a recording name produces a distinct cloud upload.

The compatibility test compiles the actual decoder at
`de76098779ffcc45a78aa4f3751eb6b50a1eee19` (before native slot renaming), using
the unchanged recording data model. It checks that current exports retain their
recording count, input bytes, kinds, and move IDs in that decoder. Older mods
do not gain native-menu renaming. Their parser treats `#` as a comment, so they
may shorten such a name; the recording remains playable. Current mods preserve
the complete name. This does not make move IDs portable across game patches.

Run from the repository root in PowerShell (Git history must contain that commit):

```powershell
cmake -S dll/tests/renaming -B dll/build-renaming-tests -A x64
cmake --build dll/build-renaming-tests --config Release
ctest --test-dir dll/build-renaming-tests -C Release --output-on-failure
$env:OPENDOJO_NAMED_DRILL = (Resolve-Path dll/build-renaming-tests/named-drill.txt).Path
npx --yes deno test --allow-read --allow-env=OPENDOJO_NAMED_DRILL supabase/functions/submit_drill/validate.test.ts supabase/functions/submit_drill/renaming.test.ts
```

The cloud test passes actual C++ encoder output through a JSON round trip and
the production submit validator, asserting that content is unchanged. The server
stores that content verbatim; the download API returns it and the mod saves it
with an optional drill-level cloud ID. This test does not publish a live drill
or exercise production authentication, database access, or network availability.

In-game check: edit a populated slot, clear its name, hover/focus/reopen the
practice menu, then save/share and reload a drill containing both named and
unnamed slots. Check the export table at the supported menu scales: fields
should match text row height, remain within the Name column, scroll while
editing long text, and show the full label on hover.
