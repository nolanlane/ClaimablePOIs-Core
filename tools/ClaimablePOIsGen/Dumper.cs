using System.Reflection;
using System.Text.Json;
using Mutagen.Bethesda;
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Plugins.Binary.Headers;
using Mutagen.Bethesda.Plugins.Binary.Parameters;
using Mutagen.Bethesda.Plugins.Masters;
using Mutagen.Bethesda.Plugins.Records;
using Mutagen.Bethesda.Starfield;
using Noggog;

namespace ClaimablePOIsGen;

public static class Dumper
{
    public static ToolResult Dump(string inputFile, string? dataDir = null)
    {
        try
        {
            dataDir ??= Path.GetDirectoryName(inputFile)!;
            var modKey = ModKey.FromFileName(Path.GetFileName(inputFile));

            // Build master flag lookup for all masters by reading each master's header.
            // Official Starfield masters have known styles.
            var masterFlagLookup = new Noggog.Cache<IModMasterStyledGetter, ModKey>(x => x.ModKey);

            // Add known official masters
            masterFlagLookup.Add(new KeyedMasterStyle(ModKey.FromFileName("Starfield.esm"), MasterStyle.Full));
            masterFlagLookup.Add(new KeyedMasterStyle(ModKey.FromFileName("Constellation.esm"), MasterStyle.Small));
            masterFlagLookup.Add(new KeyedMasterStyle(ModKey.FromFileName("OldMars.esm"), MasterStyle.Small));
            masterFlagLookup.Add(new KeyedMasterStyle(ModKey.FromFileName("BlueprintShips-Starfield.esm"), MasterStyle.Full));
            masterFlagLookup.Add(new KeyedMasterStyle(ModKey.FromFileName("ShatteredSpace.esm"), MasterStyle.Full));
            masterFlagLookup.Add(new KeyedMasterStyle(ModKey.FromFileName("SFBGS003.esm"), MasterStyle.Medium));
            masterFlagLookup.Add(new KeyedMasterStyle(ModKey.FromFileName("SFBGS004.esm"), MasterStyle.Medium));
            masterFlagLookup.Add(new KeyedMasterStyle(ModKey.FromFileName("SFBGS006.esm"), MasterStyle.Medium));
            masterFlagLookup.Add(new KeyedMasterStyle(ModKey.FromFileName("SFBGS007.esm"), MasterStyle.Small));
            masterFlagLookup.Add(new KeyedMasterStyle(ModKey.FromFileName("SFBGS008.esm"), MasterStyle.Small));
            masterFlagLookup.Add(new KeyedMasterStyle(ModKey.FromFileName("SFBGS00D.esm"), MasterStyle.Full));

            // Also scan the data dir for other .esm files and add them as Full style
            foreach (var esmFile in Directory.GetFiles(dataDir, "*.esm"))
            {
                var esmKey = ModKey.FromFileName(Path.GetFileName(esmFile));
                if (!masterFlagLookup.ContainsKey(esmKey))
                {
                    try
                    {
                        var hdr = ModHeaderFrame.FromPath(esmFile, GameRelease.Starfield);
                        masterFlagLookup.Add(new KeyedMasterStyle(esmKey, hdr.MasterStyle));
                    }
                    catch
                    {
                        masterFlagLookup.Add(new KeyedMasterStyle(esmKey, MasterStyle.Full));
                    }
                }
            }

            var readParams = new BinaryReadParameters { MasterFlagsLookup = masterFlagLookup };
            var mod = StarfieldMod.CreateFromBinary(inputFile, StarfieldRelease.Starfield, readParams);
            Console.Error.WriteLine($"MOD: {Path.GetFileName(inputFile)}");
            Console.Error.WriteLine($"  Masters: {string.Join(", ", mod.ModHeader.MasterReferences.Select(m => m.Master.FileName))}");
            Console.Error.WriteLine($"  Flags: {mod.ModHeader.Flags}");

            foreach (var rec in mod.EnumerateMajorRecords())
            {
                var edid = rec.EditorID ?? "<none>";
                Console.WriteLine($"REC: {rec.GetType().Name} formID=0x{rec.FormKey.ID:X8} edid=\"{edid}\"");

                if (rec is Quest quest)
                {
                    DumpQuest(quest);
                }
                else if (rec is Mutagen.Bethesda.Starfield.Activator activ)
                {
                    Console.WriteLine($"  Activator: model={activ.Model != null} script={activ.VirtualMachineAdapter != null} " +
                        $"keywords={activ.Keywords?.Count ?? 0}");
                }
                else if (rec is Global glbl)
                {
                    Console.WriteLine($"  Global: data={glbl.Data}");
                }
                else if (rec is FormList list)
                {
                    Console.WriteLine($"  FormList: items={list.Items.Count}");
                }
                else if (rec is Message msg)
                {
                    Console.WriteLine($"  Message: name={msg.Name?.String} desc={msg.Description?.String}");
                }
                else if (rec is MiscItem misc)
                {
                    Console.WriteLine($"  MiscItem: name={misc.Name?.String}");
                }
                else if (rec is Ingestible ing)
                {
                    Console.WriteLine($"  Ingestible: name={ing.Name?.String} weight={ing.Weight} value={ing.Value} effects={ing.Effects.Count}");
                    foreach (var eff in ing.Effects)
                    {
                        Console.WriteLine($"    Effect: BaseEffect={eff.BaseEffect.FormKey} Mag={eff.Data?.Magnitude} Dur={eff.Data?.Duration} Area={eff.Data?.Area}");
                    }
                }
                else if (rec is MagicEffect mgef)
                {
                    Console.WriteLine($"  MagicEffect: Archetype={mgef.Archetype} Flags={mgef.Flags} CastType={mgef.CastType} TargetType={mgef.TargetType} State={mgef.DATADataTypeState}");
                }
            }

            var json = JsonSerializer.Serialize(new
            {
                operation = "dump",
                status = "succeeded",
                input_path = inputFile,
                records = mod.EnumerateMajorRecords().Count()
            }, new JsonSerializerOptions { WriteIndented = true });
            return new ToolResult(json, 0);
        }
        catch (Exception ex)
        {
            var json = JsonSerializer.Serialize(new
            {
                operation = "dump",
                status = "failed",
                input_path = inputFile,
                error = ex.ToString()
            }, new JsonSerializerOptions { WriteIndented = true });
            return new ToolResult(json, 1);
        }
    }

    private static void DumpQuest(Quest quest)
    {
        Console.WriteLine($"  Quest.FormVersion = {quest.FormVersion}");
        Console.WriteLine($"  Quest.QuestType = {quest.QuestType}");
        Console.WriteLine($"  Quest.VirtualMachineAdapter = {(quest.VirtualMachineAdapter != null ? "Present" : "Null")}");
        if (quest.VirtualMachineAdapter != null)
        {
            Console.WriteLine($"    VMAD Type: {quest.VirtualMachineAdapter.GetType().FullName}");
            Console.WriteLine($"    VMAD Version: {quest.VirtualMachineAdapter.Version}");
            Console.WriteLine($"    VMAD ObjectFormat: {quest.VirtualMachineAdapter.ObjectFormat}");
            Console.WriteLine($"    VMAD Scripts count: {quest.VirtualMachineAdapter.Scripts.Count}");
            foreach (var s in quest.VirtualMachineAdapter.Scripts)
            {
                Console.WriteLine($"      Script: {s.Name} (Type: {s.GetType().FullName})");
                foreach (var p in s.Properties)
                {
                    Console.WriteLine($"        Prop: {p.Name} (Type: {p.GetType().FullName})");
                }
            }
        }
        Console.WriteLine($"  Quest.Stages count: {quest.Stages.Count}");
        foreach (var st in quest.Stages)
        {
            Console.WriteLine($"    Stage: index={st.Index} flags={st.Flags}");
        }
        Console.WriteLine($"  Quest.Aliases count: {quest.Aliases?.Count ?? 0}");
        if (quest.Aliases != null)
        {
            foreach (var al in quest.Aliases)
            {
                Console.WriteLine($"    Alias: {al.GetType().Name}");
            }
        }
        Console.WriteLine($"  Quest.Components count: {quest.Components.Count}");
        foreach (var c in quest.Components)
        {
            Console.WriteLine($"    Component: {c.GetType().Name}");
        }

        if (quest.Data != null)
        {
            Console.WriteLine($"  QuestData.Flags = {quest.Data.Flags}");
            Console.WriteLine($"  QuestData.Priority = {quest.Data.Priority}");
            Console.WriteLine($"  QuestData.Type = {quest.Data.Type}");
            Console.WriteLine($"  QuestData.Unused length = {quest.Data.Unused.Length}");
        }
    }
}
