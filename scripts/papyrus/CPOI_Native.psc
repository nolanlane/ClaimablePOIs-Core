ScriptName CPOI_Native Extends ScriptObject Native hidden

; --- Native functions registered by ClaimablePOIs.dll ---
; These are bound via IVirtualMachine::BindNativeMethod at kPostDataLoad.

bool Function NotifyPlayerLoaded(Actor akPlayer) Global Native
    ; Call first on the actual player's OnPlayerLoadGame event. Drops obsolete
    ; native overlay metadata without dereferencing the previous world's pointers.
    ; False means retained enrollment references require a full process restart.
    ; Loaded-save helpers must be recovered separately after this succeeds.

bool Function InitializeInheritedProtection(ObjectReference akBeacon) Global Native
    ; Fresh-claim finalizer only; cannot certify a legacy already-enrolled claim.
bool Function HasInheritedProtection(ObjectReference akBeacon) Global Native
bool Function IsInheritedTeardownReady() Global Native
int Function GetPendingInheritedTeardown() Global Native
int Function GetInheritedTeardownBeacon(int aiToken) Global Native
Function CancelInheritedTeardown(int aiToken, bool abFault = false) Global Native
int Function PrepareInheritedTeardown(ObjectReference akBeacon, int aiToken) Global Native
    ; 0 = no mutation/blocked. -1 = partial/fault; stop. -2 = detached but queue
    ; rejected. Positive = detached count + 1, verified vanilla removal queued.
int Function GetInheritedTeardownCompletion(int aiToken) Global Native
    ; 1 = stock handler returned, beacon retirement ran, player-outpost registry
    ; no longer contains it, and inherited states survived. -1 = failed; 0 = pending.

bool Function IsLocationExplored(Location akLocation) Global Native
    ; Returns true if the location has been explored by the player.

bool Function IsLocationClaimable(Location akLocation) Global Native
    ; Returns true if the location has the CPOI_LocTypeClaimablePOI keyword.

bool Function IsLocationNeverReset(Location akLocation) Global Native
    ; Returns the location's native kNoReset flag without mutating it.

bool Function IsSiteClaimed(Location akLocation) Global Native
    ; Returns true if the location is in the CPOI_ClaimedPOIs form list.

int Function GetClaimedCount() Global Native
    ; Returns the current value of the CPOI_ClaimedCount global.

float Function GetClaimRadius() Global Native
    ; Returns the current value of the CPOI_ClaimRadius global.

; Deprecated compatibility stub: always false; use Claim Beacon.
bool Function ClaimSite(Location akLocation) Global Native
    ; Always false. No Location flags, ownership or claim bookkeeping is edited.

; Deprecated compatibility stub: always false; use normal protected removal.
bool Function UnclaimSite(Location akLocation) Global Native
    ; Always false. Normal outpost removal uses protected vanilla teardown.

int Function BeginPOIOverlayUnlock(ObjectReference akOverlayActivator) Global Native
    ; InitialBitOnly-v1 isolated trial: sets only bit 0 on the resolved
    ; ActivatedOverlayProperty and the verified engine dirty byte for the initial
    ; FC07OutpostActivator claim; it intentionally does not dispatch SetOutpostOverlay.
    ; Verifies post-mutation bit 0 and captures tracking identity. 1 = unlocked,
    ; 0 = safely rejected, -1 = recovery required; retain the helper on -1.

int Function BeginClaimedPOIWorkshopUnlock(ObjectReference akOverlayActivator) Global Native
    ; InitialBitOnly-v1 isolated trial: same guarded property-bit-only activation
    ; route as the initial claim; it intentionally does not dispatch SetOutpostOverlay.
    ; The native path verifies post-mutation bit 0 and captures tracking identity
    ; for recovery. 1 = unlocked, 0 = safely rejected, -1 = recovery required;
    ; retain the helper on -1.

bool Function ResumePOIOverlayUnlock(ObjectReference akOverlayActivator) Global Native
    ; Resolves the cell ActivatedOverlayProperty for a serialized FC07OutpostActivator helper
    ; and recaptures tracking identity after game load without mutating state.

bool Function ResumePOIOverlayRecovery(ObjectReference akOverlayActivator) Global Native
    ; Restoration-only recapture after load: accepts a set OR already-clear bit.
    ; Preserves a matching live tracker and rejects a conflicting transaction.
    ; Success does not authorize scanner placement; call RestorePOIOverlay next.

bool Function RestorePOIOverlay(ObjectReference akOverlayActivator) Global Native
    ; Clears bit 0 on the captured ActivatedOverlayProperty, sets the engine dirty byte,
    ; verifies the flag is cleared, and resets tracking. Already-clear is treated as success.

bool Function IsInitialClaimBound(ObjectReference akOverlayActivator, ObjectReference akBeacon) Global Native
    ; Read-only proof from the completed vanilla registration for this live helper/beacon.
    ; Requires reuse of the original POI overlay. Query before RestorePOIOverlay clears
    ; the live transaction. Never reconstructs a result from cached IDs after load.

bool Function RecoverClaimedPOIWorkshopUnlock(ObjectReference akOverlayActivator) Global Native
    ; Resolves the cell ActivatedOverlayProperty for a serialized FC07OutpostActivator helper,
    ; ensures bit 0 is clear whether loaded clear or set, verifies safe state, and clears tracking.

bool Function LogReferenceState(ObjectReference akReference, int aiStage) Global Native
    ; Writes a read-only reference/extra-data snapshot to ClaimablePOIs.log.

int Function LogMiniSiteCensus(ObjectReference akPlayer, Cell[] akLoadedCells, Location akLocation, int aiCheckpoint) Global Native
    ; Operator-only 64u census anchored at the player on checkpoint 0.
    ; Checkpoints 0..3 must be sequential in one accepted load epoch/world.
    ; Returns -1 for rejection/incomplete coverage. No engine pointers saved.

int Function LogReferencePopulation(ObjectReference akCenter, Cell akClaimCell, Cell[] akLoadedCells, Location akLocation, float afRadius, int aiStage) Global Native
    ; Read-only bounded scan of in-radius references in attached exterior cells.
    ; Stage 0 captures a volatile identity baseline; later stages report exact
    ; missing/added/changed deltas. No engine pointers or data enter the save.
    ; Stage values are internal diagnostic checkpoints owned by the claim quest.

int Function GetDecorationSafetyFlags(ObjectReference akReference) Global Native
    ; Read-only, fail-closed decoration gate. Zero means the sole current
    ; allowlisted Starfield STAT base (0x00237543) has a non-null extra-data
    ; list, PromotedRef, and none of the rejected extra-data types.
    ; Nonzero bits identify rejection reasons:
    ; 0x00000001 null reference, 0x00000002 missing base,
    ; 0x00000004 non-STAT base, 0x00000008 base not allowlisted,
    ; 0x00000010 missing extra-data list, 0x00000020 missing PromotedRef,
    ; 0x00000040 Keywords, 0x00000080 Ownership,
    ; 0x00000100 EnableStateParent, 0x00000200 EnableStateChildren,
    ; 0x00000400 LinkedRef, 0x00000800 LinkedRefChildren,
    ; 0x00001000 Location, 0x00002000 Radius,
    ; 0x00004000 ObjectInstance, 0x00008000 Workshop,
    ; 0x00010000 WorkshopSnapping, 0x00020000 FurnitureEntryData,
    ; 0x00040000 SnapLinks, 0x00080000 GroupedPackin,
    ; 0x00100000 CalcedPackIn, 0x00200000 SourcePackIn,
    ; 0x00400000 OwningInstance, 0x00800000 ProceduralInstanceData,
    ; 0x01000000 PlacedPlanetContent, 0x02000000 native probe exception,
    ; 0x04000000 unsupported Starfield runtime.

int Function GetWorkshopReplacementSafetyFlags(ObjectReference akReference) Global Native
    ; Read-only, fail-closed preflight for an inherited object whose base has a
    ; runtime outpost build-menu recipe. Furniture additionally requires
    ; PromotedRef and FurnitureEntryData. All types require an extra-data list
    ; and no rejected state/ownership/workshop/pack-in metadata.
    ; Nonzero bits identify rejection reasons:
    ; 0x00000001 null reference, 0x00000002 missing base,
    ; 0x00000004 not enrollment-eligible, 0x00000008 not outpost-buildable,
    ; 0x00000010 missing extra-data list, 0x00000020 missing PromotedRef,
    ; 0x00000040 missing FurnitureEntryData, 0x00000080 Keywords,
    ; 0x00000100 Ownership, 0x00000200 EnableStateParent,
    ; 0x00000400 EnableStateChildren, 0x00000800 LinkedRef,
    ; 0x00001000 LinkedRefChildren, 0x00002000 Location,
    ; 0x00004000 Radius, 0x00008000 ObjectInstance,
    ; 0x00010000 Workshop, 0x00020000 WorkshopSnapping,
    ; 0x00040000 SnapLinks, 0x00080000 GroupedPackin,
    ; 0x00100000 CalcedPackIn, 0x00200000 SourcePackIn,
    ; 0x00400000 OwningInstance, 0x00800000 ProceduralInstanceData,
    ; 0x01000000 PlacedPlanetContent, 0x02000000 native probe exception,
    ; 0x04000000 unsupported Starfield runtime.

bool Function IsRuntimeOutpostBuildable(Form akBase) Global Native
    ; True when akBase is produced by a loaded, player-facing OutpostCategory*
    ; COBJ recipe discovered through the runtime all-forms registry.

ObjectReference Function BeginClaimTimeWorkshopEnrollment(Cell akCell, Cell[] akLoadedCells, Location akClaimLocation, ObjectReference akHelper, ObjectReference akBeacon, float afRadius, FormList akExcludedOriginals, FormList akExcludedReplacements) Global Native
    ; Scans deduplicated attached exterior loaded cells within the claim radius.
    ; Native attaches CONT candidates via verified F3CF40/predicate. Inherited
    ; FURN is never promoted or attached by this path. Other buildables remain
    ; replacement-adoption candidates.
    ; Returns the first attached candidate, or None when the status reports a
    ; verified zero-eligible no-op or transaction rejection. The caller must call
    ; CommitClaimTimeWorkshopEnrollment on finalization success or
    ; retain pending state on rejection. Unverified engine mutation latches -1
    ; and retains references; restart from a pre-claim save after preserving logs.

int Function GetClaimTimeWorkshopEnrollmentStatus() Global Native
    ; 0 = no-op/no active transaction, 1 = one or more candidates attached, -1 = reject.

bool Function CommitClaimTimeWorkshopEnrollment() Global Native
    ; Commits native transaction bookkeeping after claim finalization succeeds.

bool Function RollbackClaimTimeWorkshopEnrollment() Global Native
    ; Detaches a verified active transaction. A rejected transaction is retained
    ; without further engine mutation; false never discards its ledger.

bool Function CommitSelectedInheritedWorkshopItem(ObjectReference akItem, ObjectReference akBeacon, Location akClaimLocation, float afRadius, FormList akExcludedOriginals, FormList akExcludedReplacements) Global Native
    ; Experimental exact-base operator trial. Calls the whole vanilla workshop
    ; commit routine once only when the active engine context owns akBeacon.

bool Function ReattachWorkshopItem(ObjectReference akItem, ObjectReference akBeacon, Location akClaimLocation, float afRadius, FormList akExcludedOriginals, FormList akExcludedReplacements) Global Native
    ; Experimental active-session chair-only path. Revalidates exact claimed
    ; location/radius, lifecycle/safety, and both replacement ledgers, then
    ; calls F3CF40/F3CC90, invokes the exact-runtime engine ensure path only
    ; when Workshop::ExtraData is missing, and re-queues F3CF40 once more.
    ; No raw Workshop::ExtraData construction or ItemPlacedEvent dispatch.

int Function ReattachAllWorkshopItems(ObjectReference akBeacon, Cell akClaimCell, Cell[] akLoadedCells, Location akClaimLocation, float afRadius, FormList akExcludedOriginals, FormList akExcludedReplacements, bool abCommitFurniture) Global Native
    ; Type-separated active-session transactions over validated loaded exterior
    ; cells. True enrolls eligible standalone FURN, promoting through the verified
    ; engine path when needed. False enrolls unaliased, unowned compound CONT,
    ; promoting through that same engine path when needed. Source Location and
    ; incoming linked children are permitted; outgoing links and procedural/group/
    ; workshop carriers stay excluded. Opaque existing workshop links are skipped.
    ; Both modes verify attach, roll back attempted new links on failure and latch
    ; the process closed. Neither mode edits inventory, locks or owner fields.

int Function DetachSessionWorkshopFurniture(ObjectReference akBeacon) Global Native
    ; Removes only the inherited FURN links created and retained by the current
    ; claimed-workshop session. ExtraPromotedRef remains intact. Returns the
    ; detached count, zero for no tracked FURN, or -1 on a verified failure.

String Function GetLocationName(Location akLocation) Global Native
    ; Returns the real localized display name of the given location (from TESFullName),
    ; falling back to its parent location if empty. Returns "" if unresolved.

bool Function SanitizeClaimedBeacon(ObjectReference akBeacon) Global Native
    ; Strips ExtraSourcePackIn and ExtraLocation from the genuine vanilla beacon
    ; to prevent procedural suppression and build-mode spinners upon cell reload.
    ; Safe and idempotent; returns true on success or if already clean.

; Optional read-only diagnostics; defaults off, read from INI once per process.
bool Function IsDiagnosticsEnabled() global native
