# Patch discovery refinement - 2026-09-17

## Scope

This work refines discovery, ABI validation and offline tests. It does not change
controller input, import cancellation policy, player refresh behavior or menu
renaming behavior. The independent renaming agent's changes are preserved.

## Discovery strategy

Short anchors locate candidates in executable code. The resolver then decodes the
owning function using chained Windows unwind records and MinHook's pinned HDE64
decoder. Small helpers without unwind records use bounded reachable-instruction
traversal. Discovered operands and actual call/branch targets provide fields and
relationships; there are no historical-address fallbacks.

Instruction fragments tolerate alignment NOPs and equivalent short/near branches.
Critical branch destinations, repeated field references, allocation sizes and
caller/callee relationships must agree. Multiple validated candidates or excessive
anchor candidates reject discovery. Match results own their instructions so they
remain valid after the decoded function view is destroyed.

## Refined paths

- Scheduler completion: replaces the former 196-byte wrapper and 1,381-byte
  scheduler signatures with a 14-byte phase-3 call anchor and dispatch/join/cleanup
  contracts. Actual Windows imports identify release, fiber switch and infinite
  wait operations. Returning/escaping paths that bypass completion are rejected.
- Practice teardown: body anchor, vtable entry identity, singleton clear and
  deletion path identify the destructor and practice global.
- Player refresh: native getters, initialized player indices, distinct player
  stores and pointer bias establish player roles. Character predicates independently
  confirm the character field through decoded control-flow contracts.
- Pool allocation: both allocation/clear paths agree on sizes and allocator/free
  targets; repeated references identify each pool independently of adjacency.
- Session finalization: derives pending, completion and player flag fields and
  verifies the native/holder pointer relationship against player discovery.
- Movelist: follows the wrapper's actual element/field helpers. Bounds-check
  division must agree mathematically with element stride; field/index relationships
  are checked. Legacy byte-position decoders were removed.
- Slot flags and recording state: decoded bounds, stores, reset loops and field
  agreement establish the layout without fixed positions in matched functions.
- Recording format: validates the count, payload, event stride, duration/key
  interpretation, copy sizes and pool relationships. Unrelated random-number
  bookkeeping is excluded from the consumer contract.
- Subsystem/context: validates the hash traversal graph and derives its operands;
  follows the actual accessor getter/key/tail-call chain. The recording-pool key
  remains an identity requirement.
- Reflection: field iteration, invocation, ProcessEvent, property stepping and
  name decoding derive fields from actual operands and cross-check relationships.
  The former 502-byte finder and 217-byte iterator patterns are also removed.
- Allocator/weak references: short anchors plus decoded operation graphs tolerate
  branch widening and alignment while preserving null, serial, bounds and flags
  checks. Valid/get helpers must agree on object storage and serial fields.
- FString/object arrays: follow the reflected wrapper's actual callees and validate
  count/capacity locations, element width, growth and stores. Former full-wrapper
  byte signatures and fixed call positions are removed.
- Thread ID: validates the actual GetCurrentThreadId import and derives the native
  thread-ID store. Unrelated following calls are no longer part of its anchor.

Scheduler/thread availability and allocator/weak-reference availability remain
independent: a menu helper failure does not discard working import dispatch, and
an invalid dispatcher does not discard working UObject discovery.

## Deliberately strict requirements

The recording codec remains uint16 count plus 1,800 four-byte events (7,202 bytes).
The FString and object-array representations used by the mod remain explicit ABI
requirements. Weak-handle representation, character identity constants and the
recording subsystem key are not indiscriminately wildcarded.

This is instruction-shape validation, not arbitrary semantic equivalence. Anchors
still require their own contiguous bytes. Register reassignment, changed addressing
forms, inlining that removes a required hook boundary, unsupported instructions,
changed data representations and rewritten algorithms can require an update.
Harmless changes inside one of these retained shapes may still disable discovery.
No available historical executable corpus establishes a numerical patch-survival
probability. Future-version compatibility and live behavior remain unverified.

## Validation

The production resolver runs on a read/write inert copy of the current executable.
PE absolute relocations are applied; no game instruction executes and no executable
on disk is modified. Tests register unwind metadata only for discovery.

All 27 tests passed. Coverage includes current and consistently moved fields,
conflicting fields, incompatible recording format, invalid context/thread/weak
contracts, scheduler API and completion bypasses, ambiguous scheduler candidates,
independent feature failure, movelist relocation and conflicting division/indexing.
After the final thread anchor simplification, the affected current-image and
thread-rejection tests and instruction-graph unit tests passed again.

The new instruction_shift scenario relocates 13 whole native functions in the
inert image, inserts a NOP, widens short branches, repairs relative/RIP references
and absolute code pointers, removes old bodies and registers replacement function
ranges. Production discovery still resolves the expected layouts. The test covers
teardown, refresh, allocation, finalization, movelist wrapper, slot flags, recording
state paths, invocation, ProcessEvent and name decoding. It does not model arbitrary
compiler optimization or execute unwinding through the synthetic functions.

Decoder tests additionally cover owned match lifetime, reachable leaf boundaries,
indirect tail dispatch, overlapping instruction rejection, out-of-range branches,
branch-target disagreement, duplicate fragments, truncated instructions and XCHG
not being mistaken for a NOP. Offline resolver cases take about 2.1 seconds each on
this machine; this is not an in-game performance measurement.

## Candidate

Release build succeeded with no deployment and no global source autoformatting.
Build directory: dll/build-pattern-review (separate from the renaming agent).

Candidate: dll/build-pattern-review/Release/dinput8.dll
SHA-256: CD9ABEBBC2401074CFA4CBFCB3B762039329AC2FBBF3D9A7B2696022792E9F14

Not installed. In-game validation belongs to the user. The renaming agent may
continue changing its implementation after this candidate was built.
