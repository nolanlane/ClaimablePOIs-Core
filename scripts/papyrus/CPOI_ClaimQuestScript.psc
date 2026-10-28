ScriptName CPOI_ClaimQuestScript Extends Quest

; --- Properties ---
GlobalVariable Property CPOI_ClaimedCount Auto Const mandatory
GlobalVariable Property CPOI_ClaimRadius Auto Const mandatory
FormList Property CPOI_ClaimedPOIs Auto Const mandatory
FormList Property CPOI_ClaimedBeacons Auto Const mandatory
FormList Property CPOI_OverlayClaims Auto Const mandatory
FormList Property CPOI_ReplacedOriginals Auto Const mandatory
FormList Property CPOI_WorkshopReplacements Auto Const mandatory
Keyword Property CPOI_LocTypeClaimablePOI Auto Const mandatory
Keyword Property CPOI_ClaimLandingAnchorLink Auto Const
FormList Property CPOI_InheritedItems Auto Const
FormList Property CPOI_ProtectedBeacons Auto Const
Message Property CPOI_Msg_ClaimUnavailable Auto Const mandatory
Message Property CPOI_Msg_AssistedInstructions Auto Const mandatory
Message Property CPOI_Msg_PlacementInstructions Auto Const mandatory
Message Property CPOI_Msg_ClearClutter Auto Const mandatory
Form Property CPOI_ClaimTransponder Auto Const mandatory
Faction Property PlayerFaction Auto Const mandatory

; --- State ---
bool g_claimInProgress = false
int g_claimRequest = 0
bool g_cleanupAfterClaim = false
; Captured only from the live registration before restoration clears native tracking.
bool g_initialBindingVerified = false
bool g_claimPending = false
Actor g_pendingPlayer = None
bool g_outpostPlacementPending = false
Actor g_outpostPlacementPlayer = None
Cell g_outpostPlacementCell = None
Location g_outpostPlacementLocation = None
; Temporary FC07 anchor used for two-phase overlay unlock and restoration.
ObjectReference g_outpostOverlayHelper = None
float g_outpostPlacementRadius = 0.0
ObjectReference g_pendingBeacon = None
; New claims retain an explicit POI/beacon association, including same-location
; beacons. The legacy split-field names preserve serialized save compatibility;
; older same-location claims retain a FormList/location fallback.
ObjectReference[] g_splitClaimBeacons = None
Location[] g_splitClaimLocations = None
; Save-local snapshots for the official Location.SetNeverResets API. Never
; touch a shared master/template location or infer ownership from proximity.
Struct ClaimResetProtection
    Location site
    bool originallyNeverResets
EndStruct
ClaimResetProtection[] g_claimResetProtection = None
int g_resetProtectionWorkerRequest = -1
Guard ClaimResetGuard ProtectsFunctionLogic
; CPOI owns these newly created references. The original pad/ship references
; never enter this cleanup ledger and are never enabled, moved or deleted here.
Struct ClaimLandingAnchor
    ObjectReference beacon
    ObjectReference marker
    bool removalConfirmed = false
EndStruct
ClaimLandingAnchor[] g_claimLandingAnchors = None
ObjectReference g_pendingOriginalLandingMarker = None
int g_landingAnchorBusyRequest = -1
; Retired FurnitureAfterReturn-v1 save fields. Kept for serialized compatibility;
; first-entry enrollment no longer reads or writes this ledger.
Struct FurnitureReturnGate
    ObjectReference beacon
    WorldSpace claimWorld
    int phase ; 0 = waiting for departure, 1 = departed, 2 = returned
EndStruct
FurnitureReturnGate[] g_furnitureReturnGates = None
; Sticky until completion: a vanished reference is not permission to cancel
; a vanilla outpost that has already been accepted.
bool g_outpostBeaconAccepted = false
bool g_placementRecoveryBusy = false
bool g_nativeLoadReady = true
int g_teardownToken = 0
ObjectReference g_teardownBeacon = None
Location g_teardownClaimLocation = None
Location g_teardownBeaconLocation = None
int g_teardownPolls = 0
int g_teardownWorkerRequest = -1
bool g_overlayRestored = false
bool g_overlayTrackingReady = false
bool g_cancelPending = false
int g_restoreRetryCount = 0
bool g_workshopSessionActive = false
ObjectReference g_workshopSessionBeacon = None
ObjectReference g_workshopSessionHelper = None
int g_workshopRestoreRetryCount = 0
bool g_workshopStartDeferred = false
ObjectReference g_workshopDeferredBeacon = None
int g_cassiopeiaVersion = 0
; Legacy serialized timer state; automatic ownership sweeping is disabled.
ObjectReference g_deferredOwnershipBeacon = None
Cell g_deferredOwnershipCell = None
float g_deferredOwnershipRadius = 0.0
; Serialized one-reference decoration gate state. This is deliberately not
; auto-restored on load; the operator must call RestoreDecorationGate().
ObjectReference g_decorationGateReference = None
string g_decorationGateReferenceID = ""
bool g_decorationGateActive = false
ObjectReference g_workshopReplacementOriginal = None
string g_workshopReplacementOriginalID = ""
Form g_workshopReplacementBase = None
ObjectReference g_workshopReplacementBeacon = None
Location g_workshopReplacementLocation = None
Cell g_workshopReplacementCell = None
float g_workshopReplacementX = 0.0
float g_workshopReplacementY = 0.0
float g_workshopReplacementZ = 0.0
float g_workshopReplacementAngleX = 0.0
float g_workshopReplacementAngleY = 0.0
float g_workshopReplacementAngleZ = 0.0
ObjectReference g_workshopReplacementCandidate = None
string g_workshopReplacementCandidateID = ""
bool g_workshopReplacementPending = false
bool g_workshopReplacementHidden = false
bool g_workshopReplacementListening = false
int g_workshopReplacementRetryCount = 0

; --- Events ---

Event OnInit()
    Debug.Trace("[CPOI] ClaimQuest initialized", 0)
    Self.RegisterForMenuOpenCloseEvent("InventoryMenu")
    InitializeCleanupSupport()
    RegisterCassiopeiaWorkshopTelemetry()
    RegisterPlayerEvents()
    StartTimer(1.0, 13)
EndEvent

Event Actor.OnPlayerLoadGame(Actor akSender)
    if akSender != Game.GetPlayer()
        return
    endif
    ; This event is delivered even when the loaded save predates the transaction.
    ; Invalidate old native metadata BEFORE any loaded-save recovery or diagnostics.
    g_nativeLoadReady = false
    ; Invalidate serialized pre-placement stacks and queued inventory requests.
    ; Accepted placement/recovery state is retained separately below.
    g_claimRequest += 1
    g_claimPending = false
    g_pendingPlayer = None
    g_claimInProgress = g_outpostPlacementPending
    g_cleanupAfterClaim = false
    CancelTimer(2)
    CancelTimer(3)
    CancelTimer(13)
    g_teardownWorkerRequest = -1
    g_resetProtectionWorkerRequest = -1
    g_nativeLoadReady = CPOI_Native.NotifyPlayerLoaded(akSender)
    CancelDeferredOwnershipSweep()
    CancelTimer(11)
    CancelTimer(12)
    if !g_nativeLoadReady
        Debug.Trace("[CPOI] Player load blocked: native recovery/protection unavailable; inspect SFSE and CPOI logs", 0)
        Debug.Notification("CPOI native recovery is unavailable. Check SFSE/CPOI logs; restart with your control save.")
        return
    endif
    Debug.Trace("[CPOI] ClaimQuest OnPlayerLoadGame", 0)
    RefreshClaimResetProtection()
    if !CPOI_Native.IsInheritedTeardownReady()
        Debug.Trace("[CPOI] InheritedTeardown-v4 protection unavailable; new mutations blocked, exact overlay recovery remains available", 0)
        Debug.Notification("CPOI teardown protection is unavailable. Do not remove outposts; preserve the CPOI log.")
    endif
    ; A native completion receipt cannot survive a load. Preserve an interrupted
    ; journal and fault new mutation, while still running exact overlay recovery.
    if g_teardownBeacon != None || g_teardownToken != 0
        CPOI_Native.CancelInheritedTeardown(0, true)
        Debug.Trace("[CPOI] InheritedTeardown-v4 interrupted journal retained; no removal replay or delete-bit inference", 0)
        Debug.Notification("This save has an interrupted outpost removal. Preserve logs and use your pre-removal save.")
    else
        ClearTeardownState()
    endif
    StartTimer(1.0, 13)
    g_landingAnchorBusyRequest = -1
    g_placementRecoveryBusy = false
    ; Loading always exits the live workshop UI; never replay an entry that was
    ; merely queued during initial beacon creation in the saved session.
    g_workshopStartDeferred = false
    g_workshopDeferredBeacon = None
    Self.RegisterForMenuOpenCloseEvent("InventoryMenu")
    InitializeCleanupSupport()
    RegisterCassiopeiaWorkshopTelemetry()
    RegisterPlayerEvents()

    ; VanillaOutpostIdentity-v1: vanilla's beacon owns LocTypeOutpost.
    ; Observe its loaded state without stripping the genuine outpost identity.
    Keyword locTypeOutpost = Game.GetFormFromFile(0x000234F1, "Starfield.esm") as Keyword
    if locTypeOutpost != None && CPOI_ClaimedPOIs != None
        int locIdx = 0
        int locCount = CPOI_ClaimedPOIs.GetSize()
        while locIdx < locCount
            Location claimedLoc = CPOI_ClaimedPOIs.GetAt(locIdx) as Location
            if claimedLoc != None && claimedLoc.HasKeyword(locTypeOutpost)
                ObjectReference claimedBeacon = FindTrackedBeaconFor(claimedLoc)
                if claimedBeacon != None && claimedBeacon.GetCurrentLocation() != claimedLoc
                    Debug.Trace("[CPOI] VanillaLocationSeparation-v1 FAILED: separate claimed POI location has LocTypeOutpost on load", 0)
                    Debug.Notification("POI location became an outpost. Stop trial and preserve logs.")
                endif
                Debug.Trace("[CPOI] VanillaOutpostIdentity-v1 player-load: preserved vanilla LocTypeOutpost on " + claimedLoc, 0)
            endif
            locIdx += 1
        endwhile
    endif

    if g_workshopReplacementPending
        RollbackPendingWorkshopReplacement("player load")
    endif
    if g_workshopSessionHelper != None || g_workshopSessionActive
        AttemptRecoverWorkshopSession()
    endif
    if g_outpostPlacementPending && g_outpostOverlayHelper != None
        ; Migrate pending state from saves made before the acceptance latch.
        if g_pendingBeacon != None
            g_outpostBeaconAccepted = true
        endif
        ; Loading enters recovery, never reopens scanner placement.
        g_cancelPending = !g_outpostBeaconAccepted
        if !g_overlayRestored
            ; NotifyPlayerLoaded discarded the previous world's native pointers.
            g_overlayTrackingReady = false
            bool resumed = CPOI_Native.ResumePOIOverlayRecovery(g_outpostOverlayHelper)
            Debug.Trace("[CPOI] OnPlayerLoadGame: ResumePOIOverlayRecovery returned " + resumed, 0)
            if !resumed
                Debug.Trace("[CPOI] OnPlayerLoadGame: warning, ResumePOIOverlayRecovery failed for helper " + g_outpostOverlayHelper, 0)
                g_restoreRetryCount += 1
                if g_restoreRetryCount <= 5
                    Debug.Trace("[CPOI] OnPlayerLoadGame: scheduling retry " + g_restoreRetryCount + "/5", 0)
                    StartTimer(2.0, 6)
                else
                    Debug.Trace("[CPOI] OnPlayerLoadGame: ResumePOIOverlayRecovery failed after max retries", 0)
                endif
                StartTimer(3.0, 4)
                return
            endif
            g_overlayTrackingReady = true
            g_restoreRetryCount = 0
        endif

        if g_outpostBeaconAccepted
            Debug.Trace("[CPOI] OnPlayerLoadGame: resuming completion with pending beacon " + g_pendingBeacon, 0)
            ProcessSuccessfulOutpostPlacement()
        else
            Debug.Trace("[CPOI] OnPlayerLoadGame: resuming pending cancellation", 0)
            AttemptCancelPendingPlacement()
        endif
    endif
    StartTimer(3.0, 4)
EndEvent


Event Actor.OnLocationChange(Actor akSender, Location akOldLoc, Location akNewLoc)
    if !g_nativeLoadReady
        return
    endif
    Actor player = Game.GetPlayer()
    if akSender != player
        return
    endif

    RetireRemovedClaimLandingAnchors()
    RefreshClaimResetProtection()


    if g_workshopReplacementPending && g_workshopReplacementLocation != None && akNewLoc != g_workshopReplacementLocation
        RollbackPendingWorkshopReplacement("location exit")
    endif

    if g_workshopSessionActive
        Debug.Trace("[CPOI] Player left location during active workshop session (" + akOldLoc + " -> " + akNewLoc + "); restoring overlay", 0)
        RestoreWorkshopOverlaySession()
    endif

    if g_outpostPlacementPending && !g_cancelPending && akSender == g_outpostPlacementPlayer
        if g_outpostBeaconAccepted && akNewLoc == g_outpostPlacementLocation
            g_restoreRetryCount = 0
            ProcessSuccessfulOutpostPlacement()
        elseif g_outpostPlacementLocation != None && akNewLoc != g_outpostPlacementLocation
            bool nearAnchor = !g_outpostBeaconAccepted && g_outpostOverlayHelper != None && !g_outpostOverlayHelper.IsDeleted() && player.GetWorldSpace() == g_outpostOverlayHelper.GetWorldSpace() && player.GetDistance(g_outpostOverlayHelper) <= g_outpostPlacementRadius
            if nearAnchor && akNewLoc != None
                Debug.Trace("[CPOI] VanillaLocationSeparation-v1: player crossed POI location within claim radius; vanilla beacon may be placed here. new=" + akNewLoc, 0)
                Debug.Notification("Outside POI location. Place the real beacon within claim range.")
            elseif nearAnchor
                Debug.Trace("[CPOI] VanillaLocationSeparation-v1: player crossed a location gap within radius; pending placement retained", 0)
            elseif !g_outpostBeaconAccepted
                Debug.Trace("[CPOI] Player left pending claim worldspace/radius; cancelling placement. new=" + akNewLoc, 0)
                AttemptCancelPendingPlacement()
            endif
        endif
    endif

    if akNewLoc == None || CPOI_ClaimedPOIs == None
        return
    endif

    bool isClaimedLoc = CPOI_ClaimedPOIs.HasForm(akNewLoc)
    ObjectReference beaconRef = FindTrackedBeaconFor(akNewLoc)
    Location resolvedPoiLoc = akNewLoc

    if beaconRef != None
        Location trackedPoi = FindClaimedLocationForBeacon(beaconRef)
        if trackedPoi != None
            resolvedPoiLoc = trackedPoi
        endif
        isClaimedLoc = true
    endif

    if !isClaimedLoc
        return
    endif

    ; VanillaOutpostIdentity-v1: preserve vanilla's location identity on return.
    Keyword locTypeOutpost = Game.GetFormFromFile(0x000234F1, "Starfield.esm") as Keyword
    if locTypeOutpost != None && akNewLoc.HasKeyword(locTypeOutpost)
        if beaconRef != None && beaconRef.GetCurrentLocation() != akNewLoc && akNewLoc == resolvedPoiLoc
            Debug.Trace("[CPOI] VanillaLocationSeparation-v1 FAILED: vanilla marked the separate POI location as an outpost on return", 0)
            Debug.Notification("POI location became an outpost. Stop trial and preserve logs.")
        endif
        Debug.Trace("[CPOI] VanillaOutpostIdentity-v1 return: preserved vanilla LocTypeOutpost on " + akNewLoc, 0)
    endif

    Debug.Trace("[CPOI] Diagnostic claimed-location entry old=" + akOldLoc + " new=" + akNewLoc + " resolvedPoi=" + resolvedPoiLoc, 0)
    CPOI_Native.LogReferenceState(player, 5)
    if beaconRef != None
        ; BeaconMetadataPreservation-v1: retain vanilla source/location extras
        ; across return diagnostics; this trial does not sanitize the beacon.
        CPOI_Native.LogReferenceState(beaconRef, 6)
        LogPopulationSnapshot(beaconRef, resolvedPoiLoc, 6)
        LogShipLandingContext("return", beaconRef)
    else
        Debug.Trace("[CPOI] Diagnostic claimed-location entry has no tracked native beacon", 0)
    endif
EndEvent

Function LogShipLandingContext(String asStage, ObjectReference akBeacon = None)
    if !CPOI_Native.IsDiagnosticsEnabled()
        return
    endif
    ; Read-only observation: neither a generic linked ref nor the player's
    ; forced marker is assumed to be the site's original landing-pad marker.
    Actor player = Game.GetPlayer()
    SpaceshipReference ship = Game.GetPlayerHomeSpaceShip()
    ObjectReference forcedMarker = None
    if player != None
        forcedMarker = player.GetForcedLandingMarker()
    endif
    ObjectReference defaultLink = None
    if ship != None && !ship.IsDeleted()
        defaultLink = ship.GetLinkedRef()
        Debug.Trace("[CPOI] LandingContext-v1 stage=" + asStage + " beacon=" + GetDiagnosticFormID(akBeacon) + " homeShip=" + GetDiagnosticFormID(ship) + " shipWorld=" + GetDiagnosticFormID(ship.GetWorldSpace()) + " shipLocation=" + GetDiagnosticFormID(ship.GetCurrentLocation()) + " shipPos=(" + ship.GetPositionX() + "," + ship.GetPositionY() + "," + ship.GetPositionZ() + ") defaultLink=" + GetDiagnosticFormID(defaultLink) + " forcedMarker=" + GetDiagnosticFormID(forcedMarker) + " mutation=false", 0)
    else
        Debug.Trace("[CPOI] LandingContext-v1 stage=" + asStage + " homeShip=unavailable forcedMarker=" + GetDiagnosticFormID(forcedMarker) + " mutation=false", 0)
    endif
    if defaultLink != None && !defaultLink.IsDeleted()
        Debug.Trace("[CPOI] LandingContext-v1 stage=" + asStage + " defaultLink=" + GetDiagnosticFormID(defaultLink) + " linkBase=" + GetDiagnosticFormID(defaultLink.GetBaseObject()) + " linkWorld=" + GetDiagnosticFormID(defaultLink.GetWorldSpace()) + " linkLocation=" + GetDiagnosticFormID(defaultLink.GetCurrentLocation()) + " linkPos=(" + defaultLink.GetPositionX() + "," + defaultLink.GetPositionY() + "," + defaultLink.GetPositionZ() + ")", 0)
    endif
    Keyword landingLink = Game.GetFormFromFile(0x0001940B, "Starfield.esm") as Keyword
    if ship != None && landingLink != None
        ObjectReference actualMarker = ship.GetLinkedRef(landingLink)
        if actualMarker != None && !actualMarker.IsDeleted()
            Debug.Trace("[CPOI] LandingAnchor-v2 context stage=" + asStage + " actualMarker=" + GetDiagnosticFormID(actualMarker) + " base=" + GetDiagnosticFormID(actualMarker.GetBaseObject()) + " disabled=" + actualMarker.IsDisabled() + " markerPos=(" + actualMarker.GetPositionX() + "," + actualMarker.GetPositionY() + "," + actualMarker.GetPositionZ() + ") mutation=false", 0)
        endif
    endif
EndFunction

Keyword Function ClaimLandingAnchorLink()
    ; Existing pre-claim saves may not have a newly appended VMAD property.
    if CPOI_ClaimLandingAnchorLink != None
        return CPOI_ClaimLandingAnchorLink
    endif
    return Game.GetFormFromFile(0x0000080F, "ClaimablePOIs.esm") as Keyword
EndFunction

bool Function IsSupportedLandingMarkerBase(Form akBase)
    ; Reviewed horizontal vanilla family. Use the same policy for capture and
    ; owned-copy retirement; vertical/hangar/mod-added variants stay excluded.
    return akBase != None && (akBase == Game.GetFormFromFile(0x001D6E5E, "Starfield.esm") || akBase == Game.GetFormFromFile(0x0017DE66, "Starfield.esm") || akBase == Game.GetFormFromFile(0x00007145, "Starfield.esm"))
EndFunction

ObjectReference Function CaptureOriginalLandingMarker(Actor akPlayer, float afRadius)
    SpaceshipReference ship = Game.GetPlayerHomeSpaceShip()
    Keyword landingLink = Game.GetFormFromFile(0x0001940B, "Starfield.esm") as Keyword
    if ship == None || ship.IsDeleted() || landingLink == None || akPlayer == None
        return None
    endif
    ObjectReference marker = ship.GetLinkedRef(landingLink)
    if marker == None || marker.IsDeleted() || marker.IsDisabled()
        return None
    endif
    Form reviewedBase = marker.GetBaseObject()
    if !IsSupportedLandingMarkerBase(reviewedBase)
        return None
    endif
    if marker.GetWorldSpace() == None || marker.GetWorldSpace() != akPlayer.GetWorldSpace() || ship.GetWorldSpace() != akPlayer.GetWorldSpace() || !(marker.GetDistance(akPlayer) >= 0.0 && marker.GetDistance(akPlayer) <= afRadius) || !(marker.GetDistance(ship) >= 0.0 && marker.GetDistance(ship) <= 120.0) || !ship.CanLandAtMarker(marker)
        return None
    endif
    Debug.Trace("[CPOI] LandingAnchor-v2 captured original=" + GetDiagnosticFormID(marker) + " ship=" + GetDiagnosticFormID(ship) + " base=" + GetDiagnosticFormID(reviewedBase), 0)
    return marker
EndFunction

bool Function LandingAnchorClaimIsCurrent(int aiRequest, ObjectReference akBeacon, ObjectReference akOriginal)
    if aiRequest != g_claimRequest || !g_nativeLoadReady || !g_outpostPlacementPending || !g_initialBindingVerified || !g_overlayRestored || akBeacon == None || akBeacon != g_pendingBeacon || akOriginal == None || akOriginal != g_pendingOriginalLandingMarker
        return false
    endif
    Actor player = Game.GetPlayer()
    if player == None || player != g_outpostPlacementPlayer || player.GetParentCell() != g_outpostPlacementCell || player.GetCurrentLocation() != g_outpostPlacementLocation || akBeacon.IsDeleted() || !CPOI_ClaimedBeacons.HasForm(akBeacon)
        return false
    endif
    return CaptureOriginalLandingMarker(player, g_outpostPlacementRadius) == akOriginal && akBeacon.GetWorldSpace() == akOriginal.GetWorldSpace() && akBeacon.GetDistance(akOriginal) <= g_outpostPlacementRadius
EndFunction

Function CreateClaimLandingAnchor(int aiRequest, ObjectReference akBeacon, ObjectReference akOriginal)
    if g_landingAnchorBusyRequest >= 0 || akOriginal == None
        Debug.Trace("[CPOI] LandingAnchor-v2 skipped: busy or no reviewed player landing marker; landing preservation unaccepted", 0)
        return
    endif
    Keyword privateLink = ClaimLandingAnchorLink()
    if privateLink == None || !LandingAnchorClaimIsCurrent(aiRequest, akBeacon, akOriginal)
        Debug.Trace("[CPOI] LandingAnchor-v2 rejected: binding/property/context changed", 0)
        return
    endif
    if g_claimLandingAnchors == None
        g_claimLandingAnchors = new ClaimLandingAnchor[0]
    endif
    int i = 0
    while i < g_claimLandingAnchors.Length
        if g_claimLandingAnchors[i].beacon == akBeacon
            ; Idempotent recovery: never recreate/re-enable an existing record.
            Debug.Trace("[CPOI] LandingAnchor-v2 existing ledger entry beacon=" + GetDiagnosticFormID(akBeacon), 0)
            return
        endif
        i += 1
    endwhile
    if akBeacon.GetLinkedRef(privateLink) != None || g_claimLandingAnchors.Length >= 64
        Debug.Trace("[CPOI] LandingAnchor-v2 rejected: existing private link or ledger capacity", 0)
        return
    endif

    g_landingAnchorBusyRequest = aiRequest
    ; Reserve the serialized owner record BEFORE the creation call can yield.
    ; Keep the local struct identity: an old stack must never append to a newly
    ; loaded save's ledger after returning from PlaceAtMe.
    ClaimLandingAnchor entry = new ClaimLandingAnchor
    entry.beacon = akBeacon
    g_claimLandingAnchors.Add(entry)
    ; Stock creation owns a fresh, initially disabled, persistent reference. No
    ; source pack-in, alias, inventory, ship link or original marker is edited.
    ObjectReference anchor = akOriginal.PlaceAtMe(akOriginal.GetBaseObject(), 1, true, true, false, None, None, false)
    entry.marker = anchor
    if anchor == None
        ReleaseLandingAnchorLock(aiRequest)
        Debug.Trace("[CPOI] LandingAnchor-v2 failed: marker creation returned None", 0)
        Debug.Notification("Original landing pad preservation failed. Preserve logs.")
        return
    endif
    if !LandingAnchorClaimIsCurrent(aiRequest, akBeacon, akOriginal)
        ReleaseLandingAnchorLock(aiRequest)
        Debug.Trace("[CPOI] LandingAnchor-v2 interrupted after creation; disabled owned marker retained for review", 0)
        return
    endif
    anchor.SetPosition(akOriginal.GetPositionX(), akOriginal.GetPositionY(), akOriginal.GetPositionZ())
    anchor.SetAngle(akOriginal.GetAngleX(), akOriginal.GetAngleY(), akOriginal.GetAngleZ())
    if !LandingAnchorClaimIsCurrent(aiRequest, akBeacon, akOriginal) || anchor.GetWorldSpace() != akOriginal.GetWorldSpace() || !(anchor.GetDistance(akOriginal) >= 0.0 && anchor.GetDistance(akOriginal) <= 0.01)
        ReleaseLandingAnchorLock(aiRequest)
        Debug.Trace("[CPOI] LandingAnchor-v2 interrupted before linking; disabled owned marker retained for review", 0)
        return
    endif
    ; Private bidirectional links protect the anchor from inherited FURN editing
    ; and corroborate ownership during eventual beacon-removal cleanup.
    anchor.SetLinkedRef(akBeacon, privateLink)
    akBeacon.SetLinkedRef(anchor, privateLink)
    if !LandingAnchorClaimIsCurrent(aiRequest, akBeacon, akOriginal) || anchor.GetLinkedRef(privateLink) != akBeacon || akBeacon.GetLinkedRef(privateLink) != anchor
        ReleaseLandingAnchorLock(aiRequest)
        Debug.Trace("[CPOI] LandingAnchor-v2 failed: private ownership links did not verify; disabled marker retained", 0)
        Debug.Notification("Original landing pad preservation needs recovery. Preserve logs.")
        return
    endif
    anchor.Enable(false)
    ReleaseLandingAnchorLock(aiRequest)
    if !LandingAnchorClaimIsCurrent(aiRequest, akBeacon, akOriginal)
        Debug.Trace("[CPOI] LandingAnchor-v2 context changed during enable; owned marker retained for review", 0)
        return
    endif
    if anchor.IsDisabled()
        Debug.Trace("[CPOI] LandingAnchor-v2 failed: owned marker remained disabled", 0)
        Debug.Notification("Original landing pad preservation failed. Preserve logs.")
        return
    endif
    Debug.Trace("[CPOI] LandingAnchor-v2 ready beacon=" + GetDiagnosticFormID(akBeacon) + " original=" + GetDiagnosticFormID(akOriginal) + " anchor=" + GetDiagnosticFormID(anchor) + " pos=(" + anchor.GetPositionX() + "," + anchor.GetPositionY() + "," + anchor.GetPositionZ() + ") originalMutations=0 normalEngineSelection=true", 0)
    CPOI_Native.LogReferenceState(anchor, 3)
EndFunction

Function ReleaseLandingAnchorLock(int aiRequest)
    if g_landingAnchorBusyRequest == aiRequest
        g_landingAnchorBusyRequest = -1
    endif
EndFunction

Function RetireRemovedClaimLandingAnchors()
    if g_landingAnchorBusyRequest >= 0 || g_claimLandingAnchors == None
        return
    endif
    Keyword privateLink = ClaimLandingAnchorLink()
    SpaceshipReference ship = Game.GetPlayerHomeSpaceShip()
    Keyword shipLandingLink = Game.GetFormFromFile(0x0001940B, "Starfield.esm") as Keyword
    if privateLink == None || ship == None || shipLandingLink == None
        return
    endif
    int cleanupRequest = g_claimRequest
    g_landingAnchorBusyRequest = cleanupRequest
    int i = g_claimLandingAnchors.Length - 1
    while i >= 0
        if cleanupRequest != g_claimRequest || !g_nativeLoadReady
            ReleaseLandingAnchorLock(cleanupRequest)
            return
        endif
        ObjectReference beacon = g_claimLandingAnchors[i].beacon
        ObjectReference anchor = g_claimLandingAnchors[i].marker
        if beacon == None || beacon.IsDeleted() || g_claimLandingAnchors[i].removalConfirmed
            if anchor == None || anchor.IsDeleted()
                g_claimLandingAnchors.Remove(i)
            elseif IsSupportedLandingMarkerBase(anchor.GetBaseObject()) && anchor.GetLinkedRef(privateLink) == beacon && !anchor.IsQuestItem() && ship.GetLinkedRef(shipLandingLink) != anchor
                ; Only the recorded CPOI-created copy is retired. An in-use pad
                ; remains until vanilla clears its ship alias on departure.
                anchor.SetLinkedRef(None, privateLink)
                anchor.Disable(false)
                if cleanupRequest != g_claimRequest || !g_nativeLoadReady
                    ReleaseLandingAnchorLock(cleanupRequest)
                    return
                endif
                anchor.Delete()
                if cleanupRequest != g_claimRequest || !g_nativeLoadReady
                    ReleaseLandingAnchorLock(cleanupRequest)
                    return
                endif
                g_claimLandingAnchors.Remove(i)
                Debug.Trace("[CPOI] LandingAnchor-v2 retired owned marker after beacon removal", 0)
            endif
        endif
        i -= 1
    endwhile
    ReleaseLandingAnchorLock(cleanupRequest)
EndFunction

Event OnMenuOpenCloseEvent(String asMenuName, Bool abOpening)
    if asMenuName == "InventoryMenu" && !abOpening && g_claimPending
        ; Let the inventory stack finish closing before showing a modal message.
        Utility.Wait(0.1)
        ProcessPendingClaim()
    endif
EndEvent

Function InitializeCleanupSupport()
    g_cassiopeiaVersion = CassiopeiaPapyrusExtender.GetCassiopeiaPapyrusExtenderVersion()
    if g_cassiopeiaVersion >= 96
        Debug.Trace("[CPOI] Cassiopeia cleanup support available, version=" + g_cassiopeiaVersion, 0)
    else
        Debug.Trace("[CPOI] Cassiopeia 9.6 unavailable; corpse cleanup and safe nearby-hostile checks are disabled", 0)
    endif
EndFunction

Function QueueClaimBeaconActivation(Actor akPlayer)
    if akPlayer == None
        Debug.Trace("[CPOI] QueueClaimBeaconActivation: player is None", 0)
        return
    endif

    if g_claimInProgress || g_claimPending || g_outpostPlacementPending
        Debug.Notification("Claim operation already in progress...")
        return
    endif

    ; Inventory use fires while InventoryMenu is open, but favorites/quickslot
    ; use can fire during normal gameplay. Only defer when the menu is actually
    ; open; otherwise the close event will never arrive.
    if g_cassiopeiaVersion >= 96 && !CassiopeiaPapyrusExtender.IsMenuOpen("InventoryMenu")
        Debug.Trace("[CPOI] InventoryMenu is not open; starting claim immediately", 0)
        HandleClaimBeaconActivation(akPlayer)
        StartBeaconRefundTimer()
        return
    endif

    g_claimPending = true
    g_pendingPlayer = akPlayer
    StartTimer(60.0, 3)
    Debug.Trace("[CPOI] Claim queued; waiting for InventoryMenu close", 0)
EndFunction

Function ProcessPendingClaim()
    if !g_claimPending
        return
    endif

    Actor player = g_pendingPlayer
    g_pendingPlayer = None
    g_claimPending = false
    CancelTimer(3)

    if player == None
        Debug.Trace("[CPOI] Pending claim lost its player reference", 0)
        StartBeaconRefundTimer()
        return
    endif

    Debug.Trace("[CPOI] InventoryMenu closed; starting deferred claim", 0)
    HandleClaimBeaconActivation(player)
    StartBeaconRefundTimer()
EndFunction

Function RegisterClaimedBeaconEvents()
    if CPOI_ClaimedBeacons == None
        return
    endif
    int i = 0
    int count = CPOI_ClaimedBeacons.GetSize()
    while i < count
        ObjectReference beaconRef = CPOI_ClaimedBeacons.GetAt(i) as ObjectReference
        if beaconRef != None && !beaconRef.IsDeleted()
            Self.RegisterForRemoteEvent(beaconRef, "OnWorkshopMode")
        endif
        i += 1
    endwhile
EndFunction

Function RegisterPlayerEvents()
    Actor player = Game.GetPlayer()
    if player != None
        Self.RegisterForRemoteEvent(player, "OnPlayerLoadGame")
        Self.RegisterForRemoteEvent(player, "OnOutpostPlaced")
        Self.RegisterForRemoteEvent(player, "OnLocationChange")

        ; Ensure the player has the claim beacon item
        if CPOI_ClaimTransponder != None && player.GetItemCount(CPOI_ClaimTransponder) == 0
            player.AddItem(CPOI_ClaimTransponder, 1, false)
            Debug.Trace("[CPOI] Added Claim Beacon to player inventory", 0)
        endif
    endif
    RegisterClaimedBeaconEvents()
EndFunction

; --- Save-local claim and beacon associations ---

ObjectReference Function FindTrackedBeaconFor(Location akLocation)
    if CPOI_ClaimedBeacons == None || akLocation == None
        return None
    endif

    if g_splitClaimBeacons != None && g_splitClaimLocations != None && g_splitClaimBeacons.Length == g_splitClaimLocations.Length
        int splitIndex = 0
        while splitIndex < g_splitClaimBeacons.Length
            ObjectReference splitBeacon = g_splitClaimBeacons[splitIndex]
            if g_splitClaimLocations[splitIndex] == akLocation && splitBeacon != None && !splitBeacon.IsDeleted() && CPOI_ClaimedBeacons.HasForm(splitBeacon)
                return splitBeacon
            endif
            splitIndex += 1
        endwhile
    endif

    int i = 0
    int count = CPOI_ClaimedBeacons.GetSize()
    while i < count
        Form stored = CPOI_ClaimedBeacons.GetAt(i)
        ObjectReference beaconRef = stored as ObjectReference
        if beaconRef != None && !beaconRef.IsDeleted() && beaconRef.GetCurrentLocation() == akLocation
            return beaconRef
        endif
        i += 1
    endwhile
    return None
EndFunction

Location Function FindClaimedLocationForBeacon(ObjectReference akBeacon)
    if akBeacon == None || CPOI_ClaimedBeacons == None || !CPOI_ClaimedBeacons.HasForm(akBeacon)
        return None
    endif
    if g_splitClaimBeacons != None && g_splitClaimLocations != None && g_splitClaimBeacons.Length == g_splitClaimLocations.Length
        int i = 0
        while i < g_splitClaimBeacons.Length
            if g_splitClaimBeacons[i] == akBeacon && g_splitClaimLocations[i] != None && CPOI_ClaimedPOIs != None && CPOI_ClaimedPOIs.HasForm(g_splitClaimLocations[i])
                return g_splitClaimLocations[i]
            endif
            i += 1
        endwhile
    endif
    Location beaconLocation = akBeacon.GetCurrentLocation()
    if CPOI_ClaimedPOIs != None && CPOI_ClaimedPOIs.HasForm(beaconLocation)
        return beaconLocation
    endif
    return None
EndFunction

bool Function RecordSplitClaimPair(ObjectReference akBeacon, Location akLocation)
    if akBeacon == None || akLocation == None || CPOI_ClaimedPOIs == None || CPOI_ClaimedBeacons == None
        return false
    endif
    if g_splitClaimBeacons == None
        g_splitClaimBeacons = new ObjectReference[0]
    endif
    if g_splitClaimLocations == None
        g_splitClaimLocations = new Location[0]
    endif
    if g_splitClaimBeacons.Length != g_splitClaimLocations.Length
        return false
    endif
    int i = 0
    while i < g_splitClaimBeacons.Length
        if g_splitClaimBeacons[i] == akBeacon
            return g_splitClaimLocations[i] == akLocation
        endif
        if g_splitClaimLocations[i] == akLocation
            return false
        endif
        i += 1
    endwhile
    g_splitClaimBeacons.Add(akBeacon)
    g_splitClaimLocations.Add(akLocation)
    if g_splitClaimBeacons.Length == i + 1 && g_splitClaimLocations.Length == i + 1 && g_splitClaimBeacons[i] == akBeacon && g_splitClaimLocations[i] == akLocation
        return true
    endif
    if g_splitClaimBeacons.Length > i
        g_splitClaimBeacons.Remove(i)
    endif
    if g_splitClaimLocations.Length > i
        g_splitClaimLocations.Remove(i)
    endif
    return false
EndFunction

; Runs once per load/location change, and on successful claim finalization.
; Existing alpha.5 pairs are sufficient; no new ESM forms or claim recreation.
Function RefreshClaimResetProtection()
    if !g_nativeLoadReady || CPOI_ClaimedBeacons == None || g_resetProtectionWorkerRequest >= 0
        return
    endif
    int request = g_claimRequest
    g_resetProtectionWorkerRequest = request
    int count = CPOI_ClaimedBeacons.GetSize()
    int protectedCount = 0
    int unresolved = 0
    ; This bounds our own claim ledger, not the engine's shared registry.
    if count <= 256
        int i = 0
        while i < count && request == g_claimRequest && g_nativeLoadReady
            ObjectReference beacon = CPOI_ClaimedBeacons.GetAt(i) as ObjectReference
            Location site = FindClaimedLocationForBeacon(beacon)
            if beacon != None && beacon == g_teardownBeacon
                ; Confirmed removal restores its snapshot under the same guard.
            elseif EnsureClaimResetProtection(beacon, site, request)
                protectedCount += 1
            else
                unresolved += 1
            endif
            i += 1
        endwhile
    else
        unresolved = count
    endif
    if g_resetProtectionWorkerRequest == request
        g_resetProtectionWorkerRequest = -1
    endif
    if request == g_claimRequest && g_nativeLoadReady
        Debug.Trace("[CPOI] ClaimResetProtection-v1 refresh protected=" + protectedCount + " unresolved=" + unresolved, 0)
        if unresolved > 0
            Debug.Notification("Some claimed sites could not receive reset protection. Check the CPOI Papyrus log.")
        endif
    endif
EndFunction

bool Function EnsureClaimResetProtection(ObjectReference akBeacon, Location akSite, int aiRequest)
    LockGuard ClaimResetGuard
        if aiRequest != g_claimRequest || !g_nativeLoadReady || akBeacon == None || akSite == None || akBeacon.IsDeleted() || akBeacon == g_teardownBeacon
            return false
        endif
        ; FF-created locations belong to this save. Never flag a shared POI base,
        ; parent planet/system location, or an ordinary untracked player outpost.
        if !CassiopeiaPapyrusExtender.IsCreated(akSite) || FindClaimedLocationForBeacon(akBeacon) != akSite
            Debug.Trace("[CPOI] ClaimResetProtection-v1 refused untracked/shared location=" + akSite, 0)
            return false
        endif
        if g_claimResetProtection == None
            g_claimResetProtection = new ClaimResetProtection[0]
        endif
        int index = g_claimResetProtection.FindStruct("site", akSite)
        if index < 0
            if g_claimResetProtection.Length >= 256
                return false
            endif
            ClaimResetProtection snapshot = new ClaimResetProtection
            snapshot.site = akSite
            snapshot.originallyNeverResets = CPOI_Native.IsLocationNeverReset(akSite)
            if aiRequest != g_claimRequest || !g_nativeLoadReady
                return false
            endif
            ; Serialize the original value before the first write; retries and loads
            ; must not replace it with the value that CPOI itself applied.
            g_claimResetProtection.Add(snapshot)
            index = g_claimResetProtection.FindStruct("site", akSite)
            if index < 0
                return false
            endif
        endif
        if aiRequest != g_claimRequest || !g_nativeLoadReady || akBeacon == g_teardownBeacon || FindClaimedLocationForBeacon(akBeacon) != akSite
            return false
        endif
        akSite.SetNeverResets(true)
        bool verified = CPOI_Native.IsLocationNeverReset(akSite)
        Debug.Trace("[CPOI] ClaimResetProtection-v1 apply site=" + akSite + " beacon=" + akBeacon + " original=" + g_claimResetProtection[index].originallyNeverResets + " verified=" + verified, 0)
        return aiRequest == g_claimRequest && g_nativeLoadReady && verified
    EndLockGuard
EndFunction

bool Function RestoreClaimResetProtection(Location akSite)
    LockGuard ClaimResetGuard
        if akSite == None || akSite != g_teardownClaimLocation || g_teardownToken == 0 || CPOI_Native.GetInheritedTeardownCompletion(g_teardownToken) != 1
            return false
        endif
        if g_claimResetProtection == None
            return true ; A claim removed before protection was applied owns no flag.
        endif
        int index = g_claimResetProtection.FindStruct("site", akSite)
        if index < 0
            return true
        endif
        if !g_nativeLoadReady || akSite == None || !CassiopeiaPapyrusExtender.IsCreated(akSite)
            return false
        endif
        int request = g_claimRequest
        bool original = g_claimResetProtection[index].originallyNeverResets
        akSite.SetNeverResets(original)
        if request != g_claimRequest || !g_nativeLoadReady || CPOI_Native.IsLocationNeverReset(akSite) != original
            return false ; Retain the snapshot rather than guessing restoration.
        endif
        Debug.Trace("[CPOI] ClaimResetProtection-v1 restored site=" + akSite + " original=" + original, 0)
        g_claimResetProtection.Remove(index)
        return true
    EndLockGuard
EndFunction

; Cassiopeia clears native-event registrations when a save is loaded. Register
; only the low-frequency v9.7 lifecycle/grab/release events, and filter them to
; the genuine beacons already owned by this quest.
Function RegisterCassiopeiaWorkshopTelemetry()
    if !CPOI_Native.IsDiagnosticsEnabled()
        return
    endif
    if g_cassiopeiaVersion < 97 || CPOI_ClaimedBeacons == None
        Debug.Trace("[CPOI] Cassiopeia 9.7 workshop telemetry unavailable", 0)
        return
    endif

    int count = CPOI_ClaimedBeacons.GetSize()
    if count <= 0
        Debug.Trace("[CPOI] Cassiopeia 9.7 workshop telemetry deferred: no claimed beacons", 0)
        return
    endif

    Form[] beaconFilters = new Form[count]
    int i = 0
    while i < count
        beaconFilters[i] = CPOI_ClaimedBeacons.GetAt(i)
        i += 1
    endwhile

    string callbackScript = "CPOI_ClaimQuestScript"
    CassiopeiaPapyrusExtender.UnregisterForNativeEvent(callbackScript, "Workshop_BuildModeEnterEvent")
    CassiopeiaPapyrusExtender.UnregisterForNativeEvent(callbackScript, "Workshop_BuildModeExitEvent")
    CassiopeiaPapyrusExtender.UnregisterForNativeEvent(callbackScript, "Workshop_ItemGrabbedEvent")
    CassiopeiaPapyrusExtender.UnregisterForNativeEvent(callbackScript, "Workshop_ItemReleasedEvent")

    bool enterRegistered = CassiopeiaPapyrusExtender.RegisterForNativeEvent(callbackScript, "Workshop_BuildModeEnterEvent", beaconFilters)
    bool exitRegistered = CassiopeiaPapyrusExtender.RegisterForNativeEvent(callbackScript, "Workshop_BuildModeExitEvent", beaconFilters)
    bool grabbedRegistered = CassiopeiaPapyrusExtender.RegisterForNativeEvent(callbackScript, "Workshop_ItemGrabbedEvent", beaconFilters)
    bool releasedRegistered = CassiopeiaPapyrusExtender.RegisterForNativeEvent(callbackScript, "Workshop_ItemReleasedEvent", beaconFilters)
    Debug.Trace("[CPOI] Cassiopeia 9.7 workshop telemetry registered beacons=" + count + " enter=" + enterRegistered + " exit=" + exitRegistered + " grabbed=" + grabbedRegistered + " released=" + releasedRegistered, 0)
EndFunction

Function Workshop_BuildModeEnterEvent(Int aiPlacementItemHandle, ObjectReference workshopRef) Global
    ObjectReference placementRef = CassiopeiaPapyrusExtender.GetReferenceByHandle(aiPlacementItemHandle)
    Form placementBase = None
    if placementRef != None
        placementBase = placementRef.GetBaseObject()
    endif
    Debug.Trace("[CPOI] Cassiopeia Workshop_BuildModeEnterEvent workshop=" + CassiopeiaPapyrusExtender.GetHexFormID(workshopRef) + " placementHandle=" + aiPlacementItemHandle + " placementRef=" + CassiopeiaPapyrusExtender.GetHexFormID(placementRef) + " placementBase=" + CassiopeiaPapyrusExtender.GetHexFormID(placementBase), 0)
EndFunction

Function Workshop_BuildModeExitEvent(ObjectReference workshopRef) Global
    Debug.Trace("[CPOI] Cassiopeia Workshop_BuildModeExitEvent workshop=" + CassiopeiaPapyrusExtender.GetHexFormID(workshopRef), 0)
EndFunction

Function Workshop_ItemGrabbedEvent(Int aiGrabbedItemHandle, Int aiPlacementItemHandle, ObjectReference workshopRef) Global
    ObjectReference grabbedRef = CassiopeiaPapyrusExtender.GetReferenceByHandle(aiGrabbedItemHandle)
    ObjectReference placementRef = CassiopeiaPapyrusExtender.GetReferenceByHandle(aiPlacementItemHandle)
    Form grabbedBase = None
    if grabbedRef != None
        grabbedBase = grabbedRef.GetBaseObject()
    endif
    Debug.Trace("[CPOI] Cassiopeia Workshop_ItemGrabbedEvent workshop=" + CassiopeiaPapyrusExtender.GetHexFormID(workshopRef) + " grabbedHandle=" + aiGrabbedItemHandle + " grabbedRef=" + CassiopeiaPapyrusExtender.GetHexFormID(grabbedRef) + " grabbedBase=" + CassiopeiaPapyrusExtender.GetHexFormID(grabbedBase) + " placementHandle=" + aiPlacementItemHandle + " placementRef=" + CassiopeiaPapyrusExtender.GetHexFormID(placementRef), 0)
EndFunction

Function Workshop_ItemReleasedEvent(Int aiReleasedItemHandle, ObjectReference workshopRef) Global
    ObjectReference releasedRef = CassiopeiaPapyrusExtender.GetReferenceByHandle(aiReleasedItemHandle)
    Form releasedBase = None
    if releasedRef != None
        releasedBase = releasedRef.GetBaseObject()
    endif
    Debug.Trace("[CPOI] Cassiopeia Workshop_ItemReleasedEvent workshop=" + CassiopeiaPapyrusExtender.GetHexFormID(workshopRef) + " releasedHandle=" + aiReleasedItemHandle + " releasedRef=" + CassiopeiaPapyrusExtender.GetHexFormID(releasedRef) + " releasedBase=" + CassiopeiaPapyrusExtender.GetHexFormID(releasedBase), 0)
EndFunction

bool Function IsWorkshopEnrollmentExcluded(ObjectReference akReference)
    if akReference == None
        return false
    endif
    if CPOI_ReplacedOriginals != None && CPOI_ReplacedOriginals.HasForm(akReference)
        return true
    endif
    if CPOI_WorkshopReplacements != None && CPOI_WorkshopReplacements.HasForm(akReference)
        return true
    endif
    return false
EndFunction

; --- Beacon refund (called by CPOI_ClaimBeaconEffect script) ---

Function StartBeaconRefundTimer()
    StartTimer(1.0, 1)
EndFunction

Event OnTimer(Int aiTimerID)
    if !g_nativeLoadReady
        return
    endif
    if aiTimerID == 13
        PollProtectedTeardown()
        StartTimer(1.0, 13)
    elseif aiTimerID == 1
        Actor player = Game.GetPlayer()
        if player != None && CPOI_ClaimTransponder != None && player.GetItemCount(CPOI_ClaimTransponder) == 0
            player.AddItem(CPOI_ClaimTransponder, 1, true)
            Debug.Trace("[CPOI] Refunded Claim Beacon to player", 0)
        endif
    elseif aiTimerID == 2
        ; Retired preflight timeout: a modal may legitimately last >60 seconds.
        ; A timer must never release a lock owned by a suspended request.
        CancelTimer(2)
    elseif aiTimerID == 3
        if g_claimPending
            g_claimPending = false
            g_pendingPlayer = None
            Debug.Trace("[CPOI] Pending claim timed out before InventoryMenu closed", 0)
            StartBeaconRefundTimer()
        endif
    elseif aiTimerID == 4
        LogClaimedOverlayState()
    elseif aiTimerID == 5
        if g_outpostPlacementPending && !g_cancelPending
            Debug.Trace("[CPOI] Placement timer (300s) expired; cancelling pending placement", 0)
            AttemptCancelPendingPlacement()
        endif
    elseif aiTimerID == 6
        if g_outpostPlacementPending
            if !g_overlayRestored && !g_overlayTrackingReady && g_outpostOverlayHelper != None
                bool resumed = CPOI_Native.ResumePOIOverlayRecovery(g_outpostOverlayHelper)
                Debug.Trace("[CPOI] Retry ResumePOIOverlayRecovery returned " + resumed, 0)
                if !resumed
                    g_restoreRetryCount += 1
                    if g_restoreRetryCount <= 5
                        StartTimer(2.0, 6)
                    else
                        Debug.Trace("[CPOI] Retry ResumePOIOverlayRecovery failed after max retries", 0)
                    endif
                    return
                endif
                g_overlayTrackingReady = true
                g_restoreRetryCount = 0
            endif
            if g_outpostBeaconAccepted || g_pendingBeacon != None
                Debug.Trace("[CPOI] Executing retry for outpost placement completion", 0)
                ProcessSuccessfulOutpostPlacement()
            elseif g_cancelPending
                Debug.Trace("[CPOI] Executing retry for pending placement cancellation", 0)
                AttemptCancelPendingPlacement()
            else
                StartTimer(300.0, 5)
            endif
        endif
    elseif aiTimerID == 7
        if g_workshopSessionActive && g_workshopSessionHelper != None
            Debug.Trace("[CPOI] Executing retry for workshop session overlay restoration", 0)
            RestoreWorkshopOverlaySession()
        endif
    elseif aiTimerID == 8
        if g_workshopSessionHelper != None || g_workshopSessionActive
            Debug.Trace("[CPOI] Executing retry for workshop session load recovery", 0)
            AttemptRecoverWorkshopSession()
        endif
    elseif aiTimerID == 9
        if g_workshopReplacementPending
            RollbackPendingWorkshopReplacement("replacement timer expired")
        endif
    elseif aiTimerID == 10
        ; A timer serialized by an older build must not restart the cell scan.
        CancelDeferredOwnershipSweep()
    elseif aiTimerID == 11
        if g_outpostPlacementPending && g_outpostOverlayHelper != None && !g_cancelPending && !g_outpostBeaconAccepted
            Actor player = Game.GetPlayer()
            if player != None
                float dist = player.GetDistance(g_outpostOverlayHelper)
                int distInt = dist as int
                if dist <= g_outpostPlacementRadius && player.GetWorldSpace() == g_outpostOverlayHelper.GetWorldSpace()
                    Debug.Notification("Claim anchor: " + distInt + "m away. Check the beacon build area covers the POI.")
                else
                    Debug.Notification("Outside claim range. Move closer to the claim anchor.")
                endif
                StartTimer(3.0, 11)
            endif
        endif
    elseif aiTimerID == 12
        ; Retire a travel-observation timer serialized by an older build.
        CancelTimer(12)
    endif
EndEvent

Function LogClaimedOverlayState()
    Actor player = Game.GetPlayer()
    if player != None
        CPOI_Native.LogReferenceState(player, 4)
    endif

    if CPOI_ClaimedBeacons == None
        return
    endif

    int i = 0
    Location playerLocation = None
    if player != None
        playerLocation = player.GetCurrentLocation()
    endif
    while i < CPOI_ClaimedBeacons.GetSize()
        ObjectReference beaconRef = CPOI_ClaimedBeacons.GetAt(i) as ObjectReference
        if beaconRef != None && !beaconRef.IsDeleted()
            CPOI_Native.LogReferenceState(beaconRef, 7)
            if playerLocation != None && FindClaimedLocationForBeacon(beaconRef) == playerLocation
                LogPopulationSnapshot(beaconRef, playerLocation, 7)
            endif
        endif
        i += 1
    endwhile
EndFunction

int Function LogPopulationSnapshot(ObjectReference akCenter, Location akLocation, int aiStage)
    if !CPOI_Native.IsDiagnosticsEnabled()
        return -1
    endif
    if akCenter == None || akLocation == None || g_cassiopeiaVersion < 96
        Debug.Trace("[CPOI] Population snapshot unavailable stage=" + aiStage + " center=" + akCenter + " location=" + akLocation + " Cassiopeia=" + g_cassiopeiaVersion, 0)
        return -1
    endif
    ObjectReference snapshotCenter = akCenter
    if akCenter.GetCurrentLocation() != akLocation
        Actor snapshotPlayer = Game.GetPlayer()
        if snapshotPlayer != None && snapshotPlayer.GetCurrentLocation() == akLocation && snapshotPlayer.GetWorldSpace() == akCenter.GetWorldSpace()
            snapshotCenter = snapshotPlayer
        endif
    endif
    Cell claimCell = snapshotCenter.GetParentCell()
    Cell[] loadedCells = CassiopeiaPapyrusExtender.GetLoadedCells()
    if claimCell == None || loadedCells == None
        Debug.Trace("[CPOI] Population snapshot unavailable stage=" + aiStage + ": missing claim cell or loaded-cell snapshot", 0)
        return -1
    endif
    float radius = 200.0
    if CPOI_ClaimRadius != None && CPOI_ClaimRadius.GetValue() > 0.0
        radius = CPOI_ClaimRadius.GetValue()
    endif
    int result = CPOI_Native.LogReferencePopulation(snapshotCenter, claimCell, loadedCells, akLocation, radius, aiStage)
    Debug.Trace("[CPOI] Population snapshot stage=" + aiStage + " result=" + result + " center=" + GetDiagnosticFormID(snapshotCenter), 0)
    return result
EndFunction

; Operator-only observation, independent of claim/workshop automation.
Function LogMiniSiteState(String asLabel, int aiCheckpoint)
    if !CPOI_Native.IsDiagnosticsEnabled()
        return
    endif
    if g_cassiopeiaVersion < 96
        Debug.Trace("[CPOI] MiniSiteObservation-v1 rejected label=" + asLabel + " reason=Cassiopeia-9.6-required", 0)
        return
    endif
    Actor observer = Game.GetPlayer()
    Cell[] loadedCells = CassiopeiaPapyrusExtender.GetLoadedCells()
    if observer == None || loadedCells == None
        Debug.Trace("[CPOI] MiniSiteObservation-v1 rejected label=" + asLabel + " reason=missing-player-or-cells", 0)
        return
    endif
    int result = CPOI_Native.LogMiniSiteCensus(observer, loadedCells, observer.GetCurrentLocation(), aiCheckpoint)
    Debug.Trace("[CPOI] MiniSiteObservation-v1 label=" + asLabel + " checkpoint=" + aiCheckpoint + " result=" + result + " readOnly=true", 0)
EndFunction

; Diagnostic-only console probe for comparing inherited and newly built POI
; references. This function never mutates the selected reference or its links.
Function LogSelectedReferenceState(String asLabel)
    if !CPOI_Native.IsDiagnosticsEnabled()
        return
    endif
    if g_cassiopeiaVersion < 96
        Debug.Trace("[CPOI] Manual reference diagnostic rejected: Cassiopeia 9.6 required label=" + asLabel, 0)
        return
    endif

    ObjectReference selected = CassiopeiaPapyrusExtender.GetConsoleRef()
    if selected == None
        Debug.Trace("[CPOI] Manual reference diagnostic rejected: no console-selected reference label=" + asLabel, 0)
        return
    endif

    ; Reject unbound VM handles before trusting cached base/3D fields.
    bool nativeLogged = CPOI_Native.LogReferenceState(selected, 15)
    if !nativeLogged
        Debug.Trace("[CPOI] Manual reference diagnostic nativeLog label=" + asLabel + " result=false captureValid=false", 0)
        return
    endif

    Form baseForm = selected.GetBaseObject()
    string selectedID = GetDiagnosticFormID(selected)
    string baseID = GetDiagnosticFormID(baseForm)
    int persistentState = CassiopeiaPapyrusExtender.IsPersistent(selectedID)
    bool promotedPersistent = CassiopeiaPapyrusExtender.IsPromotedPersistent(selectedID)
    PackIn sourcePackIn = CassiopeiaPapyrusExtender.GetSourcePackIn(selected)
    PackIn groupedPackIn = CassiopeiaPapyrusExtender.GetGroupedPackIn(selected)
    Debug.Trace("[CPOI] MiniSiteObservation-v1 child label=" + asLabel + " ref=" + selectedID + " base=" + baseID + " persistentState=" + persistentState + " promotedPersistent=" + promotedPersistent + " sourcePackInBase=" + GetDiagnosticFormID(sourcePackIn) + " groupedPackInBase=" + GetDiagnosticFormID(groupedPackIn) + " nativeValid=true ownershipProven=false readOnly=true", 0)
    string selectedDescription = CassiopeiaPapyrusExtender.GetFormDescription(selected)
    string baseDescription = "None"
    if baseForm != None
        baseDescription = CassiopeiaPapyrusExtender.GetFormDescription(baseForm)
    endif
    ObjectReference workshopRef = selected.GetWorkshop()
    string workshopID = GetDiagnosticFormID(workshopRef)
    bool is3DLoaded = selected.Is3DLoaded()
    bool isWorkshopItem = CassiopeiaPapyrusExtender.IsWorkshopItem(selected)

    Keyword workshopItemKeyword = Game.GetFormFromFile(0x00054BA6, "Starfield.esm") as Keyword
    Keyword workshopKeyword = Game.GetFormFromFile(0x00054BA7, "Starfield.esm") as Keyword
    Keyword jobRefKeyword = Game.GetFormFromFile(0x00080A, "outpostworkshopassignments.esm") as Keyword
    Keyword assignmentLinkKeyword = Game.GetFormFromFile(0x000804, "outpostworkshopassignments.esm") as Keyword
    Keyword bedLinkKeyword = Game.GetFormFromFile(0x000934, "outpostworkshopassignments.esm") as Keyword
    Keyword sitOrSandboxNearLinkKeyword = Game.GetFormFromFile(0x000901, "outpostworkshopassignments.esm") as Keyword

    bool hasWorkshopItemKeyword = false
    bool hasWorkshopKeyword = false
    bool hasJobRefKeyword = false
    bool workshopItemKeywordResolved = workshopItemKeyword != None
    bool workshopKeywordResolved = workshopKeyword != None
    bool jobRefKeywordResolved = jobRefKeyword != None
    bool assignmentLinkKeywordResolved = assignmentLinkKeyword != None
    bool bedLinkKeywordResolved = bedLinkKeyword != None
    bool sitOrSandboxNearLinkKeywordResolved = sitOrSandboxNearLinkKeyword != None
    if workshopItemKeyword != None
        hasWorkshopItemKeyword = selected.HasKeyword(workshopItemKeyword)
    endif
    if workshopKeyword != None
        hasWorkshopKeyword = selected.HasKeyword(workshopKeyword)
    endif
    if jobRefKeyword != None
        hasJobRefKeyword = selected.HasKeyword(jobRefKeyword)
    endif

    ObjectReference workshopLink = None
    ObjectReference assignmentLink = None
    ObjectReference bedLink = None
    ObjectReference sitOrSandboxNearLink = None
    if workshopKeyword != None
        workshopLink = selected.GetLinkedRef(workshopKeyword)
    endif
    if assignmentLinkKeyword != None
        assignmentLink = selected.GetLinkedRef(assignmentLinkKeyword)
    endif
    if bedLinkKeyword != None
        bedLink = selected.GetLinkedRef(bedLinkKeyword)
    endif
    if sitOrSandboxNearLinkKeyword != None
        sitOrSandboxNearLink = selected.GetLinkedRef(sitOrSandboxNearLinkKeyword)
    endif

    Debug.Trace("[CPOI] Manual reference diagnostic label=" + asLabel + " ref=" + selectedID + " base=" + baseID + " is3DLoaded=" + is3DLoaded + " workshop=" + workshopID + " isWorkshopItem=" + isWorkshopItem, 0)
    Debug.Trace("[CPOI] Manual reference diagnostic description ref=" + selectedDescription + " base=" + baseDescription, 0)
    Debug.Trace("[CPOI] Manual reference diagnostic keyword ref=" + selectedID + " WorkshopItemKeyword=" + hasWorkshopItemKeyword + " resolved=" + workshopItemKeywordResolved + " WorkshopKeyword=" + hasWorkshopKeyword + " resolved=" + workshopKeywordResolved + " OWAJobRef=" + hasJobRefKeyword + " resolved=" + jobRefKeywordResolved, 0)
    Debug.Trace("[CPOI] Manual reference diagnostic links ref=" + selectedID + " StarfieldWorkshopKeyword=" + GetDiagnosticFormID(workshopLink) + " OWAAssignmentLink=" + GetDiagnosticFormID(assignmentLink) + " resolved=" + assignmentLinkKeywordResolved + " OWABedLink=" + GetDiagnosticFormID(bedLink) + " resolved=" + bedLinkKeywordResolved + " OWASitOrSandboxNearLink=" + GetDiagnosticFormID(sitOrSandboxNearLink) + " resolved=" + sitOrSandboxNearLinkKeywordResolved, 0)

    int outboundLinkCount = selected.CountLinkedRefChain(None, 8)
    if outboundLinkCount < 0
        outboundLinkCount = 0
    elseif outboundLinkCount > 8
        outboundLinkCount = 8
    endif
    Debug.Trace("[CPOI] Manual reference diagnostic genericLinks ref=" + selectedID + " count=" + outboundLinkCount, 0)
    int outboundLinkIndex = 0
    while outboundLinkIndex < outboundLinkCount
        ObjectReference outboundLink = selected.GetNthLinkedRef(outboundLinkIndex, None)
        string outboundLinkID = GetDiagnosticFormID(outboundLink)
        string outboundLinkDescription = "None"
        if outboundLink != None
            outboundLinkDescription = CassiopeiaPapyrusExtender.GetFormDescription(outboundLink)
        endif
        Debug.Trace("[CPOI] Manual reference diagnostic genericLink ref=" + selectedID + " index=" + outboundLinkIndex + " target=" + outboundLinkID + " description=" + outboundLinkDescription, 0)
        outboundLinkIndex += 1
    endwhile

    Debug.Trace("[CPOI] Manual reference diagnostic nativeLog label=" + asLabel + " ref=" + selectedID + " result=" + nativeLogged, 0)
EndFunction

; Operator-only, one-item engine workshop-commit trial. The native side admits
; only inherited Research Lab base 0001A21B and the exact active
; claimed workshop context. It performs no automatic batch enrollment.
Function CommitSelectedInheritedWorkshopItem()
    if g_cassiopeiaVersion < 96
        Debug.Notification("Workshop commit trial requires Cassiopeia 9.6.")
        return
    endif
    if !g_workshopSessionActive || g_workshopSessionBeacon == None || g_workshopSessionBeacon.IsDeleted()
        Debug.Notification("Enter the claimed POI workshop first.")
        return
    endif
    ObjectReference selected = CassiopeiaPapyrusExtender.GetConsoleRef()
    Actor player = Game.GetPlayer()
    if selected == None || player == None
        Debug.Notification("Select the inherited Research Lab first.")
        return
    endif
    float radius = 200.0
    if CPOI_ClaimRadius != None && CPOI_ClaimRadius.GetValue() > 0.0
        radius = CPOI_ClaimRadius.GetValue()
    endif
    bool committed = CPOI_Native.CommitSelectedInheritedWorkshopItem(selected, g_workshopSessionBeacon, player.GetCurrentLocation(), radius, CPOI_ReplacedOriginals, CPOI_WorkshopReplacements)
    Debug.Trace("[CPOI] Workshop commit trial selected=" + GetDiagnosticFormID(selected) + " committed=" + committed, 0)
    if committed
        Debug.Notification("Workshop commit accepted. Test highlight and movement only.")
    else
        Debug.Notification("Workshop commit rejected. Stop and share the log.")
    endif
EndFunction

; Bounded, fail-closed operator gate for the first decorative-static trial.
; Only the exact Starfield STAT 0x00237543 base is accepted. The selected
; reference is stored for an explicit later RestoreDecorationGate() call.
Function DisableSelectedDecorationGate()
    if g_cassiopeiaVersion < 96
        Debug.Trace("[CPOI] Decoration gate rejected: Cassiopeia 9.6 required", 0)
        Debug.Notification("Decoration gate unavailable: Cassiopeia 9.6 required.")
        return
    endif
    if g_decorationGateActive || g_decorationGateReference != None || g_decorationGateReferenceID != ""
        Debug.Trace("[CPOI] Decoration gate rejected: a reference is already tracked ref=" + g_decorationGateReferenceID, 0)
        Debug.Notification("A decoration is already gated; restore it first.")
        return
    endif

    ObjectReference selected = CassiopeiaPapyrusExtender.GetConsoleRef()
    if selected == None
        Debug.Trace("[CPOI] Decoration gate rejected: no console-selected reference", 0)
        Debug.Notification("Select one approved decoration in the console first.")
        return
    endif

    Form expectedBase = Game.GetFormFromFile(0x00237543, "Starfield.esm")
    Form selectedBase = selected.GetBaseObject()
    if expectedBase == None || selectedBase != expectedBase
        Debug.Trace("[CPOI] Decoration gate rejected: base mismatch ref=" + GetDiagnosticFormID(selected) + " base=" + GetDiagnosticFormID(selectedBase), 0)
        Debug.Notification("Decoration gate rejected: base is not approved.")
        return
    endif
    Actor player = Game.GetPlayer()
    Location currentLocation = None
    if player != None
        currentLocation = player.GetCurrentLocation()
    endif
    if player == None || currentLocation == None || CPOI_ClaimedPOIs == None || !CPOI_ClaimedPOIs.HasForm(currentLocation)
        Debug.Trace("[CPOI] Decoration gate rejected: player is not in a claimed location", 0)
        Debug.Notification("Decoration gate requires a claimed POI.")
        return
    endif

    ObjectReference beacon = FindTrackedBeaconFor(currentLocation)
    Form vanillaBeaconForm = Game.GetFormFromFile(0x002C60E9, "Starfield.esm")
    if beacon == None || vanillaBeaconForm == None || beacon.GetBaseObject() != vanillaBeaconForm || !CassiopeiaPapyrusExtender.IsOutpostBeacon(beacon)
        Debug.Trace("[CPOI] Decoration gate rejected: no tracked genuine beacon at current location", 0)
        Debug.Notification("Decoration gate requires the claimed POI beacon.")
        return
    endif
    if selected.GetCurrentLocation() != currentLocation || beacon.GetCurrentLocation() != currentLocation
        Debug.Trace("[CPOI] Decoration gate rejected: selected/beacon location mismatch ref=" + GetDiagnosticFormID(selected), 0)
        Debug.Notification("Decoration gate rejected: object is not at this POI.")
        return
    endif

    float gateRadius = 200.0
    if CPOI_ClaimRadius != None && CPOI_ClaimRadius.GetValue() > 0.0
        gateRadius = CPOI_ClaimRadius.GetValue()
    endif
    if selected.GetDistance(beacon) > gateRadius
        Debug.Trace("[CPOI] Decoration gate rejected: selected is outside claim radius ref=" + GetDiagnosticFormID(selected) + " radius=" + gateRadius, 0)
        Debug.Notification("Decoration gate rejected: object is outside the claim area.")
        return
    endif
    if selected.IsDeleted() || selected.IsDisabled() || !selected.Is3DLoaded()
        Debug.Trace("[CPOI] Decoration gate rejected: selected ref is deleted, disabled, or not loaded ref=" + GetDiagnosticFormID(selected), 0)
        Debug.Notification("Decoration gate rejected: object is not loaded and enabled.")
        return
    endif
    if selected.IsQuestItem() || CassiopeiaPapyrusExtender.IsAliasedRef(selected) || CassiopeiaPapyrusExtender.IsTemporaryReference(selected)
        Debug.Trace("[CPOI] Decoration gate rejected: quest/aliased/temporary ref=" + GetDiagnosticFormID(selected), 0)
        Debug.Notification("Decoration gate rejected: object is protected.")
        return
    endif
    if CassiopeiaPapyrusExtender.GetSourcePackIn(selected) != None || CassiopeiaPapyrusExtender.GetGroupedPackIn(selected) != None
        Debug.Trace("[CPOI] Decoration gate rejected: pack-in metadata present ref=" + GetDiagnosticFormID(selected), 0)
        Debug.Notification("Decoration gate rejected: object belongs to a pack-in.")
        return
    endif
    if selected.GetWorkshop() != None || CassiopeiaPapyrusExtender.IsWorkshopItem(selected)
        Debug.Trace("[CPOI] Decoration gate rejected: workshop object ref=" + GetDiagnosticFormID(selected), 0)
        Debug.Notification("Decoration gate rejected: workshop object.")
        return
    endif
    int decorationSafetyFlags = CPOI_Native.GetDecorationSafetyFlags(selected)
    if decorationSafetyFlags != 0
        Debug.Trace("[CPOI] Decoration gate rejected: native safety flags ref=" + GetDiagnosticFormID(selected) + " flags=" + decorationSafetyFlags, 0)
        Debug.Notification("Decoration gate rejected: native safety check failed.")
        return
    endif

    string selectedID = GetDiagnosticFormID(selected)
    bool nativeLogged = CPOI_Native.LogReferenceState(selected, 10)
    Debug.Trace("[CPOI] Decoration gate stage=10 ref=" + selectedID + " nativeLog=" + nativeLogged, 0)
    if !nativeLogged
        Debug.Trace("[CPOI] Decoration gate rejected: native stage-10 snapshot failed ref=" + selectedID, 0)
        Debug.Notification("Decoration gate rejected: native safety logging failed.")
        return
    endif
    g_decorationGateReference = selected
    g_decorationGateReferenceID = selectedID
    g_decorationGateActive = true
    selected.DisableNoWait(false)
    Utility.Wait(0.5)
    if selected.IsDisabled()
        Debug.Trace("[CPOI] Decoration gate disabled ref=" + selectedID, 0)
        Debug.Notification("Decoration disabled. Use RestoreDecorationGate when ready.")
        return
    endif

    Debug.Trace("[CPOI] Decoration gate disable verification failed; attempting rollback ref=" + selectedID, 0)
    selected.EnableNoWait(false)
    Utility.Wait(0.5)
    if !selected.IsDisabled()
        g_decorationGateReference = None
        g_decorationGateReferenceID = ""
        g_decorationGateActive = false
        Debug.Trace("[CPOI] Decoration gate rollback succeeded ref=" + selectedID, 0)
        Debug.Notification("Decoration gate failed; object was restored.")
    else
        Debug.Trace("[CPOI] Decoration gate rollback failed; retaining tracked state ref=" + selectedID, 0)
        Debug.Notification("Decoration gate failed; restore remains available.")
    endif
EndFunction

Function RestoreDecorationGate()
    if !g_decorationGateActive || g_decorationGateReference == None || g_decorationGateReferenceID == ""
        Debug.Trace("[CPOI] Decoration restore rejected: no active gate", 0)
        Debug.Notification("No decoration gate is active.")
        return
    endif

    ObjectReference stored = g_decorationGateReference
    string storedID = g_decorationGateReferenceID
    if GetDiagnosticFormID(stored) != storedID
        Debug.Trace("[CPOI] Decoration restore rejected: stored reference identity mismatch expected=" + storedID + " actual=" + GetDiagnosticFormID(stored), 0)
        Debug.Notification("Decoration restore blocked: reference identity changed.")
        return
    endif
    if stored.IsDeleted()
        Debug.Trace("[CPOI] Decoration restore rejected: stored reference is deleted ref=" + storedID, 0)
        Debug.Notification("Decoration restore blocked: stored object is gone.")
        return
    endif
    Form expectedBase = Game.GetFormFromFile(0x00237543, "Starfield.esm")
    if expectedBase == None || stored.GetBaseObject() != expectedBase
        Debug.Trace("[CPOI] Decoration restore rejected: stored base is not approved ref=" + storedID + " base=" + GetDiagnosticFormID(stored.GetBaseObject()), 0)
        Debug.Notification("Decoration restore blocked: approved base no longer matches.")
        return
    endif

    bool nativeLogged = CPOI_Native.LogReferenceState(stored, 11)
    Debug.Trace("[CPOI] Decoration restore stage=11 ref=" + storedID + " nativeLog=" + nativeLogged, 0)
    stored.EnableNoWait(false)
    Utility.Wait(0.5)
    if stored.IsDisabled()
        Debug.Trace("[CPOI] Decoration restore verification failed ref=" + storedID, 0)
        Debug.Notification("Decoration restore failed; gate remains active.")
        return
    endif

    bool restoreLogged = CPOI_Native.LogReferenceState(stored, 12)
    Debug.Trace("[CPOI] Decoration restore stage=12 ref=" + storedID + " nativeLog=" + restoreLogged, 0)
    g_decorationGateReference = None
    g_decorationGateReferenceID = ""
    g_decorationGateActive = false
    Debug.Notification("Decoration restored.")
EndFunction

Function BeginWorkshopReplacement()
    if !g_nativeLoadReady
        return
    endif
    if g_cassiopeiaVersion < 96
        Debug.Trace("[CPOI] Workshop replacement rejected: Cassiopeia 9.6 required", 0)
        Debug.Notification("Workshop replacement requires Cassiopeia 9.6.")
        return
    endif
    if g_workshopReplacementPending
        Debug.Trace("[CPOI] Workshop replacement rejected: transaction already pending", 0)
        Debug.Notification("A workshop replacement is already pending.")
        return
    endif
    if g_claimInProgress || g_claimPending || g_outpostPlacementPending || g_decorationGateActive
        Debug.Trace("[CPOI] Workshop replacement rejected: conflicting CPOI transaction", 0)
        Debug.Notification("Workshop replacement is unavailable during another CPOI operation.")
        return
    endif
    if !g_workshopSessionActive || g_workshopSessionBeacon == None || g_workshopSessionBeacon.IsDeleted()
        Debug.Trace("[CPOI] Workshop replacement rejected: no active claimed workshop session", 0)
        Debug.Notification("Enter the claimed POI workshop first.")
        return
    endif

    ObjectReference selected = CassiopeiaPapyrusExtender.GetConsoleRef()
    Actor player = Game.GetPlayer()
    if selected == None || player == None
        Debug.Trace("[CPOI] Workshop replacement rejected: missing selected reference or player", 0)
        Debug.Notification("Select the inherited furniture reference first.")
        return
    endif

    Form selectedBase = selected.GetBaseObject()
    if !IsWorkshopReplacementBaseAllowed(selectedBase)
        Debug.Trace("[CPOI] Workshop replacement rejected: unsupported base ref=" + GetDiagnosticFormID(selected) + " base=" + GetDiagnosticFormID(selectedBase), 0)
        Debug.Notification("Select an object available in the outpost build menu.")
        return
    endif

    Location currentLocation = player.GetCurrentLocation()
    ObjectReference beacon = FindTrackedBeaconFor(currentLocation)
    Form vanillaBeaconForm = Game.GetFormFromFile(0x002C60E9, "Starfield.esm")
    if currentLocation == None || CPOI_ClaimedPOIs == None || !CPOI_ClaimedPOIs.HasForm(currentLocation) || beacon == None || beacon != g_workshopSessionBeacon || vanillaBeaconForm == None || beacon.GetBaseObject() != vanillaBeaconForm || !CassiopeiaPapyrusExtender.IsOutpostBeacon(beacon)
        Debug.Trace("[CPOI] Workshop replacement rejected: claimed beacon/location mismatch", 0)
        Debug.Notification("Workshop replacement requires the active claimed POI beacon.")
        return
    endif
    Cell selectedCell = selected.GetParentCell()
    if selected.GetCurrentLocation() != currentLocation || beacon.GetCurrentLocation() != currentLocation || selectedCell == None || selectedCell != beacon.GetParentCell()
        Debug.Trace("[CPOI] Workshop replacement rejected: selected reference is not at the active POI", 0)
        Debug.Notification("The selected reference is not at this claimed POI.")
        return
    endif

    float replacementRadius = 200.0
    if CPOI_ClaimRadius != None && CPOI_ClaimRadius.GetValue() > 0.0
        replacementRadius = CPOI_ClaimRadius.GetValue()
    endif
    if selected.GetDistance(beacon) > replacementRadius
        Debug.Trace("[CPOI] Workshop replacement rejected: selected reference outside radius", 0)
        Debug.Notification("The selected reference is outside the claimed POI area.")
        return
    endif
    if selected.IsDeleted() || selected.IsDisabled() || !selected.Is3DLoaded()
        Debug.Trace("[CPOI] Workshop replacement rejected: selected reference is not loaded and enabled", 0)
        Debug.Notification("The selected reference must be loaded and enabled.")
        return
    endif
    if selected.IsQuestItem() || CassiopeiaPapyrusExtender.IsAliasedRef(selected) || CassiopeiaPapyrusExtender.IsTemporaryReference(selected)
        Debug.Trace("[CPOI] Workshop replacement rejected: selected reference is protected", 0)
        Debug.Notification("The selected reference is protected.")
        return
    endif
    if CassiopeiaPapyrusExtender.GetSourcePackIn(selected) != None || CassiopeiaPapyrusExtender.GetGroupedPackIn(selected) != None
        Debug.Trace("[CPOI] Workshop replacement rejected: selected reference is a pack-in", 0)
        Debug.Notification("Pack-in references cannot be replaced.")
        return
    endif
    if selected.GetWorkshop() != None || CassiopeiaPapyrusExtender.IsWorkshopItem(selected)
        Debug.Trace("[CPOI] Workshop replacement rejected: selected reference is already a workshop item", 0)
        Debug.Notification("Select the inherited, non-workshop reference.")
        return
    endif
    if !WorkshopReplacementLedgerIsValid()
        Debug.Trace("[CPOI] Workshop replacement rejected: replacement ledger is invalid", 0)
        Debug.Notification("Workshop replacement ledger is invalid; no mutation was made.")
        return
    endif
    if CPOI_ReplacedOriginals.HasForm(selected) || CPOI_WorkshopReplacements.HasForm(selected)
        Debug.Trace("[CPOI] Workshop replacement rejected: selected reference is already in the ledger", 0)
        Debug.Notification("That reference already has a replacement record.")
        return
    endif

    int safetyFlags = CPOI_Native.GetWorkshopReplacementSafetyFlags(selected)
    if safetyFlags != 0
        Debug.Trace("[CPOI] Workshop replacement rejected: native safety flags=" + safetyFlags, 0)
        Debug.Notification("Workshop replacement safety check failed.")
        return
    endif
    string selectedID = GetDiagnosticFormID(selected)
    bool nativeLogged = CPOI_Native.LogReferenceState(selected, 13)
    Debug.Trace("[CPOI] Workshop replacement stage=13 ref=" + selectedID + " nativeLog=" + nativeLogged, 0)
    if !nativeLogged
        Debug.Trace("[CPOI] Workshop replacement rejected: native stage-13 snapshot failed", 0)
        Debug.Notification("Workshop replacement diagnostic failed; no mutation was made.")
        return
    endif

    g_workshopReplacementOriginal = selected
    g_workshopReplacementOriginalID = selectedID
    g_workshopReplacementBase = selectedBase
    g_workshopReplacementBeacon = beacon
    g_workshopReplacementLocation = currentLocation
    g_workshopReplacementCell = selectedCell
    g_workshopReplacementX = selected.GetPositionX()
    g_workshopReplacementY = selected.GetPositionY()
    g_workshopReplacementZ = selected.GetPositionZ()
    g_workshopReplacementAngleX = selected.GetAngleX()
    g_workshopReplacementAngleY = selected.GetAngleY()
    g_workshopReplacementAngleZ = selected.GetAngleZ()
    g_workshopReplacementCandidate = None
    g_workshopReplacementCandidateID = ""
    g_workshopReplacementPending = true
    g_workshopReplacementHidden = false
    g_workshopReplacementListening = false
    g_workshopReplacementRetryCount = 0

    bool replacementEventRegistered = Self.RegisterForRemoteEvent(beacon, "OnWorkshopObjectPlaced")
    if !replacementEventRegistered
        Debug.Trace("[CPOI] Workshop replacement rejected: placement-event registration failed", 0)
        ClearPendingWorkshopReplacement()
        Debug.Notification("Workshop replacement could not monitor item placement.")
        return
    endif
    g_workshopReplacementListening = true
    StartTimer(120.0, 9)
    selected.DisableNoWait(false)
    Utility.Wait(0.5)
    if selected.IsDisabled()
        g_workshopReplacementHidden = true
        Debug.Trace("[CPOI] Workshop replacement pending original=" + selectedID + " beacon=" + GetDiagnosticFormID(beacon), 0)
        Debug.Notification("Original object hidden. Build the same object now.")
        return
    endif

    Debug.Trace("[CPOI] Workshop replacement failed to hide original ref=" + selectedID, 0)
    RollbackPendingWorkshopReplacement("original hide verification failed")
EndFunction

Function CancelWorkshopReplacement()
    if !g_workshopReplacementPending
        Debug.Trace("[CPOI] Workshop replacement cancellation: no pending transaction", 0)
        Debug.Notification("No workshop replacement is pending.")
        return
    endif
    RollbackPendingWorkshopReplacement("operator cancellation")
EndFunction

bool Function IsWorkshopReplacementBaseAllowed(Form akBase)
    if akBase == None
        return false
    endif
    return CPOI_Native.IsRuntimeOutpostBuildable(akBase)
EndFunction

bool Function WorkshopReplacementLedgerIsValid()
    if CPOI_ReplacedOriginals == None || CPOI_WorkshopReplacements == None
        return false
    endif
    int originalCount = CPOI_ReplacedOriginals.GetSize()
    if originalCount != CPOI_WorkshopReplacements.GetSize()
        return false
    endif
    int i = 0
    while i < originalCount
        ObjectReference original = CPOI_ReplacedOriginals.GetAt(i) as ObjectReference
        ObjectReference replacement = CPOI_WorkshopReplacements.GetAt(i) as ObjectReference
        if original == None || replacement == None || original == replacement
            return false
        endif
        if CPOI_ReplacedOriginals.Find(original) != i || CPOI_WorkshopReplacements.Find(replacement) != i
            return false
        endif
        if CPOI_ReplacedOriginals.HasForm(replacement) || CPOI_WorkshopReplacements.HasForm(original)
            return false
        endif
        i += 1
    endwhile
    return true
EndFunction

Function UnregisterWorkshopReplacementListener()
    if g_workshopReplacementListening && g_workshopReplacementBeacon != None
        Self.UnregisterForRemoteEvent(g_workshopReplacementBeacon, "OnWorkshopObjectPlaced")
    endif
    g_workshopReplacementListening = false
EndFunction

Function ClearPendingWorkshopReplacement()
    CancelTimer(9)
    UnregisterWorkshopReplacementListener()
    g_workshopReplacementOriginal = None
    g_workshopReplacementOriginalID = ""
    g_workshopReplacementBase = None
    g_workshopReplacementBeacon = None
    g_workshopReplacementLocation = None
    g_workshopReplacementCell = None
    g_workshopReplacementX = 0.0
    g_workshopReplacementY = 0.0
    g_workshopReplacementZ = 0.0
    g_workshopReplacementAngleX = 0.0
    g_workshopReplacementAngleY = 0.0
    g_workshopReplacementAngleZ = 0.0
    g_workshopReplacementCandidate = None
    g_workshopReplacementCandidateID = ""
    g_workshopReplacementPending = false
    g_workshopReplacementHidden = false
    g_workshopReplacementRetryCount = 0
EndFunction

Function RollbackPendingWorkshopReplacement(String asReason)
    if !g_workshopReplacementPending
        return
    endif
    UnregisterWorkshopReplacementListener()
    CancelTimer(9)

    ObjectReference original = g_workshopReplacementOriginal
    string originalID = g_workshopReplacementOriginalID
    if original == None || originalID == "" || GetDiagnosticFormID(original) != originalID || original.IsDeleted() || !IsWorkshopReplacementBaseAllowed(original.GetBaseObject())
        Debug.Trace("[CPOI] Workshop replacement rollback blocked reason=" + asReason + " original=" + originalID, 0)
        Debug.Notification("Workshop replacement rollback is blocked; original reference identity is unavailable.")
        StartTimer(120.0, 9)
        return
    endif

    if !original.IsDisabled()
        Debug.Trace("[CPOI] Workshop replacement rollback required no mutation reason=" + asReason + " original=" + originalID, 0)
        ClearPendingWorkshopReplacement()
        Debug.Notification("Workshop replacement cancelled; original furniture is enabled.")
        return
    endif

    if original.GetCurrentLocation() != g_workshopReplacementLocation || original.GetParentCell() != g_workshopReplacementCell || original.GetPositionX() != g_workshopReplacementX || original.GetPositionY() != g_workshopReplacementY || original.GetPositionZ() != g_workshopReplacementZ || original.GetAngleX() != g_workshopReplacementAngleX || original.GetAngleY() != g_workshopReplacementAngleY || original.GetAngleZ() != g_workshopReplacementAngleZ
        Debug.Trace("[CPOI] Workshop replacement rollback blocked: original state changed reason=" + asReason + " original=" + originalID, 0)
        Debug.Notification("Workshop replacement rollback blocked because the original reference changed.")
        StartTimer(120.0, 9)
        return
    endif

    original.EnableNoWait(false)
    Utility.Wait(0.5)
    if original.IsDisabled()
        g_workshopReplacementRetryCount += 1
        Debug.Trace("[CPOI] Workshop replacement rollback failed reason=" + asReason + " original=" + originalID + " retry=" + g_workshopReplacementRetryCount, 0)
        Debug.Notification("Original furniture remains hidden; rollback will be retried.")
        StartTimer(120.0, 9)
        return
    endif

    Debug.Trace("[CPOI] Workshop replacement rollback succeeded reason=" + asReason + " original=" + originalID + " candidate=" + g_workshopReplacementCandidateID, 0)
    ClearPendingWorkshopReplacement()
    Debug.Notification("Workshop replacement cancelled; original furniture restored.")
EndFunction

Function RemovePartialWorkshopReplacementLedger(Form akOriginal, Form akReplacement)
    if CPOI_ReplacedOriginals != None && akOriginal != None
        CPOI_ReplacedOriginals.RemoveAddedForm(akOriginal)
    endif
    if CPOI_WorkshopReplacements != None && akReplacement != None
        CPOI_WorkshopReplacements.RemoveAddedForm(akReplacement)
    endif
EndFunction

string Function GetDiagnosticFormID(Form akForm)
    if akForm == None
        return "None"
    endif
    if g_cassiopeiaVersion >= 96
        return CassiopeiaPapyrusExtender.GetHexFormID(akForm)
    endif
    return "0x" + Utility.IntToHex(akForm.GetFormID())
EndFunction

; Aliases used only as population collections do not make an ordinary spawned
; enemy a story character. Never clear aliases or change their owning quests.
bool Function CleanupAliasesAllowActor(Actor akActor)
    if !CassiopeiaPapyrusExtender.IsAliasedRef(akActor)
        return true
    endif
    if !CassiopeiaPapyrusExtender.IsCreated(akActor)
        return false
    endif
    Alias[] holders = CassiopeiaPapyrusExtender.GetAliases(akActor)
    if holders == None || holders.Length == 0 || holders.Length > 64
        return false
    endif
    int i = 0
    while i < holders.Length
        Alias holder = holders[i]
        if holder == None
            return false
        endif
        Quest owner = holder.GetOwningQuest()
        if owner == None
            return false
        endif
        ; An active individual reference alias can be an objective, captive,
        ; evidence body or named target, even when its actor base is generic.
        if !(holder is RefCollectionAlias)
            if !(holder is ReferenceAlias) || (owner.IsRunning() && !owner.IsCompleted())
                Debug.Trace("[CPOI] CorpseCleanup-v4 protected actor=" + GetDiagnosticFormID(akActor) + " quest=" + owner + " alias=" + CassiopeiaPapyrusExtender.GetAliasName(holder), 0)
                return false
            endif
        endif
        i += 1
    endwhile
    Debug.Trace("[CPOI] CorpseCleanup-v4 allowed generic spawned corpse with collection/inactive aliases actor=" + GetDiagnosticFormID(akActor), 0)
    return true
EndFunction

bool Function IsCleanupCandidate(ObjectReference akReference, ObjectReference akCenter, float afRadius, Actor akPlayer)
    Actor corpse = akReference as Actor
    if corpse == None || akCenter == None || corpse == akPlayer || corpse == akCenter
        return false
    endif
    if !corpse.IsDead()
        return false
    endif
    if corpse.IsDeleted() || corpse.IsDisabled()
        Debug.Trace("[CPOI] CorpseCleanup-v4 rejected deleted/disabled actor=" + GetDiagnosticFormID(corpse), 0)
        return false
    endif
    WorldSpace world = akCenter.GetWorldSpace()
    Cell corpseCell = corpse.GetParentCell()
    if world == None || corpse.GetWorldSpace() != world || corpseCell == None
        Debug.Trace("[CPOI] CorpseCleanup-v4 rejected world/cell actor=" + GetDiagnosticFormID(corpse) + " actorWorld=" + corpse.GetWorldSpace() + " claimWorld=" + world + " actorCell=" + corpseCell, 0)
        return false
    endif
    if corpseCell.IsInterior() || (!corpseCell.IsAttached() && !corpse.Is3DLoaded())
        Debug.Trace("[CPOI] CorpseCleanup-v4 rejected interior/unloaded actor=" + GetDiagnosticFormID(corpse) + " cell=" + corpseCell, 0)
        return false
    endif
    float distance = corpse.GetDistance(akCenter)
    if distance > afRadius
        Debug.Trace("[CPOI] CorpseCleanup-v4 rejected distance actor=" + GetDiagnosticFormID(corpse) + " distance=" + distance + " radius=" + afRadius, 0)
        return false
    endif
    if corpse.IsQuestItem() || corpse.IsEssential() || corpse.IsPlayerTeammate()
        Debug.Trace("[CPOI] CorpseCleanup-v4 protected quest/essential/teammate actor=" + GetDiagnosticFormID(corpse), 0)
        return false
    endif
    ActorBase actorType = corpse.GetLeveledActorBase()
    if actorType == None || actorType.IsUnique() || actorType.IsProtected() || actorType.IsEssential()
        Debug.Trace("[CPOI] CorpseCleanup-v4 protected unique/protected/essential base actor=" + GetDiagnosticFormID(corpse), 0)
        return false
    endif
    if g_cassiopeiaVersion < 100 || !CleanupAliasesAllowActor(corpse)
        Debug.Trace("[CPOI] CorpseCleanup-v4 rejected alias metadata/policy actor=" + GetDiagnosticFormID(corpse), 0)
        return false
    endif
    return true
EndFunction

bool Function IsCleanupContextCurrent(Actor akPlayer, Cell akCell, Location akLocation, int aiRequest, float afRadius = 500.0)
    if aiRequest != g_claimRequest || !g_nativeLoadReady || akPlayer == None || akLocation == None || akPlayer.IsInCombat()
        return false
    endif
    ObjectReference beacon = FindTrackedBeaconFor(akLocation)
    Cell currentCell = akPlayer.GetParentCell()
    if beacon == None || currentCell == None || currentCell.IsInterior() || beacon.GetWorldSpace() == None
        return false
    endif
    ; Creating a real outpost can reassign the player's exterior cell/location
    ; while standing still. Scope cleanup to the live claim, world and distance.
    return akPlayer.GetWorldSpace() == beacon.GetWorldSpace() && akPlayer.GetDistance(beacon) <= afRadius
EndFunction

; Retained signature only. Broad cell arrays caused excessive VM work in v2;
; never start another prop/reference sweep, including through an old caller.
Actor[] Function CollectCleanupCellActors(Cell akCell, Actor akPlayer, float afRadius, Cell akClaimCell, Location akLocation, int aiRequest)
    return new Actor[0]
EndFunction

int Function CleanupActorBatch(Actor[] akCandidates, Actor[] akSeen, Actor akPlayer, float afRadius, Cell akCell, Location akLocation, int aiRequest)
    ; Retired v2/v3 dispatch. The active worker owns a single local candidate
    ; array; never share an initially empty seen-array between helper calls.
    return 0
EndFunction

; Preserve the old helper signature for saved stacks, without the96-item cutoff.
int Function CleanupHighActors(Actor akPlayer, float afRadius, Cell akCell, Location akLocation, int aiRequest)
    return CleanupSite(akPlayer, akCell, akLocation, afRadius, aiRequest)
EndFunction

int Function CleanupSite(Actor akPlayer, Cell akCell, Location akLocation, float afRadius, int aiRequest)
    float radius = Math.Min(1000.0, Math.Max(500.0, afRadius * 2.0))
    if !IsCleanupContextCurrent(akPlayer, akCell, akLocation, aiRequest, radius) || g_cassiopeiaVersion < 100
        Debug.Trace("[CPOI] CorpseCleanup-v4 refused: claim/world/range/request/combat context", 0)
        return 0
    endif
    ObjectReference center = FindTrackedBeaconFor(akLocation)
    ; A nonempty local array removes empty/None and cross-function mutation
    ; ambiguity. Index0 stays None and is deliberately skipped by the worker.
    Actor[] candidates = new Actor[1]
    Actor[] high = CassiopeiaPapyrusExtender.GetHighActors()
    int highCount = high.Length
    int i = 0
    while i < highCount && candidates.Length < 1025
        if high[i] != None && candidates.Find(high[i]) < 0
            candidates.Add(high[i])
        endif
        i += 1
    endwhile
    high = None
    Actor[] localActors = akLocation.GetActors()
    int locationCount = localActors.Length
    i = 0
    while i < locationCount && candidates.Length < 1025
        if localActors[i] != None && candidates.Find(localActors[i]) < 0
            candidates.Add(localActors[i])
        endif
        i += 1
    endwhile
    localActors = None
    int disabled = 0
    int scanned = 0
    int dead = 0
    int rejected = 0
    Debug.Trace("[CPOI] CorpseCleanup-v4 begin radius=" + radius + " highActors=" + highCount + " locationActors=" + locationCount + " candidates=" + (candidates.Length - 1), 0)
    i = 1
    while i < candidates.Length
        if !IsCleanupContextCurrent(akPlayer, akCell, akLocation, aiRequest, radius)
            Debug.Trace("[CPOI] CorpseCleanup-v4 stopped: claim/world/range/request/combat changed scanned=" + scanned + " disabled=" + disabled, 0)
            return disabled
        endif
        Actor corpse = candidates[i]
        if corpse != None
            scanned += 1 ; Count every actor screened, before any candidate filter.
            if corpse.IsDead()
                dead += 1
            endif
            if IsCleanupCandidate(corpse, center, radius, akPlayer)
                if !IsCleanupContextCurrent(akPlayer, akCell, akLocation, aiRequest, radius)
                    return disabled
                endif
                if corpse.IsDead() && !corpse.IsDeleted() && !corpse.IsDisabled() && !corpse.IsQuestItem() && !corpse.IsEssential() && !corpse.IsPlayerTeammate()
                    corpse.DisableNoWait(false)
                    disabled += 1
                endif
            else
                rejected += 1
            endif
        endif
        i += 1
        if scanned % 16 == 0
            Utility.Wait(0.01)
        endif
    endwhile
    Debug.Trace("[CPOI] CorpseCleanup-v4 complete candidates=" + (candidates.Length - 1) + " scanned=" + scanned + " dead=" + dead + " rejected=" + rejected + " disabled=" + disabled + " actorLimitReached=" + (candidates.Length >= 1025), 0)
    return disabled
EndFunction

Function ClearPendingOutpostPlacement()
    g_pendingOriginalLandingMarker = None
    g_cleanupAfterClaim = false
    g_outpostPlacementPending = false
    g_outpostPlacementPlayer = None
    g_outpostPlacementCell = None
    g_outpostPlacementLocation = None
    g_outpostOverlayHelper = None
    g_outpostPlacementRadius = 0.0
    g_pendingBeacon = None
    g_outpostBeaconAccepted = false
    g_initialBindingVerified = false
    g_overlayRestored = false
    g_overlayTrackingReady = false
    g_cancelPending = false
    g_restoreRetryCount = 0
    g_workshopStartDeferred = false
    g_workshopDeferredBeacon = None
    g_claimInProgress = false
    g_placementRecoveryBusy = false
    CancelTimer(2)
    CancelTimer(5)
    CancelTimer(6)
    CancelTimer(11)
EndFunction

bool Function FinalizeClaimForNativePlacement(Actor akPlayer, Cell akCell, Location akLocation)
    if akPlayer == None || akCell == None || akLocation == None || CPOI_ClaimedPOIs == None || CPOI_ClaimedCount == None
        Debug.Trace("[CPOI] FinalizeClaimForNativePlacement rejected incomplete state", 0)
        return false
    endif

    bool addedClaim = false
    Debug.Trace("[CPOI] Finalizing persistent POI claim: " + akLocation, 0)
    ; Keep ownership/privacy untouched. VanillaOutpostIdentity-v1 also leaves
    ; vanilla's genuine outpost keyword intact; its regeneration role is under test.
    Keyword locTypeOutpost = Game.GetFormFromFile(0x000234F1, "Starfield.esm") as Keyword
    if locTypeOutpost != None && akLocation.HasKeyword(locTypeOutpost)
        Debug.Trace("[CPOI] VanillaOutpostIdentity-v1 finalize: preserved vanilla LocTypeOutpost on " + akLocation, 0)
    endif

    if !CPOI_ClaimedPOIs.HasForm(akLocation)
        CPOI_ClaimedPOIs.AddForm(akLocation)
        if !CPOI_ClaimedPOIs.HasForm(akLocation)
            return false
        endif
        addedClaim = true
    endif
    if addedClaim && CPOI_ClaimedCount != None
        CPOI_ClaimedCount.SetValue(CPOI_ClaimedCount.GetValue() + 1.0)
    endif

    akLocation.SetExplored(true)
    if !EnsureClaimResetProtection(FindTrackedBeaconFor(akLocation), akLocation, g_claimRequest)
        Debug.Trace("[CPOI] ClaimResetProtection-v1 new claim is tracked but reset protection could not be verified", 0)
        Debug.Notification("Claim created, but reset protection needs attention. Check the CPOI Papyrus log.")
    endif
    Debug.Trace("[CPOI] Successfully finalized claim for location: " + akLocation, 0)
    return true
EndFunction

Function CancelDeferredOwnershipSweep()
    CancelTimer(10)
    g_deferredOwnershipBeacon = None
    g_deferredOwnershipCell = None
    g_deferredOwnershipRadius = 0.0
EndFunction

Function ClearReferenceOwnership(ObjectReference akBeacon, Cell akCell, float afRadius)
    ; Keep the signature for older callers, but never capture a cell-wide array
    ; of procedural refs across latent calls. Old suspended save frames require
    ; a pre-claim save; replacing this function cannot safely repair those frames.
    CancelDeferredOwnershipSweep()
    Debug.Trace("[CPOI] OwnershipSweepIsolation-v1: per-reference ownership sweep disabled", 0)
EndFunction

Function SchedulePlacementRecovery(String asReason)
    g_restoreRetryCount += 1
    if g_restoreRetryCount <= 5
        Debug.Trace("[CPOI] Placement recovery retry " + g_restoreRetryCount + "/5: " + asReason, 0)
        StartTimer(2.0, 6)
    else
        Debug.Trace("[CPOI] Placement recovery stopped; retaining beacon and helper: " + asReason, 0)
        Debug.Notification("POI claim recovery is incomplete. Stop and preserve the logs.")
    endif
EndFunction

Function ProcessSuccessfulOutpostPlacement()
    if !g_nativeLoadReady
        return
    endif
    if g_placementRecoveryBusy
        StartTimer(2.0, 6)
        return
    endif
    int request = g_claimRequest
    g_placementRecoveryBusy = true
    CompletePendingOutpostPlacement()
    if request == g_claimRequest
        g_placementRecoveryBusy = false
    endif
EndFunction

Function CompletePendingOutpostPlacement()
    int completionRequest = g_claimRequest
    if !g_outpostPlacementPending || g_outpostOverlayHelper == None
        Debug.Trace("[CPOI] ProcessSuccessfulOutpostPlacement rejected invalid state", 0)
        return
    endif

    if g_pendingBeacon != None
        g_outpostBeaconAccepted = true
    endif
    if !g_outpostBeaconAccepted
        Debug.Trace("[CPOI] Completion rejected: no accepted vanilla beacon", 0)
        return
    endif
    g_cancelPending = false
    CancelTimer(5)

    ; Capture proof before RestorePOIOverlay retires the native claim token.
    ; On load only a previously captured, save-local result may survive.
    if !g_overlayRestored && g_overlayTrackingReady && !g_initialBindingVerified
        g_initialBindingVerified = CPOI_Native.IsInitialClaimBound(g_outpostOverlayHelper, g_pendingBeacon)
    endif

    ; 1. Restore the POI overlay first before finalizing claim bookkeeping.
    if !g_overlayRestored
        if !g_overlayTrackingReady
            Debug.Trace("[CPOI] ProcessSuccessfulOutpostPlacement waiting for native overlay tracking", 0)
            StartTimer(2.0, 6)
            return
        endif
        bool restored = CPOI_Native.RestorePOIOverlay(g_outpostOverlayHelper)
        if restored
            g_overlayRestored = true
            g_overlayTrackingReady = false
            g_restoreRetryCount = 0
            Debug.Trace("[CPOI] POI overlay successfully restored for helper: " + g_outpostOverlayHelper, 0)
        else
            g_restoreRetryCount += 1
            if g_restoreRetryCount <= 5
                Debug.Trace("[CPOI] RestorePOIOverlay failed on completion; scheduling retry " + g_restoreRetryCount + "/5", 0)
                StartTimer(2.0, 6)
                return
            else
                Debug.Trace("[CPOI] RestorePOIOverlay failed after max retries on completion; retaining state for safety", 0)
                Debug.Notification("Warning: POI overlay restoration could not be verified.")
                return
            endif
        endif
    endif

    if !g_initialBindingVerified
        Debug.Trace("[CPOI] Claim completion blocked: no exact original-overlay binding proof; genuine beacon and recovery state retained", 0)
        Debug.Notification("Outpost exists, but POI binding was not verified. Preserve logs and stop this trial.")
        return
    endif

    ; 2. Validate the genuine beacon. Inherited CONT enrollment remains disabled.
    if g_pendingBeacon == None || g_pendingBeacon.IsDeleted()
        Debug.Trace("[CPOI] Accepted beacon unavailable; retaining claim recovery state", 0)
        Debug.Notification("Accepted outpost beacon is unavailable. Stop and preserve the logs.")
        return
    endif
    if g_outpostPlacementPlayer == None || g_outpostPlacementPlayer.GetCurrentLocation() != g_outpostPlacementLocation
        Debug.Trace("[CPOI] Accepted outpost completion deferred until return to the pending POI", 0)
        return
    endif

    ; BeaconMetadataPreservation-v1: preserve the vanilla beacon's source/location
    ; extras. The prior sanitation boundary has no independent persistence proof.
    Keyword locTypeOutpost = Game.GetFormFromFile(0x000234F1, "Starfield.esm") as Keyword
    if locTypeOutpost != None && g_outpostPlacementLocation != None && g_outpostPlacementLocation.HasKeyword(locTypeOutpost)
        if g_pendingBeacon.GetCurrentLocation() != g_outpostPlacementLocation
            Debug.Trace("[CPOI] VanillaLocationSeparation-v1 FAILED: POI location acquired LocTypeOutpost despite distinct beacon location", 0)
            Debug.Notification("POI location became an outpost. Stop trial and preserve logs.")
        endif
        Debug.Trace("[CPOI] VanillaOutpostIdentity-v1 placement: preserved vanilla LocTypeOutpost on " + g_outpostPlacementLocation, 0)
    endif

    ; Procedural container enrollment has no verified ownership/persistence contract.
    Debug.Trace("[CPOI] ProceduralPreservation-v1: claim-time container enrollment bypassed", 0)

    ; 3.5. Do not hold procedural refs in a deferred ownership scan across travel.
    ; Explicit item owners remain.
    CancelDeferredOwnershipSweep()
    Debug.Trace("[CPOI] OwnershipSweepIsolation-v1: automatic ownership scan not scheduled", 0)

    ; Record the exact POI/beacon pair before publishing the location/count.
    ; Legacy field names are retained for save compatibility; new same-location
    ; claims also have an explicit pair rather than relying on a mutable location.
    if !RecordSplitClaimPair(g_pendingBeacon, g_outpostPlacementLocation)
        Debug.Notification("Claim pairing failed. Preserve logs and stop this trial.")
        return
    endif
    bool beaconAlreadyTracked = CPOI_ClaimedBeacons.HasForm(g_pendingBeacon)
    if !beaconAlreadyTracked
        CPOI_ClaimedBeacons.AddForm(g_pendingBeacon)
    endif
    if !CPOI_ClaimedBeacons.HasForm(g_pendingBeacon)
        Debug.Notification("Beacon ledger failed. Preserve logs and stop this trial.")
        return
    endif
    if !CPOI_Native.InitializeInheritedProtection(g_pendingBeacon)
        Debug.Trace("[CPOI] InheritedTeardown-v4 fresh-claim protection failed; completion retained for recovery", 0)
        Debug.Notification("Inherited-object teardown protection could not be initialized. Stop and preserve logs.")
        return
    endif
    bool finalized = FinalizeClaimForNativePlacement(g_outpostPlacementPlayer, g_outpostPlacementCell, g_outpostPlacementLocation)
    if !finalized
        if !beaconAlreadyTracked
            CPOI_ClaimedBeacons.RemoveAddedForm(g_pendingBeacon)
        endif
        Debug.Notification("Outpost exists, but POI bookkeeping needs recovery.")
        return
    endif
    Location beaconLoc = g_pendingBeacon.GetCurrentLocation()
    if beaconLoc != None && beaconLoc != g_outpostPlacementLocation && !CPOI_ClaimedPOIs.HasForm(beaconLoc)
        ; Compatibility projection only; the explicit beacon/POI pair is authoritative.
        CPOI_ClaimedPOIs.AddForm(beaconLoc)
    endif
    RegisterCassiopeiaWorkshopTelemetry()
    if g_pendingBeacon != None
        TryRenameOutpost(g_pendingBeacon, BuildClaimName(g_outpostPlacementLocation))
        CPOI_Native.LogReferenceState(g_pendingBeacon, 3)
        LogPopulationSnapshot(g_pendingBeacon, g_outpostPlacementLocation, 1)
        LogShipLandingContext("claim-complete", g_pendingBeacon)
    endif

    ; Refresh the companion perk after publishing claimed-location membership; no
    ; load, location-change, or pause-menu event will fire while the player
    ; stands here claiming.

    ; 5. Delete temporary helper and clear pending state.
    ObjectReference completedBeacon = g_pendingBeacon
    ObjectReference completedHelper = g_outpostOverlayHelper
    bool cleanupRequested = g_cleanupAfterClaim
    Actor completedPlayer = g_outpostPlacementPlayer
    Cell completedCell = g_outpostPlacementCell
    Location completedLocation = g_outpostPlacementLocation
    float completedRadius = g_outpostPlacementRadius
    bool startDeferredWorkshop = g_workshopStartDeferred && g_workshopDeferredBeacon == completedBeacon
    CreateClaimLandingAnchor(completionRequest, completedBeacon, g_pendingOriginalLandingMarker)
    ; Cosmetic cleanup is opt-in and occurs only after the claim commits.
    ; Cancellation or failed preparation no longer hides bodies.
    if cleanupRequested
        int disabled = CleanupSite(completedPlayer, completedCell, completedLocation, completedRadius, completionRequest)
        Debug.Notification("Claim complete. Cleared " + disabled + " eligible nearby bodies.")
    endif

    ; Cleanup can yield. An older completion must not clear loaded recovery state
    ; or delete a helper belonging to another request.
    if completionRequest != g_claimRequest || !g_nativeLoadReady || g_pendingBeacon != completedBeacon || g_outpostOverlayHelper != completedHelper
        return
    endif
    ClearPendingOutpostPlacement()
    if completedHelper != None
        completedHelper.Delete()
    endif

    if startDeferredWorkshop
        Debug.Trace("[CPOI] Starting workshop session deferred during native beacon placement", 0)
        BeginClaimedWorkshopSession(completedBeacon)
    endif

    Debug.Trace("[CPOI] Bound native scanner beacon to claimed POI: " + completedBeacon, 0)
    if CPOI_Msg_AssistedInstructions != None
        CPOI_Msg_AssistedInstructions.Show(0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0)
    endif
EndFunction

Function AttemptCancelPendingPlacement()
    if !g_nativeLoadReady
        return
    endif
    if g_placementRecoveryBusy
        StartTimer(2.0, 6)
        return
    endif
    g_placementRecoveryBusy = true
    CancelUnacceptedOutpostPlacement()
    g_placementRecoveryBusy = false
EndFunction

Function CancelUnacceptedOutpostPlacement()
    if !g_outpostPlacementPending
        return
    endif

    if g_outpostBeaconAccepted || g_pendingBeacon != None
        g_outpostBeaconAccepted = true
        g_cancelPending = false
        CancelTimer(5)
        Debug.Trace("[CPOI] Cancellation redirected to accepted-outpost completion recovery", 0)
        StartTimer(0.1, 6)
        return
    endif

    g_cancelPending = true
    CancelTimer(5)

    if g_outpostOverlayHelper != None
        if !g_overlayRestored
            if !g_overlayTrackingReady
                Debug.Trace("[CPOI] AttemptCancelPendingPlacement waiting for native overlay tracking", 0)
                StartTimer(2.0, 6)
                return
            endif
            bool restored = CPOI_Native.RestorePOIOverlay(g_outpostOverlayHelper)
            if restored
                g_overlayRestored = true
                g_overlayTrackingReady = false
                g_restoreRetryCount = 0
                Debug.Trace("[CPOI] POI overlay successfully restored on cancellation for helper: " + g_outpostOverlayHelper, 0)
            else
                g_restoreRetryCount += 1
                if g_restoreRetryCount <= 5
                    Debug.Trace("[CPOI] RestorePOIOverlay failed on cancellation; scheduling retry " + g_restoreRetryCount + "/5", 0)
                    StartTimer(2.0, 6)
                    return
                else
                    Debug.Trace("[CPOI] RestorePOIOverlay failed after max retries on cancellation; retaining helper anchor", 0)
                    return
                endif
            endif
        endif
    endif

    ; Restoration may yield to the placement event. Check acceptance again
    ; before releasing the anchor.
    if g_outpostBeaconAccepted || g_pendingBeacon != None
        g_outpostBeaconAccepted = true
        g_cancelPending = false
        StartTimer(0.1, 6)
        return
    endif
    if g_outpostOverlayHelper == None && !g_overlayRestored
        Debug.Trace("[CPOI] Cancellation blocked: missing helper and unverified overlay", 0)
        return
    endif

    ObjectReference helper = g_outpostOverlayHelper
    ClearPendingOutpostPlacement()
    if helper != None
        helper.Delete()
    endif
    Debug.Notification("Pending claim cancelled.")
    Debug.Trace("[CPOI] Pending placement cancelled and helper deleted without committing claim", 0)
EndFunction

Event Actor.OnOutpostPlaced(Actor akSender, ObjectReference akOutpostBeacon)
    if !g_nativeLoadReady
        return
    endif
    if !g_outpostPlacementPending
        return
    endif
    if akSender != g_outpostPlacementPlayer || akOutpostBeacon == None || g_outpostOverlayHelper == None
        Debug.Trace("[CPOI] Ignored unrelated OnOutpostPlaced event", 0)
        return
    endif

    Form vanillaBeaconForm = Game.GetFormFromFile(0x002C60E9, "Starfield.esm")
    if vanillaBeaconForm == None || akOutpostBeacon.GetBaseObject() != vanillaBeaconForm
        Debug.Trace("[CPOI] Ignored OnOutpostPlaced for a non-vanilla beacon: " + akOutpostBeacon, 0)
        return
    endif
    Location beaconLocation = akOutpostBeacon.GetCurrentLocation()
    float beaconDistance = akOutpostBeacon.GetDistance(g_outpostOverlayHelper)
    if g_outpostPlacementLocation == None || beaconLocation == None || akOutpostBeacon.GetWorldSpace() != g_outpostOverlayHelper.GetWorldSpace()
        Debug.Trace("[CPOI] Ignored OnOutpostPlaced for missing location or worldspace mismatch: pending=" + g_outpostPlacementLocation + " beacon=" + beaconLocation + " distance=" + beaconDistance, 0)
        return
    endif
    if beaconDistance > g_outpostPlacementRadius
        Debug.Trace("[CPOI] Ignored OnOutpostPlaced outside the pending claim radius: location=" + beaconLocation + " distance=" + beaconDistance + " radius=" + g_outpostPlacementRadius, 0)
        return
    endif
    if g_cassiopeiaVersion >= 96 && !CassiopeiaPapyrusExtender.IsOutpostBeacon(akOutpostBeacon)
        Debug.Trace("[CPOI] Ignored OnOutpostPlaced because Cassiopeia did not recognize a vanilla outpost beacon", 0)
        return
    endif

    ; Register at the earliest validated beacon boundary. If workshop entry is
    ; delivered before claim completion, OnWorkshopMode defers it safely.
    if !g_outpostPlacementPending || g_outpostBeaconAccepted
        return
    endif
    g_outpostBeaconAccepted = true
    g_pendingBeacon = akOutpostBeacon
    g_cancelPending = false
    if beaconLocation == g_outpostPlacementLocation
        ; Separate overlay creation does not imply a distinct current location.
        ; Record this identity without replaying the superseded trial's stop message.
        Debug.Trace("[CPOI] OverlayReturnAudit-v1: beacon shares POI location=" + beaconLocation + "; compare native overlay/reset state", 0)
    else
        Debug.Trace("[CPOI] VanillaLocationSeparation-v1: genuine beacon has distinct outpost location " + beaconLocation + " POI=" + g_outpostPlacementLocation + " distance=" + beaconDistance, 0)
    endif
    Self.RegisterForRemoteEvent(akOutpostBeacon, "OnWorkshopMode")
    CancelTimer(5)
    CancelTimer(11)
    int beaconDistInt = beaconDistance as int
    if beaconLocation != g_outpostPlacementLocation
        Debug.Trace("[CPOI] VanillaLocationSeparation-v1: accepted split beacon at " + beaconDistInt + "m; verify vanilla workshop radius covers the POI", 0)
        Debug.Notification("Separate outpost registered at " + beaconDistInt + "m. Return to the POI now.")
    else
        Debug.Notification("Outpost beacon registered. Check build-area coverage before editing.")
    endif
    CPOI_Native.LogReferenceState(akOutpostBeacon, 2)
    CPOI_Native.LogReferenceState(g_outpostOverlayHelper, 8)
    ProcessSuccessfulOutpostPlacement()

EndEvent

bool Function BeginNativeOutpostPlacement(Actor akPlayer, Cell akCell, Location akLocation, float afRadius, int aiRequest)
    if !ClaimPreflightIsCurrent(aiRequest, akPlayer, akCell, akLocation)
        return false
    endif
    LogShipLandingContext("pre-claim")
    ObjectReference originalLandingMarker = CaptureOriginalLandingMarker(akPlayer, afRadius)
    if !ClaimPreflightIsCurrent(aiRequest, akPlayer, akCell, akLocation)
        return false
    endif
    Form overlayActivatorForm = Game.GetFormFromFile(0x00260ED4, "Starfield.esm")
    if overlayActivatorForm == None
        Debug.Trace("[CPOI] Unable to resolve FC07OutpostActivator 0x00260ED4", 0)
        return false
    endif

    ObjectReference helper = akPlayer.PlaceAtMe(overlayActivatorForm, 1, true, false, false, None, None, false)
    if helper == None
        Debug.Trace("[CPOI] Failed to place temporary FC07OutpostActivator", 0)
        return false
    endif
    CPOI_Native.LogReferenceState(helper, 0)

    ; The location reset path is intentionally untouched: freezing the
    ; location blocks the procedural regeneration that restores the site
    ; after travel (runtime-rejected by the 2026-09-05 acceptance run).

    ; Capture the identity baseline before the first overlay mutation. This
    ; is diagnostic-only and never blocks the reversible claim transaction.
    LogPopulationSnapshot(helper, akLocation, 0)

    ; Revalidate after helper creation/diagnostics before the guarded bit-only mutation.
    if !ClaimPreflightIsCurrent(aiRequest, akPlayer, akCell, akLocation) || helper.GetParentCell() != akCell || helper.GetCurrentLocation() != akLocation
        helper.DisableNoWait(false)
        helper.Delete()
        return false
    endif
    int unlockResult = CPOI_Native.BeginPOIOverlayUnlock(helper)
    if unlockResult == 0
        Debug.Trace("[CPOI] BeginPOIOverlayUnlock failed for helper: " + helper, 0)
        helper.Disable(false)
        helper.Delete()
        return false
    endif

    ; Publish recovery state before calling another native on the helper.
    g_outpostPlacementPending = true
    g_outpostPlacementPlayer = akPlayer
    g_outpostPlacementCell = akCell
    g_outpostPlacementLocation = akLocation
    g_outpostOverlayHelper = helper
    g_outpostPlacementRadius = afRadius
    g_pendingBeacon = None
    g_pendingOriginalLandingMarker = originalLandingMarker
    g_outpostBeaconAccepted = false
    g_initialBindingVerified = false
    g_overlayRestored = false
    g_overlayTrackingReady = true
    g_cancelPending = false
    g_restoreRetryCount = 0
    helper.DisableNoWait(false)

    if unlockResult != 1
        g_cancelPending = true
        Debug.Trace("[CPOI] Begin unlock requires recovery; retaining helper", 0)
        Debug.Notification("POI preparation needs recovery. Do not place an outpost.")
        AttemptCancelPendingPlacement()
        return true
    endif

    ; Start 300-second placement timer and live distance ticker; do NOT finalize claim yet.
    StartTimer(300.0, 5)
    StartTimer(2.5, 11)
    Debug.Trace("[CPOI] POI overlay temporarily unlocked; waiting for native scanner placement around anchor: " + helper, 0)
    Debug.Notification("Claim prepared. Place a real beacon within range; check its build area covers the POI.")
    ; Coverage must be observed; a fixed perimeter distance is not a preservation guarantee.
    return true

EndFunction

bool Function ClaimPreflightIsCurrent(int aiRequest, Actor akPlayer, Cell akCell, Location akLocation)
    if aiRequest != g_claimRequest || !g_claimInProgress || g_outpostPlacementPending || !g_nativeLoadReady
        return false
    endif
    if akPlayer == None || akPlayer != Game.GetPlayer() || akCell == None || akLocation == None
        return false
    endif
    if akPlayer.GetParentCell() != akCell || akPlayer.GetCurrentLocation() != akLocation || akCell.IsInterior()
        return false
    endif
    if akPlayer.IsInCombat() || akPlayer.IsInSpaceEx(true) || IsSettledLocation(akLocation)
        return false
    endif
    if CPOI_ClaimedPOIs == None || CPOI_ClaimedBeacons == None || CPOI_ClaimedCount == None || CPOI_ClaimedPOIs.HasForm(akLocation)
        return false
    endif
    if CPOI_LocTypeClaimablePOI != None && akLocation.HasKeyword(CPOI_LocTypeClaimablePOI)
        return false
    endif
    return aiRequest == g_claimRequest && g_nativeLoadReady
EndFunction

Function EndClaimPreflight(int aiRequest)
    ; An old resumed stack cannot unlock or alter a newer request.
    if aiRequest == g_claimRequest && !g_outpostPlacementPending
        g_claimInProgress = false
        g_cleanupAfterClaim = false
        CancelTimer(2)
    endif
EndFunction

Function HandleClaimBeaconActivation(Actor akPlayer)
    if !g_nativeLoadReady
        Debug.Notification("Restart the game and load a pre-claim save before claiming.")
        return
    endif
    Debug.Trace("[CPOI] HandleClaimBeaconActivation entry", 0)

    if akPlayer == None
        Debug.Trace("[CPOI] HandleClaimBeaconActivation: akPlayer is None, aborting", 0)
        return
    endif

    if g_claimInProgress || g_outpostPlacementPending
        Debug.Notification("Claim operation already in progress...")
        return
    endif

    g_claimRequest += 1
    int request = g_claimRequest
    g_claimInProgress = true
    g_cleanupAfterClaim = false
    CancelTimer(2)

    ; Check 1: Player must be outdoors (exterior cell only)
    Cell currentCell = akPlayer.GetParentCell()
    Debug.Trace("[CPOI] Check 1: cell=" + currentCell, 0)
    if currentCell == None || currentCell.IsInterior()
        Debug.Notification("Cannot claim: You must be at an exterior POI.")
        EndClaimPreflight(request)
        return
    endif

    ; Check 2: Player must be in a designated location
    Location currentLoc = akPlayer.GetCurrentLocation()
    Debug.Trace("[CPOI] Check 2: loc=" + currentLoc, 0)
    if currentLoc == None
        Debug.Notification("Cannot claim: No designated POI location found here.")
        EndClaimPreflight(request)
        return
    endif

    ; Check 3: Site must not already be claimed
    bool alreadyClaimed = (CPOI_ClaimedPOIs != None && CPOI_ClaimedPOIs.HasForm(currentLoc))
    if !alreadyClaimed && CPOI_LocTypeClaimablePOI != None
        alreadyClaimed = currentLoc.HasKeyword(CPOI_LocTypeClaimablePOI)
    endif
    Debug.Trace("[CPOI] Check 3: alreadyClaimed=" + alreadyClaimed, 0)
    if alreadyClaimed
        Debug.Notification("This POI is already claimed.")
        EndClaimPreflight(request)
        return
    endif

    ; Check 3.5: Block claiming at friendly/settled/existing-outpost locations
    if IsSettledLocation(currentLoc)
        Debug.Notification("Cannot claim: This location is already settled.")
        EndClaimPreflight(request)
        return
    endif

    ; Check 4: Hostile enemy check (pure Papyrus — no native REL::Relocation)
    bool inCombat = akPlayer.IsInCombat()
    Debug.Trace("[CPOI] Check 4: inCombat=" + inCombat, 0)
    if inCombat
        Debug.Notification("Cannot claim: Hostiles remain nearby! Clear the area first.")
        EndClaimPreflight(request)
        return
    endif

    ; Check 5: General location eligibility
    bool canClaim = !akPlayer.IsInSpaceEx(true) && !currentCell.IsInterior()
    Debug.Trace("[CPOI] Check 5: canClaim=" + canClaim, 0)
    if !canClaim
        if CPOI_Msg_ClaimUnavailable != None
            CPOI_Msg_ClaimUnavailable.Show(0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0)
        else
            Debug.Notification("Location cannot be claimed right now.")
        endif
        EndClaimPreflight(request)
        return
    endif

    ; Check 6: Do not stack a second outpost beacon over an existing one
    Debug.Trace("[CPOI] Check 6: resolving OutpostBeaconActivator 0x002C60E9", 0)
    Form outpostBeaconForm = Game.GetFormFromFile(0x002C60E9, "Starfield.esm")
    Debug.Trace("[CPOI] Check 6: outpostBeaconForm=" + outpostBeaconForm, 0)
    float claimRadius = 200.0
    Debug.Trace("[CPOI] Check 6: CPOI_ClaimRadius=" + CPOI_ClaimRadius, 0)
    if CPOI_ClaimRadius != None && CPOI_ClaimRadius.GetValue() > 0.0
        claimRadius = CPOI_ClaimRadius.GetValue()
    endif
    Debug.Trace("[CPOI] Check 6: claimRadius=" + claimRadius, 0)
    Debug.Trace("[CPOI] Check 6: calling HasNearbyOutpostBeacon", 0)
    bool hasNearbyBeacon = HasNearbyOutpostBeacon(akPlayer, outpostBeaconForm, claimRadius)
    Debug.Trace("[CPOI] Check 6: HasNearbyOutpostBeacon returned=" + hasNearbyBeacon, 0)
    if hasNearbyBeacon
        Debug.Notification("Cannot claim: An outpost already exists nearby.")
        EndClaimPreflight(request)
        return
    endif

    ; Ask before mutating the claim or placing the outpost overlay.
    Debug.Trace("[CPOI] Cleanup prompt property=" + CPOI_Msg_ClearClutter, 0)
    if CPOI_Msg_ClearClutter != None
        Debug.Trace("[CPOI] Calling CPOI_Msg_ClearClutter.Show; waiting for choice", 0)
        int choice = CPOI_Msg_ClearClutter.Show(0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0)
        Debug.Trace("[CPOI] CPOI_Msg_ClearClutter.Show resumed with choice=" + choice, 0)
        if !ClaimPreflightIsCurrent(request, akPlayer, currentCell, currentLoc)
            EndClaimPreflight(request)
            return
        endif
        ; Store consent only. Cleanup cannot precede a successful claim.
        g_cleanupAfterClaim = choice == 0
    endif

    if !ClaimPreflightIsCurrent(request, akPlayer, currentCell, currentLoc) || HasNearbyOutpostBeacon(akPlayer, outpostBeaconForm, claimRadius)
        Debug.Notification("Claim preparation cancelled: the site or player state changed.")
        EndClaimPreflight(request)
        return
    endif

    ; Vanilla creates/registers the genuine beacon. The helper grants temporary
    ; permission; the native registration result must prove original-overlay reuse.
    if !BeginNativeOutpostPlacement(akPlayer, currentCell, currentLoc, claimRadius, request)
        Debug.Notification("Could not prepare this POI for native outpost placement.")
        CancelTimer(2)
        EndClaimPreflight(request)
        return
    endif

    Debug.Trace("[CPOI] HandleClaimBeaconActivation is awaiting native scanner placement", 0)
EndFunction

; Function callable from console or other scripts
Function AttemptClaim()
    HandleClaimBeaconActivation(Game.GetPlayer())
    StartBeaconRefundTimer()
EndFunction

; Deliberately fail closed. The temporary claim transaction now has a narrowly
; verified bit-clear restoration, but full claimed-outpost teardown and restoration
; of all ownership/bookkeeping state remain independently unverified.
Function ReleaseClaim(Location akLocation)
    if akLocation == None
        akLocation = Game.GetPlayer().GetCurrentLocation()
    endif

    if akLocation == None
        Debug.Notification("This site is not currently claimed.")
        return
    endif

    if CPOI_OverlayClaims != None && CPOI_OverlayClaims.HasForm(akLocation)
        Debug.Trace("[CPOI] Release blocked for persisted overlay claim: " + akLocation, 0)
        Debug.Notification("Release is unavailable: this POI has a persistent build overlay.")
    else
        Debug.Trace("[CPOI] Release blocked until overlay teardown is verified: " + akLocation, 0)
        Debug.Notification("POI release is unavailable while overlay teardown is unresolved.")
    endif
EndFunction

bool Function IsSettledLocation(Location akLocation)
    if akLocation == None
        return false
    endif
    Keyword settlementKW = Game.GetFormFromFile(0x00022611, "Starfield.esm") as Keyword
    if settlementKW != None && akLocation.HasKeyword(settlementKW)
        Debug.Trace("[CPOI] Check 3.5: LocTypeSettlement present", 0)
        return true
    endif
    Keyword outpostKW = Game.GetFormFromFile(0x000234F1, "Starfield.esm") as Keyword
    if outpostKW != None && akLocation.HasKeyword(outpostKW)
        Debug.Trace("[CPOI] Check 3.5: LocTypeOutpost present", 0)
        return true
    endif
    Keyword starstationKW = Game.GetFormFromFile(0x0001674D, "Starfield.esm") as Keyword
    if starstationKW != None && akLocation.HasKeyword(starstationKW)
        Debug.Trace("[CPOI] Check 3.5: LocTypeStarstation present", 0)
        return true
    endif
    Keyword playerHouseKW = Game.GetFormFromFile(0x002EF272, "Starfield.esm") as Keyword
    if playerHouseKW != None && akLocation.HasKeyword(playerHouseKW)
        Debug.Trace("[CPOI] Check 3.5: LocTypePlayerHouse present", 0)
        return true
    endif
    return false
EndFunction

bool Function HasNearbyOutpostBeacon(ObjectReference akCenter, Form akBeaconForm, float afRadius)
    if akCenter == None || akBeaconForm == None
        Debug.Trace("[CPOI] HasNearbyOutpostBeacon skipped: center=" + akCenter + " form=" + akBeaconForm, 0)
        return false
    endif
    Debug.Trace("[CPOI] HasNearbyOutpostBeacon: calling FindClosestReferenceOfTypeFromRef radius=" + afRadius, 0)
    ObjectReference existing = Game.FindClosestReferenceOfTypeFromRef(akBeaconForm, akCenter, afRadius)
    Debug.Trace("[CPOI] HasNearbyOutpostBeacon: query returned=" + existing, 0)
    if existing != None
        Debug.Trace("[CPOI] Check 6: nearby outpost beacon=" + existing, 0)
        return true
    endif
    return false
EndFunction

String Function BuildClaimName(Location akLocation)
    String poiName = ""
    if akLocation != None
        poiName = CPOI_Native.GetLocationName(akLocation)
        if poiName == ""
            poiName = akLocation.GetFormEditorID()
        endif
    endif
    if poiName == ""
        return "Claimed POI"
    endif
    return "Claimed " + poiName
EndFunction

Function TryRenameOutpost(ObjectReference akBeacon, String asName)
    if akBeacon == None || asName == ""
        return
    endif
    if g_cassiopeiaVersion >= 96 && CassiopeiaPapyrusExtender.RenameOutpost(akBeacon, asName)
        Debug.Trace("[CPOI] Renamed native outpost: " + asName, 0)
    else
        Debug.Trace("[CPOI] Native outpost rename unavailable; use the beacon menu to rename it", 0)
    endif
EndFunction

; User-initiated vanilla removal is first intercepted without mutation. This
; worker runs on the existing Papyrus/native lane, verifies ownership/detachment,
; and asks the same vanilla queue to retry. Only that one-use request may pass.
Function PollProtectedTeardown()
    if g_teardownWorkerRequest >= 0
        return
    endif
    int request = g_claimRequest
    g_teardownWorkerRequest = request
    int token = CPOI_Native.GetPendingInheritedTeardown()
    if token == 0
        ReleaseTeardownWorker(request)
        return
    endif
    if g_teardownToken == token
        int completion = CPOI_Native.GetInheritedTeardownCompletion(token)
        if completion > 0
            bool reconciled = ReconcileRemovedClaim(g_teardownBeacon, g_teardownClaimLocation, g_teardownBeaconLocation)
            if request != g_claimRequest
                ReleaseTeardownWorker(request)
                return
            endif
            if reconciled
                CPOI_Native.CancelInheritedTeardown(token)
                ClearTeardownState()
                RetireRemovedClaimLandingAnchors()
                Debug.Trace("[CPOI] InheritedTeardown-v4 vanilla removal confirmed and claim reconciled", 0)
                Debug.Notification("Outpost removed. Inherited site objects were preserved.")
            else
                CPOI_Native.CancelInheritedTeardown(token, true)
                g_nativeLoadReady = false
                Debug.Notification("Outpost removed, but claim bookkeeping needs recovery. Preserve logs.")
            endif
        else
            g_teardownPolls += 1
            if completion < 0 || g_teardownPolls >= 15
                CPOI_Native.CancelInheritedTeardown(token, true)
                g_nativeLoadReady = false
                Debug.Trace("[CPOI] InheritedTeardown-v4 removal completion unconfirmed; no deletion or enabling forced", 0)
                ; Retain the exact removal journal for inspection/load recovery.
                Debug.Notification("Outpost removal was not confirmed. Stop and preserve logs; no objects were force-deleted or restored.")
            endif
        endif
        ReleaseTeardownWorker(request)
        return
    endif
    int beaconID = CPOI_Native.GetInheritedTeardownBeacon(token)
    ObjectReference beacon = Game.GetForm(beaconID) as ObjectReference
    if request != g_claimRequest || !g_nativeLoadReady
        ReleaseTeardownWorker(request)
        return
    endif
    if beacon == None || beacon.IsDeleted() || g_outpostPlacementPending || g_claimPending || g_workshopSessionActive || g_workshopSessionHelper != None || g_workshopReplacementPending
        CPOI_Native.CancelInheritedTeardown(token)
        Debug.Notification("Removal paused. Finish placement or leave build mode before removing this outpost.")
        ReleaseTeardownWorker(request)
        return
    endif
    Location claimLocation = FindClaimedLocationForBeacon(beacon)
    if claimLocation == None || !CPOI_Native.HasInheritedProtection(beacon)
        CPOI_Native.CancelInheritedTeardown(token)
        Debug.Notification("Removal blocked: this claim has no verified inherited-object ledger. Keep your pre-removal save.")
        ReleaseTeardownWorker(request)
        return
    endif
    Location beaconLocation = beacon.GetCurrentLocation()
    ; Reserve serialized tracking before the native queue can complete.
    g_teardownToken = token
    g_teardownBeacon = beacon
    g_teardownClaimLocation = claimLocation
    g_teardownBeaconLocation = beaconLocation
    g_teardownPolls = 0
    int result = CPOI_Native.PrepareInheritedTeardown(beacon, token)
    if request != g_claimRequest || !g_nativeLoadReady
        ReleaseTeardownWorker(request)
        return
    endif
    Debug.Trace("[CPOI] InheritedTeardown-v4 prepare beacon=" + GetDiagnosticFormID(beacon) + " token=" + token + " result=" + result, 0)
    if result <= 0
        CPOI_Native.CancelInheritedTeardown(token)
        ClearTeardownState()
        if result == -1
            g_nativeLoadReady = false
            Debug.Notification("Inherited-object detachment failed. Removal remains blocked. Stop and preserve logs.")
        else
            Debug.Notification("Removal blocked: inherited-object protection is incomplete. Check the CPOI log.")
        endif
    endif
    ReleaseTeardownWorker(request)
EndFunction

Function ReleaseTeardownWorker(int aiRequest)
    if g_teardownWorkerRequest == aiRequest
        g_teardownWorkerRequest = -1
    endif
EndFunction

Function ClearTeardownState()
    g_teardownToken = 0
    g_teardownBeacon = None
    g_teardownClaimLocation = None
    g_teardownBeaconLocation = None
    g_teardownPolls = 0
EndFunction

bool Function ReconcileRemovedClaim(ObjectReference akBeacon, Location akClaimLocation, Location akBeaconLocation)
    if akBeacon == None || akBeacon != g_teardownBeacon || CPOI_Native.GetInheritedTeardownCompletion(g_teardownToken) != 1 || akClaimLocation == None || CPOI_ClaimedBeacons == None || CPOI_ClaimedPOIs == None || CPOI_ClaimedCount == None
        return false
    endif
    if g_splitClaimBeacons == None || g_splitClaimLocations == None || g_splitClaimBeacons.Length != g_splitClaimLocations.Length
        return false
    endif
    int i = g_splitClaimBeacons.Find(akBeacon)
    if i < 0 || g_splitClaimLocations[i] != akClaimLocation
        return false
    endif
    ; Only independently confirmed vanilla removal reaches this point. Restore
    ; before discarding the claim pair, so a failure retains recovery evidence.
    if !RestoreClaimResetProtection(akClaimLocation)
        return false
    endif
    ; Persist exact owned-anchor retirement authority before clearing the pair.
    if g_claimLandingAnchors != None
        int anchorIndex = 0
        while anchorIndex < g_claimLandingAnchors.Length
            if g_claimLandingAnchors[anchorIndex].beacon == akBeacon
                g_claimLandingAnchors[anchorIndex].removalConfirmed = true
            endif
            anchorIndex += 1
        endwhile
    endif
    CPOI_ClaimedBeacons.RemoveAddedForm(akBeacon)
    if CPOI_ClaimedBeacons.HasForm(akBeacon)
        return false
    endif
    g_splitClaimBeacons.Remove(i)
    g_splitClaimLocations.Remove(i)
    if FindTrackedBeaconFor(akClaimLocation) == None
        CPOI_ClaimedPOIs.RemoveAddedForm(akClaimLocation)
    endif
    if akBeaconLocation != None && akBeaconLocation != akClaimLocation && FindTrackedBeaconFor(akBeaconLocation) == None
        CPOI_ClaimedPOIs.RemoveAddedForm(akBeaconLocation)
    endif
    FormList protectedBeacons = Game.GetFormFromFile(0x00000811, "ClaimablePOIs.esm") as FormList
    if protectedBeacons != None
        protectedBeacons.RemoveAddedForm(akBeacon)
    endif
    CPOI_ClaimedCount.SetValue(CPOI_ClaimedBeacons.GetSize() as float)
    Debug.Trace("[CPOI] InheritedTeardown-v4 reconciled beacon=" + GetDiagnosticFormID(akBeacon) + " remaining=" + CPOI_ClaimedBeacons.GetSize(), 0)
    return true
EndFunction

; --- Claimed POI Workshop Session Overlay Lifecycle ---

Event ObjectReference.OnWorkshopMode(ObjectReference akSender, Bool aStart)
    if !g_nativeLoadReady
        return
    endif
    Debug.Trace("[CPOI] OnWorkshopMode received: sender=" + akSender + " aStart=" + aStart, 0)

    if akSender == None || akSender.IsDeleted()
        Debug.Trace("[CPOI] OnWorkshopMode rejected: sender is None or deleted", 0)
        return
    endif

    Form vanillaBeaconForm = Game.GetFormFromFile(0x002C60E9, "Starfield.esm")
    if vanillaBeaconForm == None || akSender.GetBaseObject() != vanillaBeaconForm
        Debug.Trace("[CPOI] OnWorkshopMode rejected: sender is not a vanilla beacon base 0x002C60E9", 0)
        return
    endif

    ; Initial placement and this remote event may be queued close together. Do
    ; not compete for the native tracker: remember the validated pending beacon
    ; and start its workshop session only after claim restoration/finalization.
    if g_outpostPlacementPending && akSender == g_pendingBeacon
        if aStart
            g_workshopStartDeferred = true
            g_workshopDeferredBeacon = akSender
            Debug.Trace("[CPOI] OnWorkshopMode: deferred initial workshop start until claim transaction completes", 0)
        elseif g_workshopDeferredBeacon == akSender
            g_workshopStartDeferred = false
            g_workshopDeferredBeacon = None
            Debug.Trace("[CPOI] OnWorkshopMode: cleared deferred initial workshop start", 0)
        endif
        return
    endif

    if aStart
        ; WorkshopTiming-v1: session CONT reattachment is disabled, so its old
        ; teardown cooldown must not delay the required overlay permission.
        ; Dispatch from this event without a suspended entry waiting to resume.
        Debug.Trace("[CPOI] WorkshopTiming-v1: starting overlay session without enrollment cooldown", 0)
        BeginClaimedWorkshopSession(akSender)
    else
        if g_workshopReplacementPending && akSender == g_workshopReplacementBeacon
            RollbackPendingWorkshopReplacement("workshop exit")
        endif
        if !g_workshopSessionActive
            Debug.Trace("[CPOI] OnWorkshopMode: stop received with no active workshop session", 0)
            return
        endif

        if g_workshopSessionBeacon != akSender
            Debug.Trace("[CPOI] OnWorkshopMode: stop received for non-active sender; ignored", 0)
            return
        endif

        RestoreWorkshopOverlaySession()
    endif
EndEvent

Event ObjectReference.OnWorkshopObjectPlaced(ObjectReference akSender, ObjectReference akReference)
    if !g_nativeLoadReady
        return
    endif
    if !g_workshopReplacementPending || !g_workshopReplacementListening
        return
    endif
    if akSender != g_workshopReplacementBeacon
        return
    endif
    if akReference == None || akReference.IsDeleted() || akReference == g_workshopReplacementOriginal
        return
    endif
    if akReference.GetBaseObject() != g_workshopReplacementBase
        Debug.Trace("[CPOI] Workshop replacement ignored unrelated base ref=" + GetDiagnosticFormID(akReference), 0)
        return
    endif

    Utility.Wait(0.25)
    if !g_workshopReplacementPending
        return
    endif
    string candidateID = GetDiagnosticFormID(akReference)
    float deltaX = akReference.GetPositionX() - g_workshopReplacementX
    float deltaY = akReference.GetPositionY() - g_workshopReplacementY
    float deltaZ = akReference.GetPositionZ() - g_workshopReplacementZ
    float candidateDistance = Math.sqrt((deltaX * deltaX) + (deltaY * deltaY) + (deltaZ * deltaZ))
    float replacementRadius = 200.0
    if CPOI_ClaimRadius != None && CPOI_ClaimRadius.GetValue() > 0.0
        replacementRadius = CPOI_ClaimRadius.GetValue()
    endif
    if akReference.IsDeleted() || akReference.IsDisabled() || !akReference.Is3DLoaded() || akReference.GetCurrentLocation() != g_workshopReplacementLocation || akReference.GetParentCell() != g_workshopReplacementCell || candidateDistance > replacementRadius || akReference.GetWorkshop() != g_workshopReplacementBeacon || !CassiopeiaPapyrusExtender.IsWorkshopItem(akReference)
        Debug.Trace("[CPOI] Workshop replacement candidate rejected ref=" + candidateID + " workshop=" + GetDiagnosticFormID(akReference.GetWorkshop()) + " isWorkshopItem=" + CassiopeiaPapyrusExtender.IsWorkshopItem(akReference) + " distance=" + candidateDistance, 0)
        RollbackPendingWorkshopReplacement("invalid replacement candidate")
        return
    endif

    bool nativeLogged = CPOI_Native.LogReferenceState(akReference, 14)
    Debug.Trace("[CPOI] Workshop replacement stage=14 candidate=" + candidateID + " nativeLog=" + nativeLogged, 0)
    if !nativeLogged || !WorkshopReplacementLedgerIsValid()
        Debug.Trace("[CPOI] Workshop replacement candidate rejected: snapshot or ledger validation failed", 0)
        RollbackPendingWorkshopReplacement("candidate validation failed")
        return
    endif

    ObjectReference original = g_workshopReplacementOriginal
    int originalCount = CPOI_ReplacedOriginals.GetSize()
    int replacementCount = CPOI_WorkshopReplacements.GetSize()
    if CPOI_ReplacedOriginals.HasForm(original) || CPOI_ReplacedOriginals.HasForm(akReference) || CPOI_WorkshopReplacements.HasForm(original) || CPOI_WorkshopReplacements.HasForm(akReference)
        Debug.Trace("[CPOI] Workshop replacement candidate rejected: ledger duplicate candidate=" + candidateID, 0)
        RollbackPendingWorkshopReplacement("ledger duplicate")
        return
    endif

    g_workshopReplacementCandidate = akReference
    g_workshopReplacementCandidateID = candidateID
    CPOI_ReplacedOriginals.AddForm(original)
    if !CPOI_ReplacedOriginals.HasForm(original) || CPOI_ReplacedOriginals.GetSize() != originalCount + 1
        RemovePartialWorkshopReplacementLedger(original, akReference)
        RollbackPendingWorkshopReplacement("original ledger append failed")
        return
    endif
    CPOI_WorkshopReplacements.AddForm(akReference)
    if !CPOI_WorkshopReplacements.HasForm(akReference) || CPOI_WorkshopReplacements.GetSize() != replacementCount + 1 || CPOI_ReplacedOriginals.GetSize() != CPOI_WorkshopReplacements.GetSize() || CPOI_ReplacedOriginals.GetAt(originalCount) != original || CPOI_WorkshopReplacements.GetAt(replacementCount) != akReference || !WorkshopReplacementLedgerIsValid()
        RemovePartialWorkshopReplacementLedger(original, akReference)
        RollbackPendingWorkshopReplacement("replacement ledger append failed")
        return
    endif

    UnregisterWorkshopReplacementListener()
    CancelTimer(9)
    Debug.Trace("[CPOI] Workshop replacement adopted original=" + GetDiagnosticFormID(original) + " replacement=" + candidateID, 0)
    ClearPendingWorkshopReplacement()
    Debug.Notification("Workshop replacement adopted.")
EndEvent

Function BeginClaimedWorkshopSession(ObjectReference akBeacon)
    if !g_nativeLoadReady
        return
    endif
    if akBeacon == None || akBeacon.IsDeleted()
        Debug.Trace("[CPOI] BeginClaimedWorkshopSession rejected missing beacon", 0)
        return
    endif
    if CPOI_ClaimedBeacons == None || !CPOI_ClaimedBeacons.HasForm(akBeacon)
        Debug.Trace("[CPOI] BeginClaimedWorkshopSession rejected untracked beacon", 0)
        return
    endif

    Location claimLoc = FindClaimedLocationForBeacon(akBeacon)
    if claimLoc == None
        Debug.Trace("[CPOI] BeginClaimedWorkshopSession rejected beacon without a verified claimed POI association", 0)
        return
    endif
    Actor player = Game.GetPlayer()
    float workshopClaimRadius = 200.0
    if CPOI_ClaimRadius != None && CPOI_ClaimRadius.GetValue() > 0.0
        workshopClaimRadius = CPOI_ClaimRadius.GetValue()
    endif
    if player == None || player.GetCurrentLocation() != claimLoc || player.GetWorldSpace() != akBeacon.GetWorldSpace() || player.GetDistance(akBeacon) > workshopClaimRadius
        Debug.Trace("[CPOI] BeginClaimedWorkshopSession rejected player location mismatch", 0)
        return
    endif
    if g_outpostPlacementPending
        Debug.Trace("[CPOI] BeginClaimedWorkshopSession rejected active initial placement transaction", 0)
        return
    endif

    if g_workshopSessionActive || g_workshopSessionHelper != None
        if g_workshopSessionBeacon == akBeacon
            Debug.Trace("[CPOI] BeginClaimedWorkshopSession duplicate start is harmless", 0)
        else
            Debug.Trace("[CPOI] BeginClaimedWorkshopSession rejected conflicting active beacon", 0)
        endif
        return
    endif

    Form overlayActivatorForm = Game.GetFormFromFile(0x00260ED4, "Starfield.esm")
    if overlayActivatorForm == None
        Debug.Trace("[CPOI] BeginClaimedWorkshopSession unable to resolve FC07OutpostActivator 0x00260ED4", 0)
        return
    endif

    ; A split beacon is anchored in the adjacent vanilla outpost location.
    ; Unlock the POI surface property under the player, not the beacon's cell.
    ObjectReference helper = None
    if akBeacon.GetCurrentLocation() != claimLoc
        helper = player.PlaceAtMe(overlayActivatorForm, 1, true, false, false, None, None, false)
    else
        helper = akBeacon.PlaceAtMe(overlayActivatorForm, 1, true, false, false, None, None, false)
    endif
    if helper == None
        Debug.Trace("[CPOI] BeginClaimedWorkshopSession failed to place FC07 helper", 0)
        return
    endif

    if helper.GetCurrentLocation() != claimLoc || helper.GetWorldSpace() != player.GetWorldSpace()
        Debug.Trace("[CPOI] BeginClaimedWorkshopSession rejected helper outside claimed POI location", 0)
        helper.Disable(false)
        helper.Delete()
        return
    endif

    LogPopulationSnapshot(akBeacon, claimLoc, 2)

    int unlockResult = CPOI_Native.BeginClaimedPOIWorkshopUnlock(helper)
    if unlockResult == 0
        Debug.Trace("[CPOI] BeginClaimedPOIWorkshopUnlock failed for helper: " + helper, 0)
        helper.Disable(false)
        helper.Delete()
        return
    endif

    g_workshopSessionActive = unlockResult == 1
    g_workshopSessionBeacon = akBeacon
    g_workshopSessionHelper = helper
    g_workshopRestoreRetryCount = 0
    helper.DisableNoWait(false)
    if unlockResult != 1
        Debug.Trace("[CPOI] Workshop begin requires recovery; retaining helper and skipping enrollment", 0)
        AttemptRecoverWorkshopSession()
        return
    endif
    Debug.Trace("[CPOI] Claimed POI workshop session active with helper: " + helper, 0)

    ; Claim-time enrollment remains bypassed. Both type-separated workshop
    ; calls require the verified live session and preserve native exclusions.
    ; The native enrollment contract requires a beacon in the same location.
    ; Keep the verified POI overlay session, but never call enrollment for a split beacon.
    if akBeacon.GetCurrentLocation() != claimLoc
        Debug.Trace("[CPOI] SplitLocationEnrollment-v1: unsupported beacon location; enrollmentCalled=false", 0)
        Debug.Notification("This beacon is outside the POI location. Inherited object enrollment is unavailable.")
        return ; Normal workshop exit still restores the overlay.
    endif
    ; FurnitureOnEntry-v1: enroll on the first verified workshop entry after claim.
    ; Selection may still require a later cell reload; native safety is unchanged.
    if !CPOI_Native.HasInheritedProtection(akBeacon)
        Debug.Trace("[CPOI] InheritedTeardown-v4 legacy claim: new enrollment and removal blocked; no ownership migration", 0)
        Debug.Notification("This older claim has no inherited-object protection ledger. Use a pre-claim save for teardown tests.")
        return
    endif
    Cell[] furnitureCells = CassiopeiaPapyrusExtender.GetLoadedCells()
    int containersLinked = CPOI_Native.ReattachAllWorkshopItems(akBeacon, akBeacon.GetParentCell(), furnitureCells, claimLoc, workshopClaimRadius, CPOI_ReplacedOriginals, CPOI_WorkshopReplacements, false)
    Debug.Trace("[CPOI] CompoundCONT-v2: enrollmentCalled=true beacon=" + GetDiagnosticFormID(akBeacon) + " newlyLinked=" + containersLinked, 0)
    if containersLinked < 0
        Debug.Notification("Container enrollment could not be verified. Stop trial and preserve logs.")
        RestoreWorkshopOverlaySession()
        return
    endif
    int furnitureLinked = CPOI_Native.ReattachAllWorkshopItems(akBeacon, akBeacon.GetParentCell(), furnitureCells, claimLoc, workshopClaimRadius, CPOI_ReplacedOriginals, CPOI_WorkshopReplacements, true)
    Debug.Trace("[CPOI] FurnitureOnEntry-v1: enrollmentCalled=true beacon=" + GetDiagnosticFormID(akBeacon) + " linked=" + furnitureLinked, 0)
    if furnitureLinked < 0
        Debug.Notification("Furniture enrollment could not be verified. Stop trial and preserve logs.")
        RestoreWorkshopOverlaySession()
        return
    endif
    ; Routine counts remain in logs; repeated build entry needs no popup.
    LogPopulationSnapshot(akBeacon, claimLoc, 3)
EndFunction

Function RestoreWorkshopOverlaySession()
    if !g_nativeLoadReady
        return
    endif
    if !g_workshopSessionActive || g_workshopSessionHelper == None
        return
    endif

    ObjectReference diagnosticBeacon = g_workshopSessionBeacon
    Location diagnosticLocation = None
    if diagnosticBeacon != None
        diagnosticLocation = FindClaimedLocationForBeacon(diagnosticBeacon)
        LogPopulationSnapshot(diagnosticBeacon, diagnosticLocation, 4)
    endif

    int detachedFurniture = CPOI_Native.DetachSessionWorkshopFurniture(g_workshopSessionBeacon)
    if detachedFurniture < 0
        g_workshopRestoreRetryCount += 1
        if g_workshopRestoreRetryCount <= 5
            Debug.Trace("[CPOI] Session FURN detach failed; scheduling retry " + g_workshopRestoreRetryCount + "/5", 0)
            StartTimer(2.0, 7)
        else
            Debug.Trace("[CPOI] Session FURN detach failed after max retries; retaining workshop helper", 0)
        endif
        return
    endif
    Debug.Trace("[CPOI] Detached session-only inherited FURN links: " + detachedFurniture, 0)

    bool restored = CPOI_Native.RestorePOIOverlay(g_workshopSessionHelper)
    if !restored
        ; Pre-load clears volatile native pointers before Papyrus resumes. If a
        ; load fails or an exit event arrives afterward, re-resolve the helper's
        ; surface property and verify the safe clear state before cleanup.
        restored = CPOI_Native.RecoverClaimedPOIWorkshopUnlock(g_workshopSessionHelper)
        Debug.Trace("[CPOI] Workshop restore fallback recovery returned " + restored, 0)
    endif
    if restored
        if diagnosticBeacon != None && diagnosticLocation != None
            LogPopulationSnapshot(diagnosticBeacon, diagnosticLocation, 5)
        endif
        ObjectReference helper = g_workshopSessionHelper
        g_workshopSessionActive = false
        g_workshopSessionBeacon = None
        g_workshopSessionHelper = None
        g_workshopRestoreRetryCount = 0
        CancelTimer(7)
        if helper != None
            helper.Delete()
        endif
        Debug.Trace("[CPOI] Claimed POI workshop overlay restored and helper deleted", 0)
    else
        g_workshopRestoreRetryCount += 1
        if g_workshopRestoreRetryCount <= 5
            Debug.Trace("[CPOI] RestorePOIOverlay failed for workshop session; scheduling retry " + g_workshopRestoreRetryCount + "/5", 0)
            StartTimer(2.0, 7)
        else
            Debug.Trace("[CPOI] RestorePOIOverlay failed after max retries for workshop session; retaining helper for recovery", 0)
        endif
    endif
EndFunction

Function AttemptRecoverWorkshopSession()
    if !g_nativeLoadReady
        return
    endif
    if g_workshopSessionHelper == None
        g_workshopSessionActive = false
        g_workshopSessionBeacon = None
        g_workshopRestoreRetryCount = 0
        CancelTimer(8)
        return
    endif

    bool recovered = CPOI_Native.RecoverClaimedPOIWorkshopUnlock(g_workshopSessionHelper)
    Debug.Trace("[CPOI] AttemptRecoverWorkshopSession: RecoverClaimedPOIWorkshopUnlock returned " + recovered, 0)
    if recovered
        ObjectReference helper = g_workshopSessionHelper
        g_workshopSessionActive = false
        g_workshopSessionBeacon = None
        g_workshopSessionHelper = None
        g_workshopRestoreRetryCount = 0
        CancelTimer(8)
        if helper != None
            helper.Delete()
        endif
        Debug.Trace("[CPOI] Claimed POI workshop session successfully recovered and helper deleted on load", 0)
    else
        g_workshopRestoreRetryCount += 1
        if g_workshopRestoreRetryCount <= 5
            Debug.Trace("[CPOI] RecoverClaimedPOIWorkshopUnlock failed on load; scheduling retry " + g_workshopRestoreRetryCount + "/5", 0)
            StartTimer(2.0, 8)
        else
            Debug.Trace("[CPOI] RecoverClaimedPOIWorkshopUnlock failed after max retries on load", 0)
        endif
    endif
EndFunction
