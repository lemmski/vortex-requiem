# Vortex Requiem

## Overview

Hardcore looter-shooter built with Unreal Engine 5.6 featuring procedural worlds, a dynamic day–night system, and multiplayer-ready terrain generation.

### Features

- **Procedural terrain**: Runtime heightmap or preset-based generation (FBM + thermal/hydraulic erosion), GPU mesh upload, collision, navmesh build, and cached meshes for fast retries (`ATerrainGen`).
- **Biome presets & main menu**: Select biomes from `ETerrainPreset` in the main menu; background music rotation and loading feedback (`UMainMenuWidget`).
- **Multiplayer-ready**: Server seeds and triggers generation; clients generate on replication and sync via multicast `OnAllClientsReady`. Shooter mode spawns players at computed terrain spawn points (`AShooterGameMode`).
- **Dynamic day–night**: Sun/moon control, smooth twilight, periodic skylight recapture, fog/cloud tweaks, and sky sphere updates (`ADayNightCycle`).
- **Modules**: Core game in `Source/VortexRequiem`, procedural helpers in `AlePCGModule`, third-party `FastNoiseLite.h`.

### Design pillars and roadmap (from feature plan)

- **Lethal & tactical combat**
  - **Trauma & Integrity System**: Vitality + per-limb health, status effects, armor integrity plates, complex treatments/Med-Bay, parry integration. (Planned)
  - **Enemy ecosystem**: Crystalline Chorus (node weak points) + Mutated Fauna; knowledge-driven counters; dynamic weaknesses via Resonance Cascade. (Planned)
- **Deep & tangible itemization**
  - **Assembly Bench**: Diegetic, physics-based weapon modding; tolerances and malfunctions; robust weapon customization backend. (Planned)
  - **Xenotech Reverse-Engineering**: Research fragments, salvage crafting, risky overclocking; inventory backend. (Planned)
  - **Provenance System**: Field-Stripped, Standard-Issue, Pristine-Forged tiers with visual wear. (Planned)
- **High-stakes progression & endgame**
  - **Praxis System**: Techniques, Core Implants (lost on death unless insured), Weapon Aptitude. (Planned)
  - **Bloomfall Cycle**: Time-limited PvPvE zone with elite foes and rare materials. (Planned)
  - **The Bastion**: Upgradable hideout modules (Generator, Med-Bay, Assembly Bench, Research Station, Intel Hub). (Planned)
- **A living, hostile world**
  - **Umbilical Protocol**: Deploy/extract loop with escalating threat and randomized extractions; uninsured loss on death. (Planned)
  - **Crystalline Resonance Cascade**: Per-run time/weather/enemy/terrain changes; Intel Hub preview. (Planned)
  - **Anomalous Incursions**: Rare themed events (e.g., Old World, Gothic). (Future)

### Project layout

- `Source/`
  - `VortexRequiem/`: Main C++ module (Terrain, UI, Variants, DayNightCycle).
  - `AlePCGModule/`: Procedural content generation utilities.
  - `ThirdParty/`: External headers (e.g., `FastNoiseLite.h`).
- `Content/`
  - `Blueprints/`, `Characters/`, `Weapons/`, `Levels/`, `UI/`, variants (`Variant_Horror`, `Variant_Shooter`).
- `Config/`: Engine and project settings.

### Build

- From Editor: open `VortexRequiem.uproject` with UE 5.6 and click Build/Play.
- From PowerShell (Windows):

```ps
& "E:/UnrealEngine/Engine/Build/BatchFiles/Build.bat" VortexRequiemEditor Win64 Development -Project="E:/Unreal Projects/VortexRequiem/VortexRequiem.uproject" -WaitMutex -FromMsBuild -NoLiveCoding -Progress
```

Adjust engine and project paths as needed.

### Run

- Open a map under `Content/Levels/` (e.g., `OldWorldAnomalyLvl`), place an `ATerrainGen` actor (or enable generate-on-begin-play), and press Play.
- For biome-driven generation, start from the main menu widget/level and choose a biome.

### Asset naming (prefixes)

- `BP_` Blueprint, `WBP_` UI widget, `M_` Material, `MI_` Material Instance, `MF_` Material Function, `T_` Texture, `SM_` Static Mesh, `SK_` Skeletal Mesh, `ABP_` Anim BP, `Lvl_` Level, `IA_` Input Action, `IMC_` Input Mapping.

### Notes

- Dependencies: `UMG`, `EnhancedInput`, `NavigationSystem`, `ImageWrapper`, `MeshDescription`, `StaticMeshDescription`, `ProceduralMeshComponent` (see `VortexRequiem.Build.cs`).
- License: TBD.
