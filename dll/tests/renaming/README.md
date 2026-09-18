Recording names use the existing per-recording `name:` field. No new format,
version gate, cloud column, or server deployment is required. Blank labels remain
blank through export and import. Names are included in the content hash, so
changing a recording name produces a distinct cloud upload.

Legacy exports generated literal `slot 1` through `slot 8` names for unnamed
recordings. The decoder treats those exact lowercase strings as unnamed,
including imports into a different destination slot. Other names are retained.
Loading does not rewrite existing files.

The compatibility test compiles the actual decoder at
`de76098779ffcc45a78aa4f3751eb6b50a1eee19` (before native slot renaming), using
the unchanged recording data model. It checks that current exports retain their
recording count, input bytes, kinds, and move IDs in that decoder. Older mods
do not gain native-menu renaming. Their parser treats `#` as a comment, so they
may shorten such a name; the recording remains playable. Current mods preserve
`#` within the 32-code-point name limit. This does not make move IDs portable
across game patches.

The UI, parser, slot store, and encoder share a bounded UTF-8 validator. Names
are capped at 32 Unicode scalar values / 128 bytes, leaving a terminator in the
129-byte UI buffer. Malformed UTF-8, embedded NUL, control characters, and
invisible direction formatting terminate the accepted prefix. Long legacy
names are shortened, so they do not make otherwise valid drills unloadable.
Parser/store normalization allocates only the bounded prefix, not the original
untrusted name. The original downloaded file remains unchanged on disk.

Tests include malformed and truncated UTF-8, overlong encodings, surrogates,
out-of-range code points, controls, megabyte names, format-string payloads,
and 10,000 deterministic random-byte inputs. Sanitized output is independently
checked with Windows' strict UTF-8 conversion. For AddressSanitizer, configure
the same test project with `"-DCMAKE_CXX_FLAGS=/fsanitize=address /EHsc /Zi"`
and `"-DCMAKE_EXE_LINKER_FLAGS=/DEBUG"`, then run Release tests with the Visual
C++ toolchain's `bin/Hostx64/x64` runtime directory on PATH.

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
stores that content verbatim; the download API returns it and the mod decodes
and re-encodes it with the listing's current name, description, author handle,
and an optional drill-level cloud ID. Author metadata survives local copies;
older decoders safely ignore that optional header. This test does not publish a live drill
or exercise production authentication, database access, or network availability.

In-game check: edit a populated slot, clear its name, hover/focus/reopen the
practice menu, then save/share and reload a drill containing both named and
unnamed slots. Check the export table at the supported menu scales: fields
should match text row height, remain within the Name column, scroll while
editing long text, and show the full label on hover.
# Multiline descriptions

The normal decoder and library header reader accept legacy raw continuation
lines after `description:` until a known drill header or a `---` recording
boundary. Blank lines, indentation, colons, and hashes are retained. CRLF and
CR line endings normalize to LF. A reserved header at the beginning of a line
remains structural; the parser does not guess whether it was intended as prose.

New multiline exports include the flattened `description:` fallback for older
mods, followed by `description_line: |` fields containing each literal line.
The pipe separates syntax from content, preserving leading/trailing whitespace
and preventing descriptions from injecting headers or recording markers. Older
decoders ignore those fields and keep the playable recordings.

Tests cover the historical decoder, library-reader parity, malicious section
text in descriptions, NUL rejection, the 4096-byte description limit, recording
count/event limits, and an optional actual downloaded fixture as argv[2].
