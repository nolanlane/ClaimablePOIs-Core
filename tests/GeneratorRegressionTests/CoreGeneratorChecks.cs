using ClaimablePOIsGen;
using Mutagen.Bethesda;
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Plugins.Binary.Headers;
using Mutagen.Bethesda.Plugins.Binary.Parameters;
using Mutagen.Bethesda.Plugins.Masters;
using Mutagen.Bethesda.Plugins.Records;
using Mutagen.Bethesda.Starfield;
using Noggog;

internal static class CoreGeneratorChecks
{
    internal static void Run(string corePath, string output)
    {
        var styles = new Cache<IModMasterStyledGetter, ModKey>(x => x.ModKey);
        styles.Add(new KeyedMasterStyle(ModKey.FromFileName("Starfield.esm"), MasterStyle.Full));
        styles.Add(new KeyedMasterStyle(ModKey.FromFileName("ClaimablePOIs.esm"), MasterStyle.Full));
        var read = new BinaryReadParameters { MasterFlagsLookup = styles };
        var write = new BinaryWriteParameters { MasterFlagsLookup = styles };
        var core = StarfieldMod.CreateFromBinary(corePath, StarfieldRelease.Starfield, read);
        if (Validator.Validate(corePath).ExitCode != 0) throw new InvalidDataException("Core validation failed.");
        var legacy = new[] {
            "CPOI_ClaimTransponder", "CPOI_Msg_ClaimUnavailable", "CPOI_Msg_AssistedInstructions",
            "CPOI_LocTypeClaimablePOI", "CPOI_ClaimedPOIs", "CPOI_ClaimedBeacons",
            "CPOI_ClaimedCount", "CPOI_ClaimRadius", "CPOI_ClaimQuest", "CPOI_Msg_ClearClutter",
            "CPOI_ClaimBeaconEffect", "CPOI_OverlayClaims", "CPOI_Msg_PlacementInstructions",
            "CPOI_ReplacedOriginals", "CPOI_WorkshopReplacements"
        };
        var records = core.EnumerateMajorRecords().ToDictionary(x => x.EditorID!);
        for (var i = 0; i < legacy.Length; ++i)
            if (records[legacy[i]].FormKey.ID != 0x800u + i)
                throw new InvalidDataException($"Serialized/native legacy FormID moved: {legacy[i]}");
        if (records.Count != 18 || records[Generator.ClaimLandingAnchorLinkEditorId].FormKey.ID != 0x80F ||
            records[Generator.InheritedItemsEditorId].FormKey.ID != 0x810 || records[Generator.ProtectedBeaconsEditorId].FormKey.ID != 0x811)
            throw new InvalidDataException("Existing 16 IDs must remain fixed; protection lists append at 0x810/0x811.");

        void Reject(string name, Action<StarfieldMod> mutate)
        {
            var changed = StarfieldMod.CreateFromBinary(corePath, StarfieldRelease.Starfield, read);
            mutate(changed);
            var folder = Path.Combine(output, name);
            if (Directory.Exists(folder)) throw new IOException($"Refusing to reuse fixture {folder}");
            Directory.CreateDirectory(folder);
            var path = Path.Combine(folder, Generator.PluginName);
            changed.WriteToBinary(path, write);
            if (Validator.Validate(path).ExitCode == 0) throw new InvalidDataException($"Invalid core accepted: {name}");
        }
        Reject("missing-landing-link-property", mod => {
            var props = mod.Quests.Single().VirtualMachineAdapter!.Scripts.Single().Properties;
            props.Remove(props.Single(x => x.Name == Generator.ClaimLandingAnchorLinkEditorId));
        });
        Reject("landing-link-bound-to-location-keyword", mod => {
            var prop = (ScriptObjectProperty)mod.Quests.Single().VirtualMachineAdapter!.Scripts.Single().Properties
                .Single(x => x.Name == Generator.ClaimLandingAnchorLinkEditorId);
            prop.Object = mod.Keywords.Single(x => x.EditorID == Generator.ClaimableKeywordEditorId).ToLink();
        });
        Reject("missing-private-keyword", mod => {
            mod.Keywords.Remove(mod.Keywords.Single(x => x.EditorID == Generator.ClaimLandingAnchorLinkEditorId));
        });
        foreach (var name in new[] { Generator.InheritedItemsEditorId, Generator.ProtectedBeaconsEditorId })
        {
            Reject("missing-" + name, mod => {
                mod.FormLists.Remove(mod.FormLists.Single(x => x.EditorID == name));
            });
            Reject("misbound-" + name, mod => {
                var prop = (ScriptObjectProperty)mod.Quests.Single().VirtualMachineAdapter!.Scripts.Single().Properties.Single(x => x.Name == name);
                prop.Object = mod.FormLists.Single(x => x.EditorID == Generator.ClaimedBeaconsEditorId).ToLink();
            });
        }
        Console.WriteLine("Core generator regressions passed: 16 existing IDs, append-only protection lists, 7 invalid bindings rejected.");
    }
}
