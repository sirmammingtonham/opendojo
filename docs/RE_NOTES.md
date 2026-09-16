Tekken 8 / Polaris-Win64-Shipping.exe v3.00.02 — reverse engineering findings.

Living document. Append; don't reorganize. Each block of findings should be self-contained.

Module base in user's environment: 0x7FF703A70000 (offsets here are RVAs from module base unless noted). Ghidra image base: 0x140000000.

----

UE5 version is approximately 5.2-ish based on layout conventions. Standard UE5 reflection system. No symbols in shipped binary; every function is FUN_xxxxxxx in Ghidra. Strings are present for class names but not function names (the registration tables associate name strings with stub addresses).

----

FNamePool location: module + 0x9955480.

Pool layout:
- pool + 0x10..0x10+N*8 : array of block pointers (Blocks[N], each Blocks[i] points to a contiguous block of entries).
- Each block holds entries packed back-to-back.

FName.idx encoding (32 bits):
- bits 0..15: stride (entry byte offset within block = stride * 2).
- bits 16..28: block index (13 bits).
- bits 29..31: probe hash bits (only used when stored in hash tables; mask off for the real index).

Entry header at entry+0 (uint16_t):
- bit 0: bIsWide (1 = UTF-16, 0 = ASCII).
- bits 1..5: probe hash bits (ignore for decoding).
- bits 6..15: length in characters (10 bits, max 1023).

Entry data is at entry+2, length * (bIsWide ? 2 : 1) bytes.

Decoded by FUN_142fee6b0 (FName::Add hash table) and FUN_142ff82d0 (FName lookup).

Self-test values (assuming the analyzed build): FName 0 → "None", FName 249832 → "PolarisUMGTextMenu", FName 999 → "ID".

----

UObject layout in this build:
- +0x00: vtable.
- +0x08: ObjectFlags (uint32).
- +0x0C: InternalIndex (uint32) — index into GUObjectArray's pages.
- +0x10: ClassPrivate (UClass*).
- +0x18: NamePrivate (FName, 2x uint32).
- +0x20: OuterPrivate (UObject*).

EObjectFlags (subset, decoded from observation):
- 0x00000008 RF_Transactional
- 0x00000010 RF_ClassDefaultObject
- 0x00000020 RF_ArchetypeObject
- 0x00000040 RF_Transient
- 0x00080000 RF_WasLoaded
- 0x00100000 RF_TextExportTransient
- 0x00200000 RF_LoadCompleted
- 0x20000000 RF_PendingKill (caution — appears as 0x200000 with other bits set, double-check before relying on this bit)

To distinguish CDO vs live instance: `(obj_flags & 0x10) != 0` for CDO.

----

UStruct layout (UClass / UFunction / UScriptStruct all extend UStruct):
- +0x28: UField::Next.
- +0x40: SuperStruct (UStruct*).
- +0x48: Children (UField*) — head of UFunctions for a UClass.
- +0x50: ChildProperties (FField*) — properties on UStructs. NOTE: for UClass on this build, ChildProperties is null. Properties for UFunctions are at this offset.

UFunction-specific fields (within the 0xE0-byte UFunction object):
- +0x50: ChildProperties (FField* — head of parameter list).
- +0xD8: Func (native handler function pointer).
- (Note: 0xD8 is non-standard. UE5 stock layout has Func at +0xC8.)

FField layout:
- +0x00: vtable.
- +0x08: ClassPrivate (FFieldClass*).
- +0x20: Next (FField*).
- +0x28: NamePrivate (FName).

FProperty (extends FField, +0x38..):
- +0x38: ArrayDim (int32).
- +0x3C: ElementSize (int32).
- +0x40: PropertyFlags (uint64).
- +0x4C: Offset_Internal (int32).

FFieldClass type identifiers (singleton UClass-like pointers in this build):
- 0x7FF70D469AA0: IntProperty (size 4).
- 0x7FF70D4695F0: BoolProperty (size 1).
- 0x7FF70D469CD0: NameProperty / FName (size 8).
- 0x7FF70D469730: StrProperty / FString (size 16).
- 0x7FF70D46B080: TextProperty / FText (size 16).
- 0x7FF70D4686B0: ByteProperty / EnumProperty (size 1).

(These addresses are absolute on the user's machine; subtract module base for RVAs if reproducing.)

----

GUObjectArray globals:
- module+0x99FB530: pages array (FUObjectItem**).
- module+0x99FB544: count (int32 — but this value's exact semantic is uncertain; observed not changing on every NewObject. Treat as a hint, not a count of total live objects).

FUObjectItem layout (24 bytes):
- +0x00: UObject* Object.
- +0x08: int32 Flags.
- +0x0C: int32 ClusterRootIndex.
- +0x10: int32 SerialNumber.

Page layout: pages[i >> 16] is a block of FUObjectItems indexed by (i & 0xFFFF). Each block is ~65536 entries.

----

Useful FNames worth knowing (by index — decode via FNamePool):
- 0: "None"
- 999: "ID" — most common UFunction parameter name.
- 122451: "Text" — also "Message" in some contexts.
- 137777: "Param".
- 162155: "Label" / "Title" variants.
- 68056: typically the UWorld class name.
- 233439: PolarisBattlePlayerController FName.
- 249832: PolarisUMGTextMenu.
- 249821: PolarisUMGHudViewer (subclass of TextMenu).
- 252320: PolarisUMGMessageChecker (subclass).
- 254427: PolarisUMGStoryTournamentViewer (subclass).
- 249169: PolarisUMGDialog.

UPolarisUMGTextMenu has 3 direct subclasses, all non-generic: HudViewer, MessageChecker, StoryTournamentViewer. None are list-style menus the way the API surface implies.

----

UPolarisUMGTextMenu API (17 native UFunctions, decoded from class registration table at fixed RVAs near 0x142cb7xxx — see Polaris UPolarisUMGTextMenu UClass children walk). All methods callable in principle via ProcessEvent.

Method list (decoded names, params, native impls):
- AddCommand: (int id, FString[5] labels, FName cb, int param, int extra, bool selectable). Exec stub 0x142cb7bc0; native impl TBD. Params take a 0x69-byte stack frame.
- AddCurrentMenuParam.
- ExecCommand: (this, int32). Exec stub 0x142cb7f20. Native impl 0x142e8a650.
- GetCurrentMenuId, GetCurrentMenuIndex, GetCurrentMenuParam, GetCurrentMenuText.
- GetMenuParam, GetMenuText.
- RefreshCommand: (this, int32 id). Native impl 0x142e8ffa0. Param is row ID, not index.
- SetCurrentMenuParam, SetMenuParam, SetSelectable.
- StartTextMenu: (this, int32). Native impl 0x142e924b0.
- SubCurrentMenuParam.
- TransitionNextMenu, TransitionPrevMenu.

Internal row hashtable in UPolarisUMGTextMenu instance:
- +0x288: hashtable storage (TArray of 0x70-byte entries).
- +0x290: current "live" count.
- +0x298: hashtable directory pointer.
- +0x2A8: alternative directory (overflow / spill).
- +0x2B0: capacity.
- +0x2C0..0x2D0: secondary index buckets.
- Per-entry (0x70 bytes): +0x00 ID, +0x24 text widget binding, +0x34 param widget binding, +0x44 raw text, +0x54 something convertible, +0x5C flag/state, +0x64 bool, +0x68 chain-next.

UPolarisUMGDialog also has 17 UFunctions but all share BP dispatcher FUN_14317da00 (no native bodies). Dialog is pure-BP.

----

UMG widget construction:

CreateWidget — two variants in the binary:
- FUN_144842240 at module+0x4842240: BP-level CreateWidget. Takes (APlayerController*, UClass*, FName). Returns void (widget pointer is in RAX though — Ghidra's analysis treats it as void). Validates PC has a local player (vtable+0x780 check) and PC[+0x338] != 0. Calls FUN_144841d20 internally.
- FUN_144842900 at module+0x4842900: alternative CreateWidget. Takes (UObject* outer_or_world, UClass*, FName). Branches on param_1[+0x1B8] — if non-null, treats input as a more complete context (e.g., World), otherwise uses input directly. This is the variant the game uses for the practice menu.

FUN_144841d20 at module+0x4841d20: the actual NewObject path. Calls FUN_142f23c40 (StaticConstructObject_Internal). Takes (outer, class, name, world, ?).

AddToViewport — multiple wrappers:
- FUN_14483fe70 at module+0x483fe70: the canonical AddToViewport wrapper. Takes (UUserWidget*, int32 mode). Reads widget's world via vtable[+0x188]. Builds slot_params and calls FUN_144787cc0.
- FUN_14485c790: AddToViewport BP exec stub (for UMG-derived classes).
- FUN_144787cc0 at module+0x4787cc0: thin wrapper that forwards to FUN_144787400.
- FUN_144787400 at module+0x4787400: the real UGameViewportSubsystem::AddWidget implementation.

The AddToViewport slot_params struct (passed to FUN_144787cc0):
- bytes 0..15: FMargin Offsets (4 floats: L, T, R, B).
- bytes 16..31: FAnchors (4 floats: Min.X, Min.Y, Max.X, Max.Y).
- bytes 32..47: FVector2D Alignment (2 doubles in UE5.4-ish).
- bytes 48..63: another vector field (purpose unclear).
- bytes 64..67: ZOrder (int32).
- bytes 68..71: padding.
- (bytes 64..71 in the BP exec stub also carries a widget FName via `local_18 = CONCAT44(...,uVar1)`, but FUN_14483fe70 explicitly puts the `mode` arg there as ZOrder.)

The .rdata block at module+0x6da07a0 (anchor defaults the game uses) is 16 bytes that read as 4 floats `(0.0, 1.875, 0.0, 1.875)` or 2 doubles `(1.0, 1.0)`. When AddToViewport reads them as 4 floats, the resulting anchors are degenerate (Min == Max, Y=1.875 off-screen). The game still uses this — possibly the widget overrides the anchor internally or relies on overrides set elsewhere. Hardcoding `(0, 0, 1, 1)` as 4 floats produces full-screen anchors.

For top-level overlays, the canonical mode is 0x80 (used by WBP_UI_PracticeMenu_C). Other observed modes: 3 (used by sibling WBP_UI_Practice_S2_C).

----

Widget rendering observation: setting `widget[+0xE1] |= 0x20` is what FUN_144787400 does on success. This is the "added to screen" flag. Top-level widgets added via AddToViewport have this set. Sub-widgets (UMG children embedded in another widget's WidgetTree) DO NOT have this bit — they're rendered as part of their parent's slate tree.

Widget visibility cache at widget+0xE5. The actual visibility comes from the slate object's byte at slate+0x17c, mapped to UMG enum 0..4 via globals DAT_149611460..464. Standard UE5 ESlateVisibility convention (0=Visible, 1=Collapsed, 2=Hidden, 3=HitTestInvisible, 4=SelfHitTestInvisible).

Visibility setter wrapper FUN_142e90970(widget, char param): if param==0 sets to UMG enum 1, if param!=0 sets to enum 4. Neither sets to Visible (0). The game's call FUN_142e90970(widget, 0) effectively sets Collapsed — which is suspicious because game widgets are visible. The widget appears to default to Visible from construction and this call seems to be a precondition reset rather than the actual "show" trigger.

----

Slate refs: UMG widgets store their slate hook at widget+0x100 (an 8-byte pair: object + ref-counted control block, then a second pair at +0x110). These are TSharedRef<SObjectWidget>. Non-null after Initialize() runs.

WidgetTree at widget+0x228. Child count (uint32) at widget+0x230. Each child is a UWidget*.

----

Widget per-instance vs class-level fields (from byte-level diff of live LIVE[0] vs OUR constructed instance of WBP_UI_PracticeMenu_C):
- +0x008..+0x028: per-instance UObject header.
- +0x028: matches between live and ours (template vtable, class-level).
- +0x040, +0x048: template asset pointers, identical across instances.
- +0x100..+0x118: per-instance slate refs.
- +0x148: a vtable-y address, class-level.
- +0x150..+0x180: floats (1.0 patterns) representing FVector2D scale defaults — class-level.
- +0x228: WidgetTree pointer (per-instance).
- +0x230: child count (per-instance, 12 for WBP_UI_PracticeMenu_C).
- +0x280..+0x288: input flag bytes.
- +0x290..+0x4D0: 10 DELEGATE BINDING SLOTS (each ~0x40 bytes, holding function pointer + receiver). Populated by an EXTERNAL game-side manager, NOT by the BP construction script. Without these the widget exists but has no event lifecycle.

For sub-widgets (e.g., WBP_UI_PracticeMenu_Menu_3_C):
- +0x020 (Outer): a UWidgetTree of the parent widget.
- +0x030: UCanvasPanelSlot* (positions widget in parent's canvas).
- +0xE1: bit 0x20 NOT set (the widget wasn't added to viewport — it's a CanvasPanel child).

For top-level widgets (e.g., WBP_UI_PracticeMenu_C added to viewport):
- +0x020 (Outer): BP_PolarisGameInstance_C.
- +0x030: NULL (no slot — it's a viewport overlay).
- +0xE1: bit 0x20 SET (added to viewport).

So +0x030 nullness alone doesn't predict invisibility. The OUTER widget has +0x030 NULL and is visible. The INNER sub-widgets have +0x030 set and are visible because of their parent.

----

PlayerController findings (specific to Tekken):
- PC class FName: 233439 ("PolarisBattlePlayerController" or similar; decode via FNamePool).
- PC+0x328: byte field with bit 1 (mask 0x02) = "is local player". Used by CreateWidget validation.
- PC+0x338: pointer to Player (ULocalPlayer*). Null for remote/dummy PCs.
- PC vtable+0x780: function returning bool. Confirms "local player" status. Both PC+0x328 bit 1 AND vtable[+0x780] must pass for CreateWidget to succeed.
- PC vtable+0x188: function returning UWorld* (or world-like object). Used by AddToViewport.

Practice PC discovery via GetObjectsOfClass with `/Script/Polaris.PolarisBattlePlayerController`, exclude_object_flags=0x30 (skip CDO + Archetype). Typically one local PC instance in practice mode. Filter to local-with-player using PC+0x328 bit and PC+0x338 non-null.

----

World layout findings:
- World class FName: 68056 (decode for confirmation).
- World+0x138 / +0x13A area: contains EWorldType byte. Valid types per FUN_1454f2fa0's mask 0x6A: 1=Game, 3=PIE, 5=GamePreview, 6=GameRPC. In practice mode this is typically 1.
- The world is reached via PC vtable[+0x188]() — returns the world the PC is in. Crashes if PC has no Player.

----

UGameViewportSubsystem location/usage:
- Class lookup: `/Script/UMG.GameViewportSubsystem`.
- One live instance per session (find via GetObjectsOfClass).
- Has hash map of viewport-attached widgets at instance+0x30.
- FUN_1454ee240(world) is "get game viewport for this world" helper. If GEngine is null this returns null. GEngine is at module+0x9B4F760.
- FUN_14479BB60 is a getter that resolves the GameViewportSubsystem via World. Earlier feared crash but works once GEngine is valid.

----

Practice menu architecture (Bandai Kamui framework + UMG):
- kamui::ui::PracticeMenuImpl: native C++ class (NOT a UObject). Plain class allocated via FMemory::Malloc(0x1C0) with placement-new.
- RTTI mangle for the class: `?AV?$_Binder@U_Unforced@std@@P8PracticeMenuImpl@ui@kamui@@EAAX_N@ZPEAV345@AEBU?$_Ph@$00@2@@std@@` at module+0x17736d0 (string).
- vtable (derived): module+0x855da58.
- vtable (base): module+0x855d388.
- Singleton pointer: module+0x9B7BC90. Set by base constructor, cleared by destructor.
- Factory: module+0x5FD0480 (`malloc(0x1C0); ctor(p); return p;` — also writes the singleton).
- Derived ctor: module+0x5DA5A20.
- Base ctor: module+0x5DA5840.
- Destructors: module+0x5DA86F0 (derived), module+0x5DA7740 (base).
- "Open practice menu" function (vtable slot 1): module+0x5DB2A30. Takes (manager, world_ctx). Has ZERO call xrefs — invoked via vtable dispatch or delegate.

PracticeMenuImpl fields (relevant subset):
- +0x198: async loader for WBP_UI_PracticeMenu_C class path string.
- +0x1A0: async loader for WBP_UI_Practice_S2 class path string.
- +0x1A8: TWeakObjectPtr-like holder for the outer practice menu widget.
- +0x1B0: TWeakObjectPtr-like holder for the S2 sibling widget.
- +0x1B8: byte "initialized" flag.

The outer "practice mode controller" lives at module+0x9B79290 (allocated in FUN_145c8a5e0). It owns the PracticeMenuImpl reference at field +0x88. Main practice tick callback: FUN_145c902b0.

----

The practice menu open sequence the game runs (FUN_145db2a30, this function is PracticeMenuImpl::Open or similar):
1. FUN_145db2880 (setup task state).
2. Async-load WBP_UI_PracticeMenu_C class via path string.
3. Async-load WBP_UI_Practice_S2 class via path string.
4. Wait for both to load (FUN_141761970 yields a frame).
5. For each loaded class: FUN_144845a80(world, &out, class, 1) — find existing instance.
6. If none: FUN_144842900(world, class, 0) — CreateWidget.
7. Type check: FUN_145da1a90 for outer (IsA<UPolarisUMGPracticeMenu>), FUN_145d7c9e0 for S2.
8. FUN_14483fe70(widget, mode) — AddToViewport with mode 0x80 (outer) or 3 (S2).
9. widget[+0x280] = 0; widget[+0x282] = 1; (input flags).
10. FUN_142e90970(widget, 0); (visibility reset).
11. Store both widgets in PracticeMenuImpl holders (+0x1A8 for outer, +0x1B0 for S2).
12. Bind 10 delegates on the outer widget at offsets +0x290, +0x2D0, +0x310, +0x350, +0x390, +0x3D0, +0x410, +0x450, +0x490, +0x4D0. Each binding: `{ vtable_FUN_146ec6318, label/callback function ref, manager_this, ... }`. Callbacks point back to PracticeMenuImpl member functions.
13. Set PracticeMenuImpl+0x1B8 = 1 (initialized flag).

The 10 delegate bindings are C++ direct assignment, not UFunction system. They give the widget event handlers (button clicks, slot changes, etc.).

----

Native menu attempts and outcomes:

1. Naive CreateWidget(UPolarisUMGTextMenu, AddToViewport) → widget constructed but no visible content. The 3 BP subclasses of UPolarisUMGTextMenu are non-generic and don't render as standalone overlays either.

2. Construct WBP_UI_PracticeMenu_Menu_3_C → byte-level analysis shows this is a sub-widget designed to be a CanvasPanel child. Outer should be a WidgetTree, not GameInstance, and Slot should be a UCanvasPanelSlot. Trying to add to viewport leaves it orphaned and invisible.

3. Construct outer WBP_UI_PracticeMenu_C using FUN_144842900 + FUN_14483fe70(mode=0x80) + flag bytes + FUN_142e90970(0) → widget byte-matches live game instance on construction. Same WidgetTree pointer pattern, same child count, same flag bits. STILL doesn't render. Missing: the 10 external delegate bindings at +0x290..+0x4D0 that the game's manager populates.

4. Read the PracticeMenuImpl singleton at module+0x9B7BC90, factory-allocate if null, call vtable slot 1 with (manager, world) → no visible effect, no crash. The manager's surrounding state machine (the controller at module+0x9B79290) likely requires additional state to be live before the open path activates. The internal async-loader fields may also need a tick context that's only valid from within the practice main loop.

Conclusion: the practice menu opening is deeply integrated with Tekken's practice mode state machine. Replicating it from a DLL would require re-implementing a chunk of game logic, not just calling one function.

----

UCanvasPanel-related (would be useful if we'd ever want to add a widget as a CanvasPanel child):
- UCanvasPanel::StaticClass: module+0x476c080.
- UCanvasPanelSlot::StaticClass: module+0x476c710.
- UPanelWidget::StaticClass: module+0x4800580.
- UCanvasPanel::AddChildToCanvas exec stub: module+0x47c52d0. Calling convention is UE5 DECLARE_FUNCTION thunk (RCX=Context, RDX=FFrame, R8=Result), not the friendly C++ signature. Use ProcessEvent with `FindFunctionByName("AddChildToCanvas")` instead.
- UCanvasPanel::RebuildWidget: module+0x47b4710. Constructs the SConstraintCanvas slate root and iterates Slots[+0x150]/Slots count[+0x158] calling FUN_144789070 (BuildSlot) for each.
- UCanvasPanelSlot::BuildSlot: module+0x4789070.
- UCanvasPanel Slots TArray: panel+0x150 (data), panel+0x158 (count).

----

Pattern resolution helpers (working patterns):
- findUnrealClass (FindObject<UClass>): `45 33 C0 49 8B CF E8 ?? ?? ?? ?? 48 8B 4C 24 60`. Result at offset +7 (RIP-relative). Returns UClass* given a wide-char path. Use exact_class=true for native classes.
- findUnrealObjectsOfClass (GetObjectsOfClass): `E8 ?? ?? ?? ?? 90 48 89 6C 24 30`. Result at offset +1. Returns void; fills a TArray<UObject*> output param. Caller pre-allocates a 16-byte UE_TArray { data*, num, max }.

----

Bandai-specific UI architecture: the practice menu (and many other Polaris menus) use a "Kamui" wrapper layer. kamui::ui::*Impl classes are native C++ singletons that manage one or more UMG widgets. They:
- Allocate themselves via malloc + placement-new (not UObject system).
- Are stored as global singleton pointers in module's data section.
- Own their UMG widget instances through TWeakObjectPtr-like holders.
- Bind C++ callbacks directly on widget memory at hardcoded offsets — bypassing both UE's delegate reflection system and BP graphs.

This means Polaris's UI lifecycle does NOT follow standard UMG patterns. You can't just CreateWidget + AddToViewport and expect a working menu — the Kamui manager has to drive the widget.

Other Kamui implementations to investigate (mentioned by static analysis): KamuiManager at module+0x77F1398, kamui::ui::PauseMenuImpl (similar pattern, dedicated singleton).

----

Working approaches that DO produce visible UI inside Tekken:
- D3D12 vtable hook on Present + ImGui (the existing OpenDojo overlay).
- (Untried but architecturally cleaner per static analysis) APolarisHUD::PostRender hook — native UCanvas drawing (DrawText, DrawTexture) inside the game's draw pipeline. No D3D12 hook needed. APolarisHUD is the active HUD instance reachable via `PlayerController->MyHUD`. Strings present in binary include "DrawText", "DrawTexture", "Canvas Draw functions may only be called during the handling of the DrawHUD event".
- (Untried) UGameViewportClient::AddViewportWidgetContent — UPolarisGameViewportClient::StaticClass at module+0x4XXXXX (string at module+0x78c2aba). Takes a raw SWidget*, bypasses UMG. This is what loading screens use internally.

----

Approaches that DO NOT work for adding a native UMG menu (with reasons):
- CreateWidget + AddToViewport on any WBP_UI_*_C class — widget constructs but doesn't render. Either the widget is a sub-widget designed as a CanvasPanel child, or the outer top-level widget requires an external Kamui manager to bind delegates.
- Constructing UPolarisUMGTextMenu or its 3 subclasses (HudViewer, MessageChecker, StoryTournamentViewer) — none have visual content. The 17-method API is real but isn't used for any generic list-style menu in Tekken 8.
- Constructing UPolarisUMGDialog — pure BP, no native rendering, same blocker.
- Calling kamui::ui::PracticeMenuImpl::Open directly — function executes without crash but no visible result. The wider practice-mode controller state machine must be active and properly initialized for the open path to actually display the menu.
- Hardware data breakpoints on memory regions — crash the game even when offline. Use software bps or UE4SS hooks instead.

----

Recommended approach for future modders wanting a native UI:
- For drawn primitives (text, boxes, simple shapes): hook APolarisHUD::PostRender. Get UCanvas* from the call, use its DrawText/DrawTexture API. Render anything you want each frame.
- For interactive UMG: clone an existing visible widget's class (e.g., a HUD info bar) and modify its row text in place. Avoid trying to spawn a new managed widget.
- For ImGui-style overlay: already implemented in OpenDojo, works reliably.
- If you must construct a new UMG widget: target only classes that DON'T extend Polaris/Kamui (e.g., stock UTextBlock, UImage on a self-managed CanvasPanel). You'll need to assemble the WidgetTree manually rather than relying on a BP class.

----

Tekken 8 game state findings (for the OpenDojo drill mod specifically — see also the existing memory notes for full context):
- Practice mode gate: subsystems::KEY_GAMEPLAY at module+0x9537314, when non-zero indicates practice gameplay active. Per-frame mod logic must gate on this + a small exit grace.
- pool1/pool2 recording buffer pools: lazily allocated at module+0x986AC70 / +0x986AC78. Callable force-allocator at module+0x18E8E00 with dummy this-ptr.
- Singleton +0x002: side-gate byte. Must be 0x40 on import or P2-side drills play back mirrored.
- opponent_player[+0x39C0]: "session loaded" flag. Set to 1 after writing slots or in-game practice menu shows "no recordings".
- subB[+0x065]: playback-session-armed flag. Setting to 0 mid-intro freezes character input — was found to be a side-effect of set_recorded_flag(true), removed.
- Polaris-side player chain: P1/P2 character_id, main_player_info.player_id reachable via Irony's AOB patterns. See project_tekken_player_chain memory note.

----

Polaris build identifier strings observed in binary:
- "F:\p4\eagle\v300_xx-build_pkg_win\ue\Engine\Source\Runtime\UMG\Private\Blueprint\GameViewportSubsystem.cpp" — confirms UE5 source location + build path.
- Various PolarisUMG* names in class registration strings.

----

Diagnostic patterns that proved useful:
- `dump_object_memory(obj, len, label)` — log first N bytes of an object as qwords. Side-by-side diff of live vs constructed instances was the killer feature for identifying what's missing.
- `inspect_uobject_pointer(ptr, tag)` — read a heap pointer as a UObject (vtable, class, name) and log. Made it trivial to identify what fields point to.
- `find_widget_class_by_name(name)` — enumerate UUserWidget instances, decode each class FName, return matches. Lets you target any visible widget by string name once FName decoding works.
- `discover_subclasses_of(target_cls, label)` — log all classes deriving from target_cls, including their FName, super, and instance counts. Reveals BP subclasses we'd otherwise be unaware of.
- `enumerate_live_user_widgets()` — dump every live UUserWidget on screen. Combined with FName decoding, reveals the actual widget composition of any UI state.

These diagnostics live in dll/src/native_menu.cpp.

----

ImGui+D3D12 overlay caveats (durable):
- DO NOT hook ResizeBuffers or AddRef the command queue. Both combine to crash Tekken silently before the first Present.
- Init at present #60 (gives the game time to settle).
- Use single-slot RTV heap, per-frame allocators, dedicated SRV heap for fonts.
- WndProc hook is NOT needed — input via GetAsyncKeyState polling works fine.

----

CORRECTION: PracticeMenuImpl singleton (module+0x9B7BC90) is NOT the in-game practice pause-menu manager. It exists at runtime but its +0x1B8 init flag stays 0 — the in-game pause menu is managed by a different native class. Misidentification cost us hours.

The actual in-game practice-menu manager:
- vtable RVA: 0x7F7CC80 (absolute on observed run: 0x7FF70BFECC80).
- Allocated at runtime; one live instance per practice session; the OUTER widget (WBP_UI_PracticeMenu_C) is its UI surface.
- Resolve the live instance at runtime by enumerating live UUserWidget instances by class FName (WBP_UI_PracticeMenu_C) and reading any slot's receiver field at widget+slot+0x28.

Manager class field map (decoded from runtime observation + slot 4 disassembly):
- +0x00: vtable.
- +0x40: pointer to a sub-object (UI handler). The sub-object's vtable[6] (= sub_obj_vtable+0x30) is the actual row-confirm dispatcher invoked by slot 4 for both mouse and gamepad input.
- +0x4C / +0x54: currently HIGHLIGHTED row (i32). Both offsets see traffic during a session; +0x54 confirmed by static analysis of FUN_145f266e0.
- +0x68: mode (int32).
- +0x6C: sub-mode (int32; sometimes interpreted as float — value 0x40640000 observed).
- +0x81: menu-active byte.
- +0x82: sticky modal byte.
- +0x83: help-mode toggle byte (flipped by manager vtable[6]).
- +0x84: no-revert flag byte.
- +0xAC: misc flag byte.
- +0x124: QUEUED CLICK row (i32). Written by widget binder slot 0 on mouse click. -1 = idle. Consumed and reset by manager vtable[4] per-frame tick.
- +0x12C / +0x134 / +0x13C / +0x144: additional queued-input channels (each i32; likely one per input direction or one per controller channel).

----

In-game practice menu — WIDGET BINDER SLOT MAP (this is the live runtime mapping; the LABs DIFFER from the FUN_145db2a30 mapping documented earlier, because that earlier mapping was for the REPLAY menu's widget):

| Slot | widget offset | fn RVA | observed role |
|------|---------------|--------|---------------|
| 0 | +0x290 | 0x5F55C00 | mouse click (writes row to manager+0x124) |
| 1 | +0x2D0 | 0x5F55DC0 | mouse hover (row index in *rdx, fires 2x per change) |
| 2 | +0x310 | 0x5F55D70 | unmapped (silent in tests) |
| 3 | +0x350 | 0x5DCCF20 | pane focus change (fires 2x per nav) |
| 4 | +0x390 | 0x5DCCF50 | sibling of slot 3 |
| 5 | +0x3D0 | 0x5F55DD0 | unmapped |
| 6 | +0x410 | 0x5F55B60 | unmapped |
| 7 | +0x450 | 0x5F55AF0 | unmapped |
| 8 | +0x490 | 0x5F55BD0 | unmapped |
| 9 | +0x4D0 | 0x5F55DB0 | unmapped |

All 10 slots share vtable=0x6EC6318 (the std::_Binder-style vtable PTR_FUN_146ec6318) and receiver=manager.

Binder is bound by FUN_145f5c5e0 (manager vtable slot 1, RVA 0x5F5C5E0). That function is large — Ghidra times out decompiling it; the call pattern matches FUN_145db2a30's layout (allocate, async-load, AddToViewport, then 10 delegate binds at widget+0x290..+0x4D0).

The slot 0 click handler is short enough to handwrite (RVA 0x5F55C00, 13 bytes):
  mov  r8, [rcx]            ; r8 = manager
  mov  eax, [rdx]           ; eax = clicked row idx
  mov  [r8 + 0x124], eax    ; queue the click
  ret

It doesn't dispatch; the dispatch happens in the manager's per-frame tick (vtable slot 4).

----

Manager vtable[4] (FUN_145f266e0, RVA 0x5F266E0) is the per-frame tick and CONFIRM dispatcher:
- Reads `[rax + 0x194]` from a static global; switches on values {3, 5, 7, 8, 9}.
- For value 9: loads `[rbx + 0x40]` (sub-object), reads its vtable, calls `[sub_obj_vtable + 0x30]` (= slot 6) with row index.
- Also reads manager+0x124 (queued click) and resets it; reads manager+0x54 (highlighted row).

This is the unified confirm path for both mouse and gamepad. Mouse path: slot 0 binder writes manager+0x124, tick reads + dispatches. Gamepad path: gamepad input pipeline (path not yet pinned) drives slot 4's switch state and the dispatch happens directly via the sub-object call.

----

Mouse-only menu-item hijack (working pattern, validated in CE):

1. Resolve outer practice-menu widget (e.g., via GetObjectsOfClass for WBP_UI_PracticeMenu_C).
2. Allocate hook + 24-byte state struct anywhere in process memory.
3. Replace fn pointer at widget+0x290 with hook entry.
4. Hook body (x64 fastcall, RCX = binder.receiver slot addr, RDX = ptr to clicked row int32):
     push rax
     test rdx, rdx
     jz   fwd
     mov  eax, [rdx]
     mov  [state+last_row], eax
     cmp  eax, [state+magic_row]
     jne  fwd
     inc  qword [state+hijack_count]
     pop  rax
     ret                       ; SUPPRESS — original is NOT called
   fwd:
     inc  qword [state+forward_count]
     pop  rax
     jmp  qword [state+orig_fn] ; tail-jump to module+0x5F55C00

5. To restore: write `module + 0x5F55C00` back to widget+0x290.

Effects: mouse click on the magic row is silently suppressed (no submenu opens). All other rows behave normally. Gamepad is UNAFFECTED — it bypasses this binder.

----

Failed hijack attempts in this session (recorded so they aren't tried again):

- Vtable-pointer-swap on the manager (copy vtable, replace one entry, write new vtable* to manager+0x00): CRASHED. The manager class has at least 110 virtual methods (slot 4 calls vtable+0x368 = slot 109 internally). A 32-slot copy truncates and out-of-bounds dispatches reach junk. Don't copy the vtable unless you size it correctly.
- In-place patch of manager vtable[16] (offset 0x80): zero hits. That slot is NOT the row-confirm dispatcher despite what FUN_145dac1e0 (a DIFFERENT class's binder) suggests.
- In-place patch of manager vtable[109] (offset 0x368): zero hits. The sub-agent's analysis said slot 4 calls vtable+0x368, but the actual call inside slot 4 is `call [(*manager+0x40)+0x30]` — a sub-object's vtable, NOT the manager's own.

Correct gamepad/confirm hook target (to verify next session):
- Read manager+0x40 → sub_object_ptr (heap pointer).
- Read sub_object_ptr+0x00 → sub_object_vtable.
- Read sub_object_vtable+0x30 → the actual row-confirm fn pointer (this is the unified mouse+gamepad path).
- Hook it in place.

Caveat: the sub-object's vtable lives in module memory and is likely shared by other instances of the same sub-class. If the practice menu is the only live owner, in-place patch is fine; otherwise it bleeds into other places.

----

Diagnostic patterns that worked this session (CE-based, no DLL rebuild needed):

- Find live widget without restart:
    `find_first_live_user_widget_by_class_name("WBP_UI_PracticeMenu_C")` in our DLL (F11 cycle in-game), then copy the address into CE.
- Hot-patch via CE auto_assemble:
    Use `[ENABLE]` / `[DISABLE]` blocks with explicit `dq <orig>` in DISABLE so the script is fully reversible. Allocate state + hook with `alloc(name, size, near_addr)` — `near_addr` keeps RIP-relative offsets short.
- Slot probe:
    Hook every slot with a small templated stub that writes (last_*rdx, call_count) to per-slot state cells. Press buttons in-game, read state cells after — see which slots fire for which input.

CE quirks:
- `aob_scan` / `scan_all` from the MCP returned 0 matches even for known addresses (CE's default region filter is restrictive). Use `evaluate_lua` with `AOBScan(...)` instead — that respects all regions.
- `_SH_DENYNO` (`_wfsopen` with that share mode) is required if your DLL writes a log file you want to tail live with another tool — default `_wfopen_s` locks it.

----

The "modify menu directly" path (toward the real goal — NEXT SESSION):

Phase A — find the row widgets:
- Walk widget+0x228 → UWidgetTree at 0x191FE6F0C40 (last observed). UWidgetTree has standard UObject header; Outer = the practice menu widget.
- Find the items-container widget among the 12 immediate children of the practice menu widget. Probably a UVerticalBox or UListView.
- That container's Slots TArray (panel+0x150 for UPanelWidget-derived) holds 14 row widgets.

Phase B — find row labels:
- Each row widget likely has a UTextBlock child carrying its FText (the visible label).
- Localization keys exist as data (`TEXT_000_UI_PRACTICE_*`), but at runtime each row stores a resolved FText.
- Modify row 9's UTextBlock text to "OpenDojo" (string replace on the row's FText storage).

Phase C — find row-index → callback map:
- Slot 4 (manager tick) dispatches via `[(*mgr+0x40)+0x30]`. That sub-object likely owns a per-row callback table or a switch statement.
- Hook the sub-object call and check row arg; if == 9, run OpenDojo handler instead of original.

Phase D — works for both mouse AND gamepad because we're hijacking the SHARED dispatch, not the input event.

----

Tooling notes (durable):

- x64dbg: enable, but disable "TLS callbacks" in Events to stop the game pausing on every module load. Software BPs are safe in Tekken (HW data BPs crash — see existing memory note).
- CE MCP bridge: install once; alive across CE restarts. Re-attach to game via `open_process` (param name is `process_id_or_name`, not `pid`).
- When game crashes and is relaunched: PID changes; CE stays attached to old PID and silently fails. Re-attach to new PID before doing more work.

----

Practice-mode lifecycle (used by opendojo::practice_state to replace per-tick polling):

- Practice-controller singleton slot: module+0x9B79290. Pointer-sized.
- Allocation path (single per session entry): FUN_145CA3870 (RVA 0x05CA3870). Allocates 0xD0 bytes, calls FUN_145C8A5E0 ctor which writes the singleton slot. All gated callers (e.g. FUN_145C5FC40 game-state machine, FUN_145CB8CD3 et al.) check FUN_145C93300() == 0 first, so the factory fires exactly once per practice entry.
- Destruction path (single per session exit): FUN_145C8C2F0 (RVA 0x05C8C2F0). Not auto-analyzed by Ghidra, but identifiable from layout: starts immediately after FUN_145C8C290's RET, contains a write `MOV [DAT_149B79290], 0` at 0x145C8C349, references the derived vtable at 0x1485508A0 (the one installed by ctor at 0x145C8A602). The dtor is the only writer of the singleton slot besides the ctor.
- Adjacent dtors share the same prologue (`MOV [RSP+8],RBX; MOV [RSP+10],RSI; PUSH RDI; SUB RSP,0x20` = 14 bytes), so MinHook can patch the prologue safely.
- Signature: `void* __fastcall ~PracticeController(void* this, unsigned flags)` — standard MSVC scalar-deleting dtor. `flags & 1` triggers operator delete with size 0xD0.
- Hook at entry to read live gameplay state BEFORE the dtor tears down sub-objects (sub-fields at +0xA0 and +0x98 are released early in the dtor body).
- The same dtor symbol could in principle be reused for non-singleton instances, so the hook filters: only fire when `this == *(void**)(module+0x9B79290)`.

----

## 2026-08-20 patch (the Bob release)

Build identity: the exe has no version resource (`FileVersionRaw` reads
`1.0.0.0`) and no `x.yy.zz` string, so use the Steam buildid `24827309`
(`steamapps/appmanifest_1778820.acf`).

Only one thing broke: `players.cpp::PAT_MAIN_INFO`. The tell is
`players: pattern miss (players=<nonzero> info=0x0)` — the two patterns are
scanned separately, so a nonzero `players` with `info=0x0` names the casualty.

Cause: the pattern (from Irony) led with the prologue `40 53 48 83 EC 20`, and
this patch stopped emitting that body as its own function. Both surviving copies
now sit mid-function (RVA 0x5C2FF94 after an epilogue, 0x5C5DDC4 after a call)
with the body bytes unchanged. Fix is to anchor on the body; disp32 +9 -> +3:

```
48 8B 1D ?? ?? ?? ?? 48 85 DB 74 ?? BA 01 00 00 00 48 8B CB E8 ?? ?? ?? ?? 48 85 C0 74 ?? B2 01
```

Both body sites decode to the same global, so the tail only pins uniqueness.
General rule: anchor on a function's body, not its prologue — prologues die to
inlining and object-file reordering, bodies only die when the code changes.

Values: `PAT_MAIN_INFO` global `0x9B8DB70`. Holder global (`PAT_PLAYERS`,
pattern unchanged) `0x9B91BC0` — independently matches tekken-fashion-hub's
`PLAYERMGR_GLOBAL_RVA_FALLBACK`. `CTX_PTR_OFFSET` fallback `0x9537300` ->
`0x954D300` (the AOB resolved CTX either way).

Scanning caveat: re-derive from the exe ON DISK, not from live memory — MinHook
overwrites the prologue of anything already detoured, so a live scan misses
exactly the functions we hook.
