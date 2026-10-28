# Claimable POIs alpha.6 source

Code and build inputs for the alpha.6 release. SOURCE-IDENTITY.json records the
original build and runtime hashes; a local rebuild will have its own identity.

## Requirements

Windows x64, PowerShell 7, Visual Studio C++ tools, Git, XMake 3.0.9 and .NET SDK
10.0.401. See DEPENDENCIES.md for versions. Supply your own Starfield/Creation Kit
Papyrus inputs and install the runtime dependencies separately.

## Build

In a fresh extraction, initialize the included libraries at their exact public
upstream revisions. This sets up Git metadata without replacing the source:

```powershell
git -C external/CommonLibSF init
git -C external/CommonLibSF fetch --depth=1 https://github.com/libxse/commonlibsf.git 6eeb9e60590db21b6f983d487b6b2d51a0fb6726
git -C external/CommonLibSF reset --mixed FETCH_HEAD
git -C external/CommonLibSF/lib/commonlib-shared init
git -C external/CommonLibSF/lib/commonlib-shared fetch --depth=1 https://github.com/libxse/commonlib-shared.git 5470284e964d5510aa001dca3e0bb5548b6356a4
git -C external/CommonLibSF/lib/commonlib-shared reset --mixed FETCH_HEAD
git init
git add .gitignore .gitattributes .gitmodules src scripts tests tools patches external/CommonLibSF xmake.lua global.json DEPENDENCIES.md README.md LICENSE EXCEPTIONS THIRD-PARTY-NOTICES.md
git commit -m "Import alpha.6 source"
./scripts/prepare_papyrus_sources.ps1 -ContentResourcesPath 'YOUR-GAME/Tools/ContentResources.zip'
./scripts/build_candidate.ps1 -GameDataPath 'YOUR-GAME/Data'
```

Both library working trees must be clean. Build helpers apply and restore the
included compatibility patch. Install the pinned SDK in .cache/dotnet-10.0.401
or select it through CPOI_DOTNET_ROOT. Package with scripts/package.ps1; use the
guarded scripts/deploy.ps1 only with the game closed. Release archives contain
Data/ only, with legal notices under Data/Docs/ClaimablePOIs/.

Library source and licenses are under external/; upstream/ contains the exact
spdlog source ZIP. Public dependency downloads are needed for rebuilding. Vendor
support files are kept so the guarded library checkouts remain clean.

## License

GPL-3.0-or-later with the supplied EXCEPTIONS and third-party notices.
Code and documentation were developed with AI assistance, including generated code.