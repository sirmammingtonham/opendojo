# Reliability reversing, 2026-09-16

Current installed Polaris executable SHA-256:
`74FCEEDA116632E96EC93921A9D11E399DFEB04A4C16ACD3B9B41C6D3BAB3144`.
Addresses below use image base `0x140000000`, not live ASLR addresses.
Read-only Cheat Engine inspection used PID 44472, base `0x7FF68A1E0000`.
Ghidra auto-analysis was still running during this inspection.

## Player timer discovery

At `0x141960138`, the game updates three consecutive timers using the same
player pointer in RSI:

1. Load/increment/store `[rsi+0x1548]`, saturating at `0xFFFF`.
2. Load/increment/store `[rsi+0x15D0]`, saturating at `0xFFFF`.
3. Load/increment/store `[rsi+0x17A4]`, saturating at `0x0FFFFFFF`.

The second timer is the field the mod already uses for its automatic-load gate.
Both live players contained 65535 at that offset during the long-running
Practice session. This agrees with the saturation instructions, but does not
by itself prove round-readiness semantics. After the user reset Practice,
direct reads from the same player object observed 740 and then 1139, confirming
that the counter restarted and resumed incrementing. The timer sampler retained
only the original saturated sample, so it did not capture the transition to zero
or the intro interval; it was removed after the direct checks. Exact intro/input
readiness and character-switch observations remain necessary.

Implemented a unique 88-byte code signature with all nine field displacements
wildcarded, then decoded the second timer's displacement. The current installed
file contains exactly one match, at raw offset `0x195F738` / image address
`0x141960138`, yielding `0x15D0`. The decoder verifies agreement among each
timer's load and two stores, distinct fields, alignment, and bounded offsets.
There is no fallback to a fixed field offset. Missing discovery leaves character
detection available but makes the automatic-load readiness check return false.

Automatic loading now cancels its pending attempt when the readiness timeout
expires, instead of bypassing the gate. Manual loading remains available.
Unit coverage includes relocated fields, truncated input, inconsistent stores,
out-of-range offsets, and aliased fields. Release build and all eight existing
and extended test cases passed. The candidate has not been installed or tested
in-game yet.

## Native recording lifecycle leads

Pool initializer: `0x1418EDA70`. Its first 24 bytes in the installed executable
match the running game's bytes. A scan of direct relative CALL encodings found
one candidate call site, `0x141918FAB`; live disassembly confirms this call.
The caller obtains the recording subsystem, initializes its pool, then calls
`0x1418F0050` with mode-dependent arguments. This is a lead for understanding
the natural initialization path, not evidence that it is a safe import hook.

Live Practice-controller vtable: `0x1485651F8`.

| Slot | Function |
| --- | --- |
| 0 | `0x145C9BE40` (matches installed destructor hook) |
| 1 | `0x145C561A0` |
| 2 | `0x141213CC0` |
| 3 | `0x145C5FA10` |
| 4 | `0x145CA4280` |
| 5 | `0x145CAB220` |
| 6 | `0x145CA42D0` |
| 7 | `0x145CAB120` |
| 8 | `0x145CA42A0` |
| 9 | `0x145C9D550` |

Next: decompile these functions and trace callers once Ghidra analysis finishes;
identify the update thread and teardown ordering before moving any imports.
No game memory was patched or native game functions invoked during this work.

## Ghidra follow-up, 2026-09-17

The pool initializer and its caller now decompile. The caller is
`FUN_141918f10`, and invokes `FUN_1418f0050(recording, mode, index)` after
allocation. The latter stores mode at recording+0x64 and selects +0x60 or
+0x5C for the index. The caller also changes gameplay flags and player state,
so it is not a neutral replacement for the mod's import operation.

The checked Practice-controller methods still have no function definitions,
including the destructor already verified by the live vtable. The current MCP
bridge exposes decompilation and renaming but no function-creation operation.
`tools/ghidra/DefinePracticeFunctions.java` verifies the entire ten-entry table
before defining missing functions, preserving existing definitions. It compiled
successfully against the locally installed Ghidra 12.0.4 API. Run it in Ghidra
after Auto Analyze finishes, then retry the lifecycle decompilation.

## Verified session-finalization fixes

After the Python script defined the methods, the initial Practice methods
decompiled. Slots 5/6 save/restore recording settings; slots 7/8 save/restore a
larger gameplay-settings block. These are not per-frame update callbacks.
The live vtable actually contains 34 executable entries. The Python helper now
verifies and defines all 34; the remaining definitions still need its rerun.

Following the settings-restoration path reached native finalizer
`FUN_1419161f0`. Its disassembly exposed two existing incorrect writes:

- Native `MOV dword ptr [RCX+0x88],0` was implemented as singleton+0x22. The
  latter is the decompiler's uint-array index, not a byte offset.
- Native player-array entries point at allocation bases, while holder player
  pointers are base+0x90. Verified live for both players. The native session
  flag is allocation+0x3AE0, hence holder-player+0x3A50. The mod used +0x39C0.

The finalizer's 172-byte signature matches once in the installed executable
(raw offset 0x19157F0). Fields and RIP displacements are wildcarded. The mod
now decodes pending-state, holder-relative player-flag, and completion-byte
offsets from this function, validates their bounds and pointer-base relationship,
and disables finalization if discovery fails. No stale-field fallback remains
in `mark_session_loaded`.

Tests cover independently shifted fields, inconsistent pointer relationships,
truncation, whole-object write comparisons, and rejection when layout discovery
is unavailable. Release build and all nine tests passed. These fixes are in the
built candidate; live deployment and recording-load testing remain pending.

## Practice fiber lifecycle tracing

The expanded Python script successfully defined the remaining virtual methods.
The update loop is not itself a vtable method. Constructor `FUN_145c9a5a0`
registers `0x145CA0340` with `FUN_1458b1580`, phase 5, passing the controller
as the callback argument. It retains an unregister handle with cleanup function
`0x14587E2F0` in its +0x70 vector.

`FUN_1458b1580` allocates a task and passes it to `FUN_141761a50`, which can
create a Windows fiber and adds the task to the phase list under a critical
section. The callback initializes the Practice manager and enters a long-lived
loop: controller mode 2 calls `FUN_145ca3a80`; other modes call `0x145CA2E70`.
The live ordinary Practice controller's mode byte at +0x131 was 0.

The ordinary loop starts its repeated work at `0x145CA2EC0`, calling
`FUN_145ff5820` and checking controller+0x126. A temporary hardware execution
breakpoint recorded 311 hits with RSI equal to the live Practice controller.
The mode-2 loop probe recorded no hits in this session. This establishes the
correct live loop, not exclusive ownership of recording state.

Both loops call `FUN_141763fb0(task, 1)` to yield via `SwitchToFiber`. Upon
resumption it checks task+0x31 and task+0x33 and may throw a cancellation
exception. Cleanup `0x14587E2F0` sets task+0x31, clears task+0x28, and frees
the unregister handle. A hook at callback entry would not execute once per
frame and must not be used as an import dispatcher.

Thread identity is still unverified: CE's `getCurrentThreadID()` reported its
own callback thread (absent from the game's thread list), and its THREADID
global was zero. Neither is accepted as evidence of game-thread identity.
The sampled fiber stack was not matched to a thread with a read-only TEB scan.

All temporary breakpoints, including raw Lua one-shot probes, were removed.
CE's VEH debugger attach succeeded; its detach operation returned false.
No recording memory writes or game function calls were introduced by these
probes. Import dispatch remains unchanged until update/teardown ownership can
be established and transition tests completed.


## Additional layout discovery (September 17)

Implemented startup discovery of the subsystem hash-table layout from the complete
92-byte leaf function at `0x1418E0440`. The unique signature wildcards the context
map field, mask/sentinel/buckets fields, bucket stride and last pointer, node key,
previous pointer and value. Both key comparisons must agree; alignment, signed
operands, overlaps and bounds are checked. No native lookup call is made. Runtime
reads retain the 64-step walk limit, mask checks and guarded memory access.

Movelist discovery anchors on the unique wrapper at `0x14191A8B0` and follows its
actual calls to the element accessor (`0x141909600`) and move getter
(`0x141909C00`). Complete callee shapes are checked within `.text`. It decodes the
human-side offset, vector end, element stride and move-ID base. The division magic
and shift must agree with the stride used for indexing. Unsupported instruction
shapes disable movelist access. Read, write, capture and import use this layout;
reference constants in slot.hpp are no longer used by runtime movelist accesses.

Pool2's global is now decoded from its own native initializer reference, instead
of assuming pool1+8. All four added patterns match exactly once in the current EXE.
Synthetic patch tests relocate movelist fields and table fields, test collision
traversal and cycles, and verify rejected imports leave destinations unchanged.

The remaining layout items listed at this point were addressed by the pass below.
Import execution on the game's owning thread remains a separate lifecycle issue.


## Final offset pass (September 17)

All previously identified runtime field offsets now come from native instructions
or reflection, or are part of an explicitly checked supported ABI/recording format.
Historical addresses in these notes and test fixtures are provenance, not runtime
fallbacks. Removed the unused reference offset constants from the public slot header.

| Access | Native evidence and behavior |
| --- | --- |
| Context global | Follow movelist wrapper -> record-pool accessor -> context getter; verify subsystem key and lookup target. No neighboring-function anchor. |
| Slot flags | Setter `141924510` decodes flag base (currently `484`), checks zero index bias and supported eight-slot/eight-byte-entry shape. Existing first-bank behavior retained. |
| Session active flag | Finalizer's bit-test immediate supplies the active mask; read/modify/write the uint32 preserving unrelated flags. |
| Session scalar | Getter `14191FE10` supplies the current `+8` uint32 field; previous byte write corrected to native width. |
| Pause | Routine `1418B0010` supplies current `+65`; three native stores must agree. |
| Side record | Reset `1418842A0` supplies record base `D0` and stride `160`; called initializer `14187F9C0` supplies member `2C`. Existing target is `D0 + 160 + 2C = 25C`. |
| Recording state | Setter `1418F0FA0` supplies current `+28`, checked against adjacent native field and pool initializer. |
| Players | Refresh `145E7FD50` supplies holder P1/P2 fields `30/38`; actual called accessor supplies bias `90`. Independent Jack and Alisa predicates agree on character field `168`. Human side uses gameplay discovery; old info chain removed. |
| UE metadata | Native field iterator, iterator-next, native-invoke, property-step and ProcessEvent supply class/name/flags, children/properties, field links, native-function and property offsets. |
| Parent class | Native iterator reveals super-getter virtual slot; decode actual simple getter to obtain parent field without calling it. |
| ProcessEvent | Unique common vtable entry on the resolved UClass and UFunction objects; no fixed slot 77. |
| Names | Two agreeing references in native name decoder supply FName pool; derive block-array and text offsets. No RVA hint or heuristic fallback pool scan. |
| Native parameters | ProcessEvent's two parameter-copy lengths supply UFunction parameter-size field. Reflect parameter names/offsets and verify exact sizes before calling. |

The `+25C` field was previously described incorrectly as a global count. It belongs
to the second per-side record and is initialized as part of a qword pair of `-1`
values. This pass derives its address and preserves the existing write behavior;
it does not establish new semantics for writing `1`, or change flag-bank selection.

Supported format/ABI boundaries remain deliberately explicit:

- Live recording codec: uint16 count, 1800 four-byte events, 7202-byte slot. Native
  allocation, copy pitch and event consumer must agree before any live pool access.
  A format change needs a deliberate file migration, not a blind new stride.
- Eight user slots and existing flag-bank behavior: checked native setter shape.
- FString: validate the actual reflected SetRawText wrapper and its copy/resize
  callees, including count/capacity fields and two-byte character arithmetic.
- TArray returned by GetObjectsOfClass: validate enumerator and actual append
  callback, including count/capacity fields and pointer element stride.
- Parameter records: SetRawText `raw_text` at 0, `ReplaceUnsupportedCharacter` at
  16, size 17; GetString `textId` at 0, `ReturnValue` at 16, size 32. Verified live
  by read-only CE inspection. Changed metadata disables calls.
- FName encoding: block/index split and string-header encoding are supported ABI
  assumptions checked by native decoder shape and live `None` entry validation.

No unresolved address falls back to a historical RVA. A compiler rewrite or native
behavior/format change can still require a mod update. Pattern matching is not a
proof that every future patch will be compatible. Missing/ambiguous discoveries
and disagreeing operands disable dependent access and log the reason.

### Verification

`native_layout_tests` maps the current EXE into non-executable memory and registers
its unwind table, then runs the production resolver. It never executes game code.
It verifies current fields and simulates consistent shifts of slot/session/player/
reflection fields, an independently relocated pool2, and changed neighboring
context-getter bytes. Separate cases reject conflicting fields, incompatible
recording format, and a broken context call chain. Additional ABI mutations reject
changed FString and returned-array layouts.

Import tests exercise relocated destinations and bit masks, preservation of
unrelated singleton flags, clearing and recording writes, and zero writes when
required discovery or recording compatibility is unavailable. Existing table,
movelist, timer, file handling and proxy tests remain in the suite.

Run with the local executable (not checked into the repository):

```powershell
cmake -S dll/tests -B dll/build-review-tests -DOPENDOJO_TEST_GAME_EXE="E:/Steam/steamapps/common/TEKKEN 8/Polaris/Binaries/Win64/Polaris-Win64-Shipping.exe"
cmake --build dll/build-review-tests --config Release
ctest --test-dir dll/build-review-tests -C Release --output-on-failure
```

The release candidate is built in `dll/build-review` with automatic deployment
disabled. In-game validation of this final candidate is still pending. Import
thread ownership/lifetime and transition safety are not solved by offset discovery.


Offset-pass local result (superseded by the lifecycle follow-up below): Release build succeeded; all 14 CTests passed, including all
five inert executable scenarios. `git diff --check` reported no whitespace errors.
Candidate SHA-256:
`74D56C7FA947C19683C70A4197F65B630D4BCF0E39BBBDBA36CEA67ACE72963B`.
The game was still running at completion, so this candidate was not installed.


## Native ownership and lifetime follow-up (2026-09-17)

The earlier import-thread ownership limitation is addressed in this candidate.
All addresses below are reversing evidence for the current executable, not runtime
fallbacks. Discovery requires unique instruction contracts and agreeing references.

- Scheduler `141760900..141760E65` dispatches fibers and worker tasks, waits for
  each worker completion semaphore, and performs cancellation cleanup. Its phase-3
  caller `145BC54A0..145BC5564` then invokes deferred cleanup (`1417607F0`).
  The dispatcher pumps only after this caller returns and a phase-3 scheduler
  completion was observed on the same thread for the discovered manager.
- Init sequence `140A6F487..140A6F4A7` stores Windows GetCurrentThreadId into
  Unreal's game-thread ID. The resolver verifies the imported API identity and
  derives the global. Runtime thread checks gate the dispatcher and menu hooks.
- Native free wrapper `142EF7810..142EF7844` releases engine allocations.
  Returned GetObjectsOfClass arrays and GetString return buffers now use RAII.
- Weak assignment `1431B6D30..1431B6D63`, validity `1431BBFA0..1431BBFEE`,
  and get `1431BA7E0..1431BA86D` provide index/serial identities. The resolver
  checks the eight-byte handle ABI and matching registry references. Validity
  rejects native garbage/pending-kill/unreachable flags before resolving pointers.

Manual loads enqueue decoded data, with a capacity of eight and a five-second
expiry. Each request checks its session generation at execution; lifecycle changes
cancel pending work. UI reads use immutable owned snapshots, refreshed at 100 ms
intervals and immediately after imports. Transition autosaves accept the departing
character's recent snapshot without reading its replaced game objects or labels.
Native mutation entry points reject callers outside the scheduler callback.

The local suite now includes current and changed runtime/weak-reference contracts,
off-thread mutation rejection, snapshot isolation and stale-session reads, plus
queue cancellation, expiry, reentrancy, exception isolation and concurrent producers.
Live entry, reset, character switching, menus and autoload behavior remain user-owned
validation. This pass makes no claim of live testing or deployment.

Lifecycle follow-up result: Release build succeeded; all 17 CTests passed. The
SetTextID reinstall path preserves its original thunk and rejects a changed thunk,
preventing recursion when rediscovery finds an already-patched UFunction.
Candidate: dll/build-review/Release/dinput8.dll
SHA-256: ECEA328FBAAD43FD0615AB5B9BA577E04994DF403920F3F60B3CBEA66AFACA33
Automatic deployment is disabled; this candidate was not installed by this pass.
