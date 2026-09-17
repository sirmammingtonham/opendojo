# Reliability follow-up

## File handling fixes (2026-09-16)

- Config and upload identity saves now use flushed temporary files and atomic
  replacement. Failed writes preserve the existing destination.
- Migration retains legacy identity and author files until the destination is
  saved successfully. Embedded credentials stay in config when identity saving
  fails. Failed migrations can retry on the next load.
- Unexpected JSON types in author and authentication fields return defaults
  instead of throwing from accessors.
- Drill browser header parsing enforces its 4 KiB budget while reading, including
  files containing a single oversized line. Incomplete reads of full drill files
  are rejected rather than passed to the decoder.

Regression coverage uses isolated temporary directories and synthetic identity
values. Cases include blocked identity destinations, locked settings files,
migration retry, malformed field types, and oversized and unterminated lines.

The reported controller Practice-entry regression was withdrawn by the user.
The experimental controller/proxy changes were reverted and are not part of
this follow-up.

## Patch resilience and remaining validation

The September 17 reversing pass replaced the previously identified fixed gameplay,
player, session, subsystem and Unreal metadata offsets with code/reflection
discovery. Native checks gate the supported recording format, FString, returned
object arrays and UFunction parameter records. No historical RVA fallback remains.
See [RE_RELIABILITY_2026_09_16.md](RE_RELIABILITY_2026_09_16.md) for evidence,
compatibility boundaries and executable-level simulated patch tests.

Changes to instruction shapes, native behavior, slot/recording formats or engine
ABIs may still require an update; unsupported layouts disable dependent operations.
The remaining ownership and resource-lifetime implementation is now complete:

- Imports, autoload, session activation and native menu work run after the native
  scheduler joins workers and completes cleanup, gated by Unreal's game-thread ID.
- Manual imports use a bounded queue. Session changes, round-counter resets,
  player refresh and teardown invalidate pending requests; expired requests cancel.
  Mutation APIs reject calls outside this update boundary.
- UI reads and transition autosaves use owned recording snapshots, including
  character identity and slot labels. Snapshots refresh at most ten times per second;
  successful imports publish immediately. Missing/stale snapshots preserve existing saves.
- Cached menu objects use Unreal weak identities (index plus serial number).
  Invalid objects trigger rediscovery, including Blueprint property metadata.
- Returned object arrays and localization strings have scoped cleanup through the
  discovered engine allocator; native calls are confined to the game thread.

In-game behavior and validation remain with the user. These changes address the
remaining implementation findings; they do not guarantee compatibility with arbitrary
engine rewrites or establish that the candidate has passed live regression testing.


## Manual-load cancellation regression (2026-09-17)

Live validation found manual loads cancelling while synchronous autoload succeeded.
The player-refresh detour unconditionally advanced the session generation before
calling the native service-locator refresh. Republishing unchanged P1/P2 pointers
therefore cancelled queued loads. The detour now compares the discovered P1/P2
fields before/after the original and the published CPU identity; only a change or
unreadable state invalidates queued work. Controller teardown and the scheduler's
session/round checks remain enabled. Cancellation logs include request age and
submitted/current generations.

The new regression test executes the production detour with a simulated native
refresh: 120 unchanged refreshes preserve a queued manual load. P1/P2 replacement,
character changes at unchanged addresses, teardown, and read failure cancel it.
The focused player-refresh, session-queue, import and session-write tests pass.
A rebuilt candidate requires a game restart and user validation; no hot replacement
of the running DLL is performed.
