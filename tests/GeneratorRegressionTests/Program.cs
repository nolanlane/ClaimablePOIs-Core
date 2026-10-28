using ClaimablePOIsGen;
if (args.Length != 3 || args[0] != "--core")
    throw new ArgumentException("Expected --core <ClaimablePOIs.esm> <fresh output directory>.");
CoreGeneratorChecks.Run(args[1], args[2]);
