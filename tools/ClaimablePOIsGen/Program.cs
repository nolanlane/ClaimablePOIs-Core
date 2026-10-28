using ClaimablePOIsGen;
if (args.Length == 0) return PrintUsage();
var rest = args.Skip(1).ToArray();
return args[0].ToLowerInvariant() switch
{
    "generate" => await RunGenerate(rest),
    "validate" => RunValidate(rest),
    "dump" => RunDump(rest),
    "--help" or "-h" or "help" => PrintUsage(),
    _ => PrintUsage($"Unknown core command: {args[0]}")
};
static int PrintUsage(string? error = null)
{
    if (error != null) Console.Error.WriteLine(error);
    Console.Error.WriteLine("ClaimablePOIsGen generate --output <dir> [--result-file <file>]");
    Console.Error.WriteLine("ClaimablePOIsGen validate --input <esm> [--result-file <file>]");
    Console.Error.WriteLine("ClaimablePOIsGen dump --input <esm> [--data-dir <dir>]");
    return 1;
}
static async Task<int> RunGenerate(string[] args)
{
    string? outputDir = null;
    string? resultFile = null;
    var minimal = false;
    for (var i = 0; i < args.Length; i++)
    {
        if (args[i] == "--output" && i + 1 < args.Length)
            outputDir = args[++i];
        else if (args[i] == "--result-file" && i + 1 < args.Length)
            resultFile = args[++i];
        else if (args[i] == "--minimal")
            minimal = true;
    }
    if (string.IsNullOrEmpty(outputDir))
    {
        Console.Error.WriteLine("Error: --output <directory> is required");
        return 1;
    }

    var result = await Generator.GenerateAsync(outputDir, includePhase1: !minimal);
    OutputResult(result, resultFile);
    return result.ExitCode;
}

static int RunValidate(string[] args)
{
    string? inputFile = null;
    string? resultFile = null;
    for (var i = 0; i < args.Length; i++)
    {
        if (args[i] == "--input" && i + 1 < args.Length)
            inputFile = args[++i];
        else if (args[i] == "--result-file" && i + 1 < args.Length)
            resultFile = args[++i];
    }
    if (string.IsNullOrEmpty(inputFile))
    {
        Console.Error.WriteLine("Error: --input <file> is required");
        return 1;
    }

    var result = Validator.Validate(inputFile);
    OutputResult(result, resultFile);
    return result.ExitCode;
}

static int RunDump(string[] args)
{
    string? inputFile = null;
    string? dataDir = null;
    for (var i = 0; i < args.Length; i++)
    {
        if (args[i] == "--input" && i + 1 < args.Length)
            inputFile = args[++i];
        else if (args[i] == "--data-dir" && i + 1 < args.Length)
            dataDir = args[++i];
    }
    if (string.IsNullOrEmpty(inputFile))
    {
        Console.Error.WriteLine("Error: --input <file> is required");
        return 1;
    }

    var result = Dumper.Dump(inputFile, dataDir);
    Console.Error.WriteLine(result.Json);
    return result.ExitCode;
}

static void OutputResult(ToolResult result, string? resultFile)
{
    if (!string.IsNullOrEmpty(resultFile))
    {
        var dir = Path.GetDirectoryName(resultFile);
        if (!string.IsNullOrEmpty(dir))
            Directory.CreateDirectory(dir);
        File.WriteAllText(resultFile, result.Json + Environment.NewLine);
    }
    else
    {
        Console.WriteLine(result.Json);
    }
}
