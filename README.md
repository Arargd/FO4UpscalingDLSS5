AI sloppening, (I'd recommend just using https://github.com/NIGos/dlss5-dx11-bridge )

# FO4Upscaling

[![Build](https://github.com/northaxosky/FO4Upscaling/actions/workflows/build.yml/badge.svg)](https://github.com/northaxosky/FO4Upscaling/actions/workflows/build.yml)

F4SE plugins for Fallout 4 providing frame generation and spatial upscaling.

Fork of [doodlum/fo4test](https://github.com/doodlum/fo4test) migrated to [DearModdingFO4 CommonLibF4](https://github.com/Dear-Modding-FO4/commonlibf4) for single-DLL multi-runtime support (OG/NG/AE).

## Plugins

| Plugin | DLL | Description |
|--------|-----|-------------|
| **AAAFrameGeneration** | `AAAFrameGeneration.dll` | Frame generation with FSR3 (AMD), DLSS-G (NVIDIA RTX 40+), and XeSS-FG (Intel/cross-vendor) via D3D11/D3D12 interop |
| **Upscaling** | `Upscaling.dll` | Spatial upscaling with AMD FSR3 and NVIDIA DLSS via Streamline. ENB compatible (native AA mode). |
| **MotionVectorFixes** | `MotionVectorFixes.dll` | Standalone fixes for broken motion vectors (weapon ghosting, menu ghosting, animated objects) |

## Frame Generation

The FrameGeneration plugin supports three backends, configurable via MCM or `FrameGeneration.ini`:

| Backend | Setting | GPU Requirement | Description |
|---------|---------|-----------------|-------------|
| **FSR3** | `iFrameGenType=0` | Any GPU | AMD FidelityFX frame interpolation |
| **DLSS-G** | `iFrameGenType=1` | NVIDIA RTX 40+ | NVIDIA DLSS Frame Generation via Streamline SDK |
| **XeSS-FG** | `iFrameGenType=2` | Any SM 6.4 GPU | Intel XeSS Frame Generation (multi-frame on Intel Arc) |

All backends use D3D11-to-D3D12 interop (game renders D3D11, frame gen runs D3D12). If the selected backend isn't available at runtime, it falls back to FSR3 automatically.

## ENB Compatibility

When ENBSeries is detected, the Upscaling plugin runs in **native AA mode** (DLAA for DLSS, Native AA for FSR). Sub-native quality modes are not available with ENB because the render target proxy system causes viewport compounding through ENB's D3D11 wrapper pipeline. Native AA provides temporal anti-aliasing at full resolution — better quality than the game's built-in TAA with no performance cost from upscaling.

Frame generation works with ENB across all backends (FSR3, DLSS-G, XeSS-FG).

## Requirements

- [Visual Studio 2022](https://visualstudio.microsoft.com/) (Desktop C++ workload)
- [CMake 3.21+](https://cmake.org/)
- [vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` environment variable set
- [Git](https://git-scm.com/)

## User Requirements

- [Address Library for F4SE](https://www.nexusmods.com/fallout4/mods/47327)
- [Fallout 4 Script Extender (F4SE)](https://f4se.silverlock.org/)

## Build

```bash
git clone --recursive https://github.com/northaxosky/FO4Upscaling.git
cd FO4Upscaling
bash scripts/fetch-sdks.sh      # Download third-party runtime DLLs
cmake -S . --preset=default
cmake --build build --config Release
```

Output:
- `build/plugins/FrameGeneration/Release/AAAFrameGeneration.dll`
- `build/plugins/Upscaling/Release/Upscaling.dll`
- `build/plugins/MotionVectorFixes/Release/MotionVectorFixes.dll`

## SDK Runtime DLLs

Third-party runtime DLLs (NVIDIA Streamline, AMD FidelityFX, Intel XeSS) are not stored in git. Run `scripts/fetch-sdks.sh` after cloning to download them from official GitHub releases. Downloaded archives are verified against the SHA-256 hashes in `scripts/sdk-manifest.sh`.

| SDK | Version | DLLs | Source |
|-----|---------|------|--------|
| NVIDIA Streamline | v2.11.1 | sl.interposer, sl.dlss_g, sl.reflex, etc. | [GitHub release](https://github.com/NVIDIA-RTX/Streamline/releases/tag/v2.11.1) |
| AMD FidelityFX | v1.1.4 | amd_fidelityfx_dx12.dll | [GitHub release](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases/tag/v1.1.4) |
| Intel XeSS | submodule | libxess_fg.dll, libxell.dll | Copied from `extern/XeSS/bin/` |

The deploy script auto-fetches if DLLs are missing. Downloaded archives are cached in `.sdk-cache/`. The tracked `package/` tree contains mod assets (shaders, MCM config, mesh/texture overrides, and license files); SDK runtime DLLs are generated/staged files and are ignored by git.

## Scripts

Before using scripts, copy `scripts/.env.example` to `scripts/.env` and configure paths for your setup:

```bash
cp scripts/.env.example scripts/.env
# Edit scripts/.env with your MO2 mod folder, game paths, etc.
```

### Deploy to MO2 mod folder

```bash
./scripts/deploy.sh build    # Build + deploy DLLs and shaders
./scripts/deploy.sh deploy   # Deploy only (skip build)
```

### Automated test

```bash
./scripts/test.sh            # Build, deploy, launch game, monitor logs, screenshot, cleanup
```

Results are saved to `test-results/<timestamp>/` with logs, screenshots, and a pass/fail summary.

## Project Structure

```
plugins/
  FrameGeneration/       # AAAFrameGeneration target (FSR3 + DLSS-G + XeSS-FG frame gen)
  Upscaling/             # Upscaling target (FSR3 + DLSS spatial upscaling)
  MotionVectorFixes/     # Standalone motion vector bug fixes
include/                 # Shared headers (PCH, ENB SDK, Detours)
extern/
  CommonLibF4/           # DearModdingFO4 (address-independent, all runtimes)
  FidelityFX-SDK/        # AMD FSR3 SDK
  Streamline/            # NVIDIA Streamline SDK v2.11.1 (DLSS, DLSS-G, Reflex)
  XeSS/                  # Intel XeSS SDK (XeSS-FG, XeLL)
package/
  F4SE/Plugins/          # Runtime files (HLSL shaders, INI configs)
  MCM/                   # Mod Configuration Menu definitions
cmake/
  Common.cmake           # Shared build config (configure_xse_plugin function)
scripts/
  .env.example           # Template for local path config
  fetch-sdks.sh          # Download third-party SDK runtime DLLs
  deploy.sh              # Build + deploy to MO2 test mod
  test.sh                # Automated game test pipeline
  compare-fg.sh          # A/B/C comparison: No FG vs FSR3 vs DLSS-G vs XeSS-FG
```

## Releases

Each release produces 3 separate distributable zips, one per plugin:

| Package | Contents |
|---------|----------|
| `MotionVectorFixes-vX.X.X.zip` | DLL |
| `Upscaling-vX.X.X.zip` | DLL + shaders + Streamline DLLs + MCM config + mesh overrides |
| `FrameGeneration-vX.X.X.zip` | DLL + shaders + Streamline + FidelityFX + XeSS DLLs + MCM config |
| `FO4Upscaling-Symbols-vX.X.X.zip` | Debug symbols for this project's DLLs only |

Each zip has the correct folder structure for MO2 — extract into your mod folder.

## License

MIT
