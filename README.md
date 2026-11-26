# SkinFlow

Simulated sweat for Skyrim Special Edition / Anniversary Edition. Sweat beads up at pores, grows, runs down the skin along the real direction of gravity for the actor's current pose, leaves a wet trail behind it and dries. It is drawn through Community Shaders' Advanced Skin.

## How it works

- **Surfaces in texture space.** Each simulated actor gets a small render target per body part, laid out in the mesh's own UV space. SkinFlow replays the game's skinned draw of that part into it, so the simulation follows the posed, animated body without doing any skinning of its own.
- **Gravity pass** (`Gravity.hlsl`): writes, for each texel, which way is downhill on the skin right now (from the posed normal, including the normal map) and how steep it is.
- **Flow pass** (`Flow.hlsl`, compute): sweat is added at a share of the texels (the pores). Water stays put until a bead is heavy enough for the slope, then hops texel to texel downhill, leaving a little film behind, and evaporates over time.
- **Look** (Community Shaders patch): Advanced Skin reads the water texture and draws round beads, running drops and the wet film into its own wetness layer. That way the lighting, reflections and subsurface scattering stay Community Shaders' own.

Every body part is sized so that one texel covers the same area of skin (`fCellSize`), so drops have the same size on the head as on the body.

## Repository layout

| Path | What |
|---|---|
| `plugin/` | The SKSE plugin `SkinFlow.dll` (C++23, CommonLibSSE-NG) and its runtime data: `SKSE/Plugins/SkinFlow.ini` and the shaders in `SKSE/Plugins/SkinFlow/` |
| `plugin/include/SkinFlowAPI.h` | The C API the renderer uses (`SkinFlow_GetSurface`) |
| `cs-patch/` | The patch to Community Shaders 1.9.1 (Advanced Skin binds the surfaces and draws the sweat) |
| `extern/CommonLibSSE-NG` | Submodule, the same commit Community Shaders 1.9.1 builds with |

The patched Community Shaders tree is the [`skinflow` branch of YoucefNabil/skyrim-community-shaders](https://github.com/YoucefNabil/skyrim-community-shaders/tree/skinflow).

## Building

Requirements: Visual Studio 2022 with the C++ desktop workload (it brings CMake, Ninja and vcpkg), Git.

```
git clone --recursive https://github.com/YoucefNabil/SkinFlow.git
SkinFlow\plugin\build.cmd
```

The DLL lands in `plugin/build/Release/SkinFlow.dll`. `VCPKG_ROOT` is used when set; otherwise the vcpkg that ships with Visual Studio.

Community Shaders: check out the `skinflow` branch of the fork above (or apply `cs-patch/skinflow-v1.9.1.patch` to the `v1.9.1` tag) and build it the usual way (`BuildRelease.bat`).

## Installing (from source)

Copy into the game's `Data` folder, or into a mod manager mod that overwrites Community Shaders:

- `SKSE/Plugins/SkinFlow.dll`, `SKSE/Plugins/SkinFlow.ini`, `SKSE/Plugins/SkinFlow/*.hlsl`
- from the patched Community Shaders build: `SKSE/Plugins/CommunityShaders.dll`, `Shaders/Lighting.hlsl`, `Shaders/Skin/Skin.hlsli`, and the rest of the Advanced Skin feature (`Shaders/Features/Skin.ini`, `Shaders/Skin/skin_detail_n.dds`)

Then clear Community Shaders' shader cache once.

## Settings

`SkinFlow.ini` holds every setting with a comment. With [SKSE Menu Framework](https://www.nexusmods.com/skyrimspecialedition/mods/120352) installed they can be tuned live: F1, then SkinFlow, then Sweat. Save writes them to the ini.
## License

GPL-3.0 with the modding exception (`LICENSE`, `EXCEPTIONS.md`), the same terms as Community Shaders. Third-party code: `THIRD-PARTY-NOTICES.md`.
