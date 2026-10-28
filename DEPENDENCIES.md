# Build dependencies

| Input | Version |
|---|---|
| Steam Starfield | 1.16.244.0 |
| SFSE | 0.2.21 — https://sfse.silverlock.org/ |
| Address Library | v22; versionlib-1-16-244-0.bin — Nexus mod3256 |
| Cassiopeia | 10.0 — Nexus mod10896 |
| CommonLibSF | 6eeb9e60590db21b6f983d487b6b2d51a0fb6726 |
| commonlib-shared | 5470284e964d5510aa001dca3e0bb5548b6356a4 |
| spdlog | 1.16.0, static, std::format, wchar |
| Mutagen.Bethesda.Starfield | 0.54.0; resolved through NuGet |
| .NET SDK | 10.0.401 |
| XMake | 3.0.9 |
| Compiler/shell | Visual Studio C++ tools; PowerShell7; Windows x64 |

Game and Creation Kit files are separately obtained licensed inputs. Prepare
Papyrus imports with scripts/prepare_papyrus_sources.ps1 and install the matching
Cassiopeia DLL/PEX/PSC set separately. Nothing here redistributes those inputs.

CommonLib sources and licenses are included under external/. The spdlog source
ZIP under upstream/ has SHA-256
3D25808D2FC4DB86621A46855800C99AB5734999B61C4CBF9470EDF631555397.
Optional CommonLib INI/JSON/TOML/Xbyak packages are disabled.

The SDK helper selects the exact pin from .cache/dotnet-10.0.401, an explicit
CPOI_DOTNET_ROOT, DOTNET_ROOT or PATH. A newer SDK alone does not satisfy the pin.
See README.md for library Git setup and guarded build commands.