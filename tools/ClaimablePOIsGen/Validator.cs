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

public static class Validator
{
    public const string SchemaVersion = "claimable-pois.mutagen-validation.v1";

    // Must match Generator and the DLL's Forms::Resolve.
    public const string TransponderEditorId = "CPOI_ClaimTransponder";
    public const string ClaimUnavailableEditorId = "CPOI_Msg_ClaimUnavailable";
    public const string AssistedInstructionsEditorId = "CPOI_Msg_AssistedInstructions";
    public const string ClearClutterEditorId = "CPOI_Msg_ClearClutter";
    public const string PlacementInstructionsEditorId = "CPOI_Msg_PlacementInstructions";

    private const string AssistedInstructionsText = "POI claimed. Outpost beacon registered.";
    private const string PlacementInstructionsText =
        "Build area unlocked. Place an outpost beacon here within 5 minutes.";
    private const string ClearClutterText =
        "After this POI is successfully claimed, permanently hide eligible nearby dead bodies? Quest-linked bodies and other clutter will remain. Cancelled or failed claims leave bodies unchanged.";

    // Phase 1 records — must match Generator.
    public const string OverlayClaimsEditorId = "CPOI_OverlayClaims";
    public const string ClaimQuestEditorId = "CPOI_ClaimQuest";
    public const string ClaimableKeywordEditorId = "CPOI_LocTypeClaimablePOI";
    public const string ClaimedPOIsEditorId = "CPOI_ClaimedPOIs";
    public const string ClaimedBeaconsEditorId = "CPOI_ClaimedBeacons";
    public const string ReplacedOriginalsEditorId = "CPOI_ReplacedOriginals";
    public const string WorkshopReplacementsEditorId = "CPOI_WorkshopReplacements";
    public const string ClaimLandingAnchorLinkEditorId = "CPOI_ClaimLandingAnchorLink";
    public const string InheritedItemsEditorId = "CPOI_InheritedItems";
    public const string ProtectedBeaconsEditorId = "CPOI_ProtectedBeacons";
    public const string ClaimedCountEditorId = "CPOI_ClaimedCount";
    public const string ClaimRadiusEditorId = "CPOI_ClaimRadius";

    public static ToolResult Validate(string inputPath)
    {
        try
        {
            if (!File.Exists(inputPath))
            {
                return Blocked("INPUT_MISSING",
                    $"Input file not found: {inputPath}", inputPath);
            }

            var fileName = Path.GetFileName(inputPath);
            if (!fileName.Equals(Generator.PluginName, StringComparison.OrdinalIgnoreCase))
            {
                return Blocked("INPUT_FILENAME_MISMATCH",
                    $"Input must be named exactly {Generator.PluginName}; found {fileName}.", inputPath);
            }

            // Read the ESM using Mutagen with master flag lookup for Starfield.esm.
            var masterFlagLookup = new Noggog.Cache<IModMasterStyledGetter, ModKey>(x => x.ModKey);
            masterFlagLookup.Add(new KeyedMasterStyle(ModKey.FromFileName("Starfield.esm"), MasterStyle.Full));
            var readParams = new BinaryReadParameters { MasterFlagsLookup = masterFlagLookup };
            var mod = StarfieldMod.CreateFromBinary(inputPath, StarfieldRelease.Starfield, readParams);

            var errors = new List<string>();

            // Verify the mod is marked as a master (ESM).
            if (mod.ModKey.Type != ModType.Master)
            {
                errors.Add($"Mod is not marked as a master (ESM); ModKey type is {mod.ModKey.Type}.");
            }

            // Verify CPOI_ClaimBeaconEffect (MagicEffect) exists.
            var claimEffect = mod.MagicEffects
                .FirstOrDefault(r => r.EditorID == "CPOI_ClaimBeaconEffect");
            if (claimEffect == null)
            {
                errors.Add("Missing MagicEffect with EditorID 'CPOI_ClaimBeaconEffect'.");
            }
            else
            {
                if (claimEffect.VirtualMachineAdapter == null)
                    errors.Add("MagicEffect 'CPOI_ClaimBeaconEffect' has no VirtualMachineAdapter (script attachment).");
            }

            // Verify exactly the three required records exist with correct EditorIDs and types.
            var transponder = mod.Ingestibles
                .FirstOrDefault(r => r.EditorID == TransponderEditorId);
            if (transponder == null)
            {
                errors.Add($"Missing Ingestible with EditorID '{TransponderEditorId}'.");
            }
            else
            {
                if (transponder.Name == null || string.IsNullOrWhiteSpace(transponder.Name.String))
                    errors.Add($"Ingestible '{TransponderEditorId}' has no Name.");
            }

            var claimUnavailable = mod.Messages
                .FirstOrDefault(r => r.EditorID == ClaimUnavailableEditorId);
            if (claimUnavailable == null)
            {
                errors.Add($"Missing Message with EditorID '{ClaimUnavailableEditorId}'.");
            }
            else
            {
                if (claimUnavailable.Name == null || string.IsNullOrWhiteSpace(claimUnavailable.Name.String))
                    errors.Add($"Message '{ClaimUnavailableEditorId}' has no Name.");
                if (claimUnavailable.Description == null || string.IsNullOrWhiteSpace(claimUnavailable.Description.String))
                    errors.Add($"Message '{ClaimUnavailableEditorId}' has no Description.");
            }

            var assistedInstructions = mod.Messages
                .FirstOrDefault(r => r.EditorID == AssistedInstructionsEditorId);
            if (assistedInstructions == null)
            {
                errors.Add($"Missing Message with EditorID '{AssistedInstructionsEditorId}'.");
            }
            else
            {
                if (assistedInstructions.Name == null || string.IsNullOrWhiteSpace(assistedInstructions.Name.String))
                    errors.Add($"Message '{AssistedInstructionsEditorId}' has no Name.");
                if (assistedInstructions.Description == null || assistedInstructions.Description.String != AssistedInstructionsText)
                    errors.Add($"Message '{AssistedInstructionsEditorId}' must use the concise success text and no alternate text.");
                if (assistedInstructions.Flags != (Message.Flag)0)
                    errors.Add($"Message '{AssistedInstructionsEditorId}' must be non-modal (Flags=0).");
                if (assistedInstructions.DisplayTime != 5)
                    errors.Add($"Message '{AssistedInstructionsEditorId}' must have DisplayTime=5 seconds.");
            }

            var placementInstructions = mod.Messages
                .FirstOrDefault(r => r.EditorID == PlacementInstructionsEditorId);
            if (placementInstructions == null)
            {
                errors.Add($"Missing Message with EditorID '{PlacementInstructionsEditorId}'.");
            }
            else
            {
                if (placementInstructions.Name == null || string.IsNullOrWhiteSpace(placementInstructions.Name.String))
                    errors.Add($"Message '{PlacementInstructionsEditorId}' has no Name.");
                if (placementInstructions.Description == null || placementInstructions.Description.String != PlacementInstructionsText)
                    errors.Add($"Message '{PlacementInstructionsEditorId}' must use the concise placement text and no alternate text.");
                if (placementInstructions.Flags != (Message.Flag)0)
                    errors.Add($"Message '{PlacementInstructionsEditorId}' must be non-modal (Flags=0).");
                if (placementInstructions.DisplayTime != 5)
                    errors.Add($"Message '{PlacementInstructionsEditorId}' must have DisplayTime=5 seconds.");
            }

            var clearClutter = mod.Messages
                .FirstOrDefault(r => r.EditorID == ClearClutterEditorId);
            if (clearClutter == null)
            {
                errors.Add($"Missing Message with EditorID '{ClearClutterEditorId}'.");
            }
            else
            {
                if (clearClutter.Description == null || clearClutter.Description.String != ClearClutterText)
                    errors.Add($"Message '{ClearClutterEditorId}' must use the concise cleanup prompt.");
                if (clearClutter.Flags != Message.Flag.MessageBox)
                    errors.Add($"Message '{ClearClutterEditorId}' must be a modal MessageBox.");

                var expectedButtons = new[] { "Clear Dead Bodies", "Leave All" };
                if (clearClutter.MenuButtons.Count != expectedButtons.Length)
                {
                    errors.Add($"Message '{ClearClutterEditorId}' must have exactly 2 buttons in the expected order.");
                }
                else
                {
                    for (var i = 0; i < expectedButtons.Length; i++)
                    {
                        var button = clearClutter.MenuButtons[i];
                        if (button.Text == null || button.Text.String != expectedButtons[i])
                            errors.Add($"Message '{ClearClutterEditorId}' button {i} must be '{expectedButtons[i]}'.");
                        if (!string.IsNullOrEmpty(button.ButtonText))
                            errors.Add($"Message '{ClearClutterEditorId}' button {i} must leave ButtonText empty so the game supplies the control prompt.");
                    }
                }
            }

            // Verify the Phase 1 records exist with correct EditorIDs and types.
            var replacedOriginals = mod.FormLists
                .FirstOrDefault(r => r.EditorID == ReplacedOriginalsEditorId);
            var workshopReplacements = mod.FormLists
                .FirstOrDefault(r => r.EditorID == WorkshopReplacementsEditorId);
            var claimQuest = mod.Quests
                .FirstOrDefault(r => r.EditorID == ClaimQuestEditorId);
            if (claimQuest == null)
            {
                errors.Add($"Missing Quest with EditorID '{ClaimQuestEditorId}'.");
            }
            else
            {
                if (claimQuest.Name == null || string.IsNullOrWhiteSpace(claimQuest.Name.String))
                    errors.Add($"Quest '{ClaimQuestEditorId}' has no Name.");
                if (claimQuest.Data == null)
                    errors.Add($"Quest '{ClaimQuestEditorId}' has no Data.");
                if (claimQuest.VirtualMachineAdapter == null)
                    errors.Add($"Quest '{ClaimQuestEditorId}' has no VirtualMachineAdapter.");
                else
                {
                    var expectedQuestPropertyNames = new HashSet<string>(StringComparer.Ordinal)
                    {
                        "CPOI_ClaimedCount",
                        "CPOI_ClaimRadius",
                        "CPOI_ClaimedPOIs",
                        "CPOI_ClaimedBeacons",
                        "CPOI_LocTypeClaimablePOI",
                        "CPOI_Msg_ClaimUnavailable",
                        "CPOI_Msg_AssistedInstructions",
                        "CPOI_ClaimTransponder",
                        "PlayerFaction",
                        "CPOI_Msg_ClearClutter",
                        "CPOI_OverlayClaims",
                        "CPOI_Msg_PlacementInstructions",
                        "CPOI_ReplacedOriginals",
                        "CPOI_WorkshopReplacements",
                        ClaimLandingAnchorLinkEditorId,
                        InheritedItemsEditorId,
                        ProtectedBeaconsEditorId
                    };
                    var questScripts = claimQuest.VirtualMachineAdapter.Scripts;
                    if (questScripts.Count != 1)
                    {
                        errors.Add($"Quest '{ClaimQuestEditorId}' must contain exactly one VMAD script entry; found {questScripts.Count}.");
                    }
                    else
                    {
                        var questScript = questScripts[0];
                        if (questScript.Name != "CPOI_ClaimQuestScript")
                            errors.Add($"Quest '{ClaimQuestEditorId}' VMAD script must be named 'CPOI_ClaimQuestScript'.");

                        if (questScript.Properties.Count != expectedQuestPropertyNames.Count)
                        {
                            errors.Add($"Quest '{ClaimQuestEditorId}' script must contain exactly {expectedQuestPropertyNames.Count} properties; found {questScript.Properties.Count}.");
                        }

                        foreach (var property in questScript.Properties)
                        {
                            if (!expectedQuestPropertyNames.Contains(property.Name))
                                errors.Add($"Quest '{ClaimQuestEditorId}' script contains unexpected property '{property.Name}'.");
                        }

                        foreach (var expectedPropertyName in expectedQuestPropertyNames)
                        {
                            if (!questScript.Properties.Any(p => p.Name == expectedPropertyName))
                                errors.Add($"Quest '{ClaimQuestEditorId}' script is missing property '{expectedPropertyName}'.");
                        }

                        var landingLink = mod.Keywords.FirstOrDefault(r => r.EditorID == ClaimLandingAnchorLinkEditorId);
                        var landingProperty = questScript.Properties.FirstOrDefault(p => p.Name == ClaimLandingAnchorLinkEditorId);
                        if (landingLink == null || landingLink.FormKey.ID != 0x80F ||
                            landingProperty is not ScriptObjectProperty landingObjectProperty ||
                            landingObjectProperty.Object == null || landingObjectProperty.Object.FormKey != landingLink.FormKey)
                        {
                            errors.Add($"Quest landing link must bind private keyword '{ClaimLandingAnchorLinkEditorId}' at local 0x80F.");
                        }

                        foreach (var (name, id) in new[] { (InheritedItemsEditorId, 0x810u), (ProtectedBeaconsEditorId, 0x811u) })
                        {
                            var list = mod.FormLists.FirstOrDefault(x => x.EditorID == name);
                            var property = questScript.Properties.FirstOrDefault(x => x.Name == name);
                            if (list == null || list.FormKey.ID != id ||
                                property is not ScriptObjectProperty bound || bound.Object == null || bound.Object.FormKey != list.FormKey)
                                errors.Add($"Teardown ledger '{name}' must bind its own FormList at local 0x{id:X}.");
                        }

                        var placementProperty = questScript.Properties
                            .FirstOrDefault(p => p.Name == "CPOI_Msg_PlacementInstructions");
                        if (placementProperty is not ScriptObjectProperty placementObjectProperty)
                        {
                            errors.Add($"Quest '{ClaimQuestEditorId}' placement property must be a ScriptObjectProperty.");
                        }
                        else if (placementInstructions == null
                                 || placementObjectProperty.Object == null
                                 || placementObjectProperty.Object.FormKey != placementInstructions.FormKey)
                        {
                            errors.Add($"Quest '{ClaimQuestEditorId}' placement property must link to '{PlacementInstructionsEditorId}'.");
                        }

                        var replacedOriginalsProperty = questScript.Properties
                            .FirstOrDefault(p => p.Name == ReplacedOriginalsEditorId);
                        if (replacedOriginalsProperty is not ScriptObjectProperty replacedOriginalsObjectProperty)
                        {
                            errors.Add($"Quest '{ClaimQuestEditorId}' '{ReplacedOriginalsEditorId}' property must be a ScriptObjectProperty.");
                        }
                        else if (replacedOriginals == null
                                 || replacedOriginalsObjectProperty.Object == null
                                 || replacedOriginalsObjectProperty.Object.FormKey != replacedOriginals.FormKey)
                        {
                            errors.Add($"Quest '{ClaimQuestEditorId}' property must link to '{ReplacedOriginalsEditorId}'.");
                        }

                        var workshopReplacementsProperty = questScript.Properties
                            .FirstOrDefault(p => p.Name == WorkshopReplacementsEditorId);
                        if (workshopReplacementsProperty is not ScriptObjectProperty workshopReplacementsObjectProperty)
                        {
                            errors.Add($"Quest '{ClaimQuestEditorId}' '{WorkshopReplacementsEditorId}' property must be a ScriptObjectProperty.");
                        }
                        else if (workshopReplacements == null
                                 || workshopReplacementsObjectProperty.Object == null
                                 || workshopReplacementsObjectProperty.Object.FormKey != workshopReplacements.FormKey)
                        {
                            errors.Add($"Quest '{ClaimQuestEditorId}' property must link to '{WorkshopReplacementsEditorId}'.");
                        }
                    }
                }
            }

            var claimableKeyword = mod.Keywords
                .FirstOrDefault(r => r.EditorID == ClaimableKeywordEditorId);
            if (claimableKeyword == null)
            {
                errors.Add($"Missing Keyword with EditorID '{ClaimableKeywordEditorId}'.");
            }

            var claimedPOIs = mod.FormLists
                .FirstOrDefault(r => r.EditorID == ClaimedPOIsEditorId);
            if (claimedPOIs == null)
            {
                errors.Add($"Missing FormList with EditorID '{ClaimedPOIsEditorId}'.");
            }

            var claimedBeacons = mod.FormLists
                .FirstOrDefault(r => r.EditorID == ClaimedBeaconsEditorId);
            if (claimedBeacons == null)
            {
                errors.Add($"Missing FormList with EditorID '{ClaimedBeaconsEditorId}'.");
            }

            var overlayClaims = mod.FormLists
                .FirstOrDefault(r => r.EditorID == OverlayClaimsEditorId);
            if (overlayClaims == null)
            {
                errors.Add($"Missing FormList with EditorID '{OverlayClaimsEditorId}'.");
            }

            if (replacedOriginals == null)
            {
                errors.Add($"Missing FormList with EditorID '{ReplacedOriginalsEditorId}'.");
            }

            if (workshopReplacements == null)
            {
                errors.Add($"Missing FormList with EditorID '{WorkshopReplacementsEditorId}'.");
            }

            var claimedCount = mod.Globals
                .FirstOrDefault(r => r.EditorID == ClaimedCountEditorId);
            if (claimedCount == null)
            {
                errors.Add($"Missing Global with EditorID '{ClaimedCountEditorId}'.");
            }
            else if (claimedCount.Data == null)
            {
                errors.Add($"Global '{ClaimedCountEditorId}' has no Data.");
            }

            var claimRadius = mod.Globals
                .FirstOrDefault(r => r.EditorID == ClaimRadiusEditorId);
            if (claimRadius == null)
            {
                errors.Add($"Missing Global with EditorID '{ClaimRadiusEditorId}'.");
            }
            else if (claimRadius.Data == null)
            {
                errors.Add($"Global '{ClaimRadiusEditorId}' has no Data.");
            }

            // Verify no banned record groups contain overrides.
            if (mod.PackIns.Count > 0)
                errors.Add($"Banned group 'PackIns' contains {mod.PackIns.Count} record(s); expected 0.");
            if (mod.Cells.Count > 0)
                errors.Add($"Banned group 'Cells' contains {mod.Cells.Count} record(s); expected 0.");
            if (mod.Worldspaces.Count > 0)
                errors.Add($"Banned group 'Worldspaces' contains {mod.Worldspaces.Count} record(s); expected 0.");
            if (mod.NavigationMeshInfoMaps.Count > 0)
                errors.Add($"Banned group 'NavigationMeshInfoMaps' contains {mod.NavigationMeshInfoMaps.Count} record(s); expected 0.");

            // Verify no Bethesda master overrides — all records must originate from our plugin.
            foreach (var record in mod.EnumerateMajorRecords())
            {
                if (record.FormKey.ModKey != mod.ModKey)
                {
                    errors.Add($"Record {record.FormKey} does not originate from {mod.ModKey}; "
                               + "Bethesda master overrides are not permitted.");
                }
            }

            if (errors.Count > 0)
            {
                return Blocked("STRUCTURAL_VALIDATION_FAILED",
                    string.Join(" | ", errors), inputPath);
            }

            var sha256 = ComputeSha256(inputPath);
            var fileSize = new FileInfo(inputPath).Length;

            return Success(new
            {
                operation = "validate",
                status = "passed",
                input_path = inputPath,
                filename = fileName,
                file_size = fileSize,
                esm_sha256 = sha256,
                mod_key = mod.ModKey.FileName,
                mod_type = mod.ModKey.Type.ToString(),
                records_verified = 16,
                record_editor_ids = new[]
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
            });
        }
        catch (Exception ex)
        {
            return Blocked("VALIDATION_EXCEPTION", ex.ToString(), inputPath);
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

    private static ToolResult Blocked(string code, string message, string inputPath)
    {
        var json = SerializeJson(new
        {
            schema = SchemaVersion,
            status = "blocked",
            code,
            message,
            input_path = inputPath
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

    private static string ComputeSha256(string path)
    {
        using var stream = File.OpenRead(path);
        var hash = SHA256.HashData(stream);
        return Convert.ToHexString(hash).ToLowerInvariant();
    }
}
