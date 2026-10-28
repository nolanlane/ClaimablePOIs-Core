ScriptName CPOI_ClaimBeaconEffect Extends ActiveMagicEffect

Quest Property CPOI_ClaimQuest Auto Const mandatory

Event OnEffectStart(ObjectReference akTarget, Actor akCaster, MagicEffect akBaseEffect, Float afMagnitude, Float afDuration)
    Debug.Trace("[CPOI] Claim Beacon effect started", 0)
    if akCaster != None && CPOI_ClaimQuest != None
        Debug.Trace("[CPOI] Queueing claim until InventoryMenu closes...", 0)
        (CPOI_ClaimQuest as CPOI_ClaimQuestScript).QueueClaimBeaconActivation(akCaster)
    else
        Debug.Trace("[CPOI] Effect skipped: akCaster=" + akCaster + " quest=" + CPOI_ClaimQuest, 0)
    endif
EndEvent
