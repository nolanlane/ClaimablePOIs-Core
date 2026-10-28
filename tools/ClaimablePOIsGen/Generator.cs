using System.IO;
using System.Security.Cryptography;
using System.Text.Json;
using Mutagen.Bethesda;
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Plugins.Binary.Headers;
using Mutagen.Bethesda.Plugins.Binary.Parameters;
using Mutagen.Bethesda.Plugins.Masters;
using Mutagen.Bethesda.Plugins.Records;
using Mutagen.Bethesda.Starfield;
using Mutagen.Bethesda.Strings;
using Noggog;

namespace ClaimablePOIsGen;

public static class Generator
{
    public const string PluginName = "ClaimablePOIs.esm";
    public const string SchemaVersion = "claimable-pois.mutagen-result.v1";

    // Required EditorIDs — must match the DLL's Forms::Resolve expectations.
    public const string TransponderEditorId = "CPOI_ClaimTransponder";
    public const string ClaimUnavailableEditorId = "CPOI_Msg_ClaimUnavailable";
    public const string AssistedInstructionsEditorId = "CPOI_Msg_AssistedInstructions";
    public const string ClearClutterEditorId = "CPOI_Msg_ClearClutter";
    public const string PlacementInstructionsEditorId = "CPOI_Msg_PlacementInstructions";

    // Phase 1 records — persistence layer for Papyrus native functions.
    public const string ClaimQuestEditorId = "CPOI_ClaimQuest";
    public const string ClaimableKeywordEditorId = "CPOI_LocTypeClaimablePOI";
    public const string ClaimedPOIsEditorId = "CPOI_ClaimedPOIs";
    public const string ClaimedBeaconsEditorId = "CPOI_ClaimedBeacons";
    public const string OverlayClaimsEditorId = "CPOI_OverlayClaims";
    public const string ReplacedOriginalsEditorId = "CPOI_ReplacedOriginals";
    public const string WorkshopReplacementsEditorId = "CPOI_WorkshopReplacements";
    public const string ClaimLandingAnchorLinkEditorId = "CPOI_ClaimLandingAnchorLink";
    public const string InheritedItemsEditorId = "CPOI_InheritedItems";
    public const string ProtectedBeaconsEditorId = "CPOI_ProtectedBeacons";
    public const string ClaimedCountEditorId = "CPOI_ClaimedCount";
    public const string ClaimRadiusEditorId = "CPOI_ClaimRadius";
    // Starfield.esm PlayerFaction — verified via Mutagen overlay search.
    public const uint PlayerFactionFormId = 0x0001C21C;

    public static async Task<ToolResult> GenerateAsync(string outputDir, bool includePhase1 = true, bool questOnly = false)
    {
        try
        {
            Directory.CreateDirectory(outputDir);

            var outputPath = Path.Combine(outputDir, PluginName);
            if (File.Exists(outputPath))
            {
                return Blocked("OUTPUT_ALREADY_EXISTS",
                    $"The output directory already contains {PluginName}.", outputDir);
            }

            var modKey = ModKey.FromFileName(PluginName);
            var mod = new StarfieldMod(modKey, StarfieldRelease.Starfield);
            mod.ModHeader.Flags |= StarfieldModHeader.HeaderFlag.Master;
            mod.ModHeader.MasterReferences.Add(new Mutagen.Bethesda.Plugins.Records.MasterReference
            {
                Master = ModKey.FromNameAndExtension("Starfield.esm")
            });

            // --- CPOI_ClaimTransponder (Ingestible / Aid Item) ---
            // Created first so it gets local FormID 0x800, matching the C++ DLL's
            // hardcoded kLocalTransponder. The effect is attached after the
            // custom MagicEffect is created (below) to preserve FormID order.
            var transponder = new Ingestible(mod.GetNextFormKey(), StarfieldRelease.Starfield);
            transponder.EditorID = TransponderEditorId;
            transponder.Name = "POI Claim Beacon";
            transponder.Weight = 0.0f;
            transponder.Value = 0;
            mod.Ingestibles.Add(transponder);

            // --- CPOI_Msg_ClaimUnavailable (Message) ---
            var claimUnavailable = mod.Messages.AddNew(ClaimUnavailableEditorId);
            claimUnavailable.Name = "Claim Unavailable";
            claimUnavailable.Description =
                "This location cannot be claimed right now. Ensure the area is cleared of hostiles, "
                + "you are outdoors, and no existing outpost overlaps this site.";

            // --- CPOI_Msg_AssistedInstructions (Message) ---
            var assistedInstructions = mod.Messages.AddNew(AssistedInstructionsEditorId);
            assistedInstructions.Name = "POI Claimed";
            assistedInstructions.Description = "POI claimed. Outpost beacon registered.";
            assistedInstructions.Flags = (Message.Flag)0;
            assistedInstructions.DisplayTime = 5;

            // --- Phase 1 records (claim interface and bookkeeping) ---
            // The DLL does not resolve these yet; they are staged for the Papyrus claim
            // flow. Skipped in --minimal mode for the crash-isolation test.
            if (includePhase1)
            {
                // --- Phase 1 records (claim interface and bookkeeping) ---
                // --- CPOI_LocTypeClaimablePOI (Keyword) ---
                var claimableKeyword = mod.Keywords.AddNew(ClaimableKeywordEditorId);
                claimableKeyword.Name = "Claimable POI Location";

                // --- CPOI_ClaimedPOIs (FormList) ---
                var claimedPOIs = mod.FormLists.AddNew(ClaimedPOIsEditorId);

                // --- CPOI_ClaimedBeacons (FormList of placed outpost beacon refs) ---
                var claimedBeacons = mod.FormLists.AddNew(ClaimedBeaconsEditorId);

                // --- CPOI_ClaimedCount (Global) ---
                var claimedCount = mod.Globals.AddNew(ClaimedCountEditorId);
                claimedCount.Data = 0f;

                // --- CPOI_ClaimRadius (Global) ---
                var claimRadius = mod.Globals.AddNew(ClaimRadiusEditorId);
                claimRadius.Data = 200.0f;

                // --- CPOI_ClaimQuest (Quest) ---
                var claimQuest = mod.Quests.AddNew(ClaimQuestEditorId);
                claimQuest.Name = "POI Claim";
                claimQuest.Data = new QuestData();
                var priorityProp = claimQuest.Data.GetType().GetProperty("Priority");
                priorityProp!.SetValue(claimQuest.Data, (byte)0);
                // Set StartGameEnabled + RunOnce flags via reflection to avoid compile-time enum name issues
                var flagsProp = claimQuest.Data.GetType().GetProperty("Flags");
                if (flagsProp != null)
                {
                    var flagsType = flagsProp.PropertyType;
                    var allFlags = System.Enum.GetValues(flagsType);
                    object combinedFlags = System.Activator.CreateInstance(flagsType)!;
                    foreach (var f in allFlags)
                    {
                        var name = f.ToString();
                        if (name == "StartGameEnabled" || name == "RunOnce")
                        {
                            combinedFlags = (int)combinedFlags | (int)f;
                        }
                    }
                    flagsProp.SetValue(claimQuest.Data, combinedFlags);
                }

                var adapter = new QuestAdapter
                {
                    Version = 6,
                    ObjectFormat = 2
                };
                var questScript = new ScriptEntry
                {
                    Name = "CPOI_ClaimQuestScript"
                };
                questScript.Properties.Add(new ScriptObjectProperty
                {
                    Name = "CPOI_ClaimedCount",
                    Object = claimedCount.ToLink()
                });
                questScript.Properties.Add(new ScriptObjectProperty
                {
                    Name = "CPOI_ClaimRadius",
                    Object = claimRadius.ToLink()
                });
                questScript.Properties.Add(new ScriptObjectProperty
                {
                    Name = "CPOI_ClaimedPOIs",
                    Object = claimedPOIs.ToLink()
                });
                questScript.Properties.Add(new ScriptObjectProperty
                {
                    Name = "CPOI_ClaimedBeacons",
                    Object = claimedBeacons.ToLink()
                });
                questScript.Properties.Add(new ScriptObjectProperty
                {
                    Name = "CPOI_LocTypeClaimablePOI",
                    Object = claimableKeyword.ToLink()
                });
                questScript.Properties.Add(new ScriptObjectProperty
                {
                    Name = "CPOI_Msg_ClaimUnavailable",
                    Object = claimUnavailable.ToLink()
                });
                questScript.Properties.Add(new ScriptObjectProperty
                {
                    Name = "CPOI_Msg_AssistedInstructions",
                    Object = assistedInstructions.ToLink()
                });
                questScript.Properties.Add(new ScriptObjectProperty
                {
                    Name = "CPOI_ClaimTransponder",
                    Object = transponder.ToLink()
                });
                questScript.Properties.Add(new ScriptObjectProperty
                {
                    Name = "PlayerFaction",
                    Object = new FormKey(ModKey.FromNameAndExtension("Starfield.esm"), PlayerFactionFormId).ToLink<IFactionGetter>()
                });

                var clearClutter = mod.Messages.AddNew(ClearClutterEditorId);
                clearClutter.Name = "Clear Site Bodies";
                clearClutter.Description =
                    "After this POI is successfully claimed, permanently hide eligible nearby dead bodies? Quest-linked bodies and other clutter will remain. Cancelled or failed claims leave bodies unchanged.";
                clearClutter.Flags = Message.Flag.MessageBox;
                clearClutter.MenuButtons.Add(new MessageButton { Text = "Clear Dead Bodies" });
                clearClutter.MenuButtons.Add(new MessageButton { Text = "Leave All" });
                questScript.Properties.Add(new ScriptObjectProperty
                {
                    Name = "CPOI_Msg_ClearClutter",
                    Object = clearClutter.ToLink()
                });

                adapter.Scripts.Add(questScript);
                claimQuest.VirtualMachineAdapter = adapter;

                // --- CPOI_ClaimBeaconEffect (MagicEffect — Script archetype) ---
                // Created after the quest so it gets local FormID 0x808.
                // Uses Script archetype (like vanilla dummy effects) to fire a
                // Papyrus script automatically when the Ingestible is consumed,
                // eliminating the need for RegisterForRemoteEvent which is lost
                // across save loads.
                var claimEffect = new MagicEffect(mod.GetNextFormKey(), StarfieldRelease.Starfield);
                claimEffect.EditorID = "CPOI_ClaimBeaconEffect";
                claimEffect.Name = "";
                claimEffect.Description = "";
                claimEffect.Archetype = new MagicEffectArchetype
                {
                    Type = MagicEffectArchetype.TypeEnum.Script
                };
                claimEffect.Flags = MagicEffect.Flag.NoMagnitude
                    | MagicEffect.Flag.NoDuration
                    | MagicEffect.Flag.NoArea
                    | MagicEffect.Flag.HideInUI
                    | MagicEffect.Flag.Recover;
                claimEffect.DATADataTypeState = MagicEffect.DATADataType.Break0;
                claimEffect.CastType = CastType.FireAndForget;
                claimEffect.TargetType = TargetType.Self;

                var mgefAdapter = new VirtualMachineAdapter
                {
                    Version = 6,
                    ObjectFormat = 2
                };
                var mgefScript = new ScriptEntry
                {
                    Name = "CPOI_ClaimBeaconEffect"
                };
                mgefScript.Properties.Add(new ScriptObjectProperty
                {
                    Name = "CPOI_ClaimQuest",
                    Object = claimQuest.ToLink()
                });
                mgefAdapter.Scripts.Add(mgefScript);
                claimEffect.VirtualMachineAdapter = mgefAdapter;
                mod.MagicEffects.Add(claimEffect);

                // Append after existing records so established local FormIDs,
                // including the native DLL's resolved forms, remain unchanged.
                var overlayClaims = mod.FormLists.AddNew(OverlayClaimsEditorId);
                questScript.Properties.Add(new ScriptObjectProperty
                {
                    Name = "CPOI_OverlayClaims",
                    Object = overlayClaims.ToLink()
                });

                var placementInstructions = mod.Messages.AddNew(PlacementInstructionsEditorId);
                placementInstructions.Name = "Build Area Unlocked";
                placementInstructions.Description =
                    "Build area unlocked. Place an outpost beacon here within 5 minutes.";
                placementInstructions.Flags = (Message.Flag)0;
                placementInstructions.DisplayTime = 5;
                questScript.Properties.Add(new ScriptObjectProperty
                {
                    Name = "CPOI_Msg_PlacementInstructions",
                    Object = placementInstructions.ToLink()
                });

                var replacedOriginals = mod.FormLists.AddNew(ReplacedOriginalsEditorId);
                questScript.Properties.Add(new ScriptObjectProperty
                {
                    Name = "CPOI_ReplacedOriginals",
                    Object = replacedOriginals.ToLink()
                });

                var workshopReplacements = mod.FormLists.AddNew(WorkshopReplacementsEditorId);
                questScript.Properties.Add(new ScriptObjectProperty
                {
                    Name = "CPOI_WorkshopReplacements",
                    Object = workshopReplacements.ToLink()
                });

                // Append only: 0x80F is a private reference-link key. It never
                // changes the vanilla ship, workshop or procedural keywords.
                var landingAnchorLink = mod.Keywords.AddNew(ClaimLandingAnchorLinkEditorId);
                questScript.Properties.Add(new ScriptObjectProperty
                {
                    Name = ClaimLandingAnchorLinkEditorId,
                    Object = landingAnchorLink.ToLink()
                });

                // Append only: native protection uses these exact local IDs.
                var inheritedItems = mod.FormLists.AddNew(InheritedItemsEditorId);
                var protectedBeacons = mod.FormLists.AddNew(ProtectedBeaconsEditorId);
                questScript.Properties.Add(new ScriptObjectProperty { Name = InheritedItemsEditorId, Object = inheritedItems.ToLink() });
                questScript.Properties.Add(new ScriptObjectProperty { Name = ProtectedBeaconsEditorId, Object = protectedBeacons.ToLink() });

                // Attach the custom effect to the transponder.
                transponder.Effects.Add(new Effect
                {
                    BaseEffect = new FormLinkNullable<IMagicEffectGetter>(claimEffect.FormKey),
                    Data = new EffectData
                    {
                        Area = 0,
                        Duration = 0,
                        Magnitude = 0.0f
                    }
                });
            }

            // Write the ESM with master flag lookup for Starfield.esm.
            var masterFlagLookup = new Noggog.Cache<IModMasterStyledGetter, ModKey>(x => x.ModKey);
            masterFlagLookup.Add(new KeyedMasterStyle(ModKey.FromFileName("Starfield.esm"), MasterStyle.Full));
            var writeParams = new BinaryWriteParameters { MasterFlagsLookup = masterFlagLookup };
            mod.WriteToBinary(outputPath, writeParams);

            uint? seqQuestId = null;
            if (includePhase1)
            {
                var generatedQuest = mod.Quests.FirstOrDefault(q => q.EditorID == ClaimQuestEditorId);
                if (generatedQuest != null)
                {
                    // SEQ stores plugin-local FormIDs: high byte = master count (Starfield.esm is master 0).
                    seqQuestId = generatedQuest.FormKey.ID | 0x01000000;
                    WriteSeqFile(outputDir, seqQuestId.Value);
                }
            }

            // Verify the file was written.
            if (!File.Exists(outputPath))
            {
                return Blocked("OUTPUT_WRITE_FAILED",
                    "The ESM file was not produced at the expected path.", outputDir);
            }

            var sha256 = await ComputeSha256Async(outputPath);
            var fileSize = new FileInfo(outputPath).Length;

            return Success(new
            {
                operation = "generate",
                status = "succeeded",
                output_path = outputPath,
                file_size = fileSize,
                esm_sha256 = sha256,
                records_created = includePhase1 ? 18 : 3,
                seq_quest_form_id = seqQuestId.HasValue ? $"0x{seqQuestId.Value:X8}" : null,
                record_editor_ids = includePhase1
                    ? new[]
                    {
                        TransponderEditorId,
                        ClaimUnavailableEditorId,
                        AssistedInstructionsEditorId,
                        ClaimQuestEditorId,
                        ClaimableKeywordEditorId,
                        ClaimedPOIsEditorId,
                        ClaimedBeaconsEditorId,
                        OverlayClaimsEditorId,
                        ClaimedCountEditorId,
                        ClaimRadiusEditorId,
                        "CPOI_ClaimBeaconEffect",
                        ClearClutterEditorId,
                        PlacementInstructionsEditorId,
                        ReplacedOriginalsEditorId,
                        WorkshopReplacementsEditorId,
                        ClaimLandingAnchorLinkEditorId,
                        InheritedItemsEditorId,
                        ProtectedBeaconsEditorId
                    }
                    : new[]
                    {
                        TransponderEditorId,
                        ClaimUnavailableEditorId,
                        AssistedInstructionsEditorId
                    }
            });
        }
        catch (Exception ex)
        {
            return Blocked("GENERATION_EXCEPTION", ex.ToString(), outputDir);
        }
    }

    private static ToolResult Success(object data)
    {
        var json = SerializeJson(new
        {
            schema = SchemaVersion,
            data
        });
        return new ToolResult(json, 0);
    }

    private static ToolResult Blocked(string code, string message, string? outputDir)
    {
        var json = SerializeJson(new
        {
            schema = SchemaVersion,
            status = "blocked",
            code,
            message,
            output_directory = outputDir
        });
        return new ToolResult(json, 1);
    }

    private static string SerializeJson(object obj)
    {
        var options = new JsonSerializerOptions
        {
            WriteIndented = true,
            PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower
        };
        return JsonSerializer.Serialize(obj, options);
    }

    private static void WriteSeqFile(string outputDir, uint questFormId)
    {
        var seqDir = Path.Combine(outputDir, "Seq");
        Directory.CreateDirectory(seqDir);
        var seqPath = Path.Combine(seqDir, PluginName + ".seq");
        File.WriteAllBytes(seqPath, BitConverter.GetBytes(questFormId));
    }

    private static async Task<string> ComputeSha256Async(string path)
    {
        await using var stream = File.OpenRead(path);
        var hash = await SHA256.HashDataAsync(stream);
        return Convert.ToHexString(hash).ToLowerInvariant();
    }
}

public record ToolResult(string Json, int ExitCode);
