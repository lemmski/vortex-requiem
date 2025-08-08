#include "ProcTerrainPreset.h"

using namespace ProcTerrainPresets;

static void FillDowntownRuins(FProcTerrainPresetDefinition& D)
{
    D.Width = 2049;
    D.Height = 2049;
    D.Seed = 2077;
    D.DefaultXYScale = 100.f;
    D.DefaultZScale = 25000.f;

    D.Fbm.bUseSimplex = false; // perlin
    D.Fbm.Scale = 600.f;
    D.Fbm.Octaves = 9;
    D.Fbm.Persistence = 0.55f;
    D.Fbm.Lacunarity = 2.1f;
    D.Fbm.WarpStrength = 10.f;

    D.bThermalEnabled = true;
    D.Thermal.Iterations = 4;
    D.Thermal.DiffusionRate = 0.01f;

    D.bHydraulicEnabled = true;
    D.Hydraulic.NumDroplets = 150000;
    D.Hydraulic.ErosionRate = 0.05f;
    // remaining parameters keep defaults

    // Splat: urban decay
    D.Splat.BlendDistance = 0.04f;
    D.Splat.bExportChannelsSeparately = true;
    {
        FSplatMapGroupDefinition Group; Group.GroupName = TEXT("urban_decay");
        {
            FSplatLayerDef Base; Base.Name = TEXT("cracked_asphalt"); Base.bIsBaseLayer = true; Group.Layers.Add(Base);
        }
        {
            FSplatLayerDef L; L.Name = TEXT("rubble_and_dust"); L.bHasChannel = true; L.Channel = 'R';
            L.Rules.bHasMinSlope = true; L.Rules.MinSlope = 0.25f; Group.Layers.Add(L);
        }
        {
            FSplatLayerDef L; L.Name = TEXT("weedy_overgrowth"); L.bHasChannel = true; L.Channel = 'G';
            L.Rules.bHasMaxSlope = true; L.Rules.MaxSlope = 0.15f; Group.Layers.Add(L);
        }
        D.Splat.OutputGroups.Add(Group);
    }
}

static void FillCrystalline(FProcTerrainPresetDefinition& D)
{
    D.Width = 4097;
    D.Height = 4097;
    D.Seed = 8008;
    D.DefaultXYScale = 100.f;
    D.DefaultZScale = 60000.f;

    D.Fbm.bUseSimplex = false; // perlin
    D.Fbm.Scale = 800.f;
    D.Fbm.Octaves = 8;
    D.Fbm.Persistence = 0.7f;
    D.Fbm.Lacunarity = 2.2f;
    D.RedistributionExp = 3.8f;

    D.bThermalEnabled = false;
    D.bHydraulicEnabled = true;
    D.Hydraulic.NumDroplets = 200000;
    D.Hydraulic.ErosionRate = 0.3f;
    D.Hydraulic.DepositionRate = 0.0f;

    D.Splat.BlendDistance = 0.02f;
    D.Splat.bExportChannelsSeparately = true;
    {
        FSplatMapGroupDefinition Group; Group.GroupName = TEXT("alien_biomass");
        {
            FSplatLayerDef Base; Base.Name = TEXT("toxic_ground_sludge"); Base.bIsBaseLayer = true; Group.Layers.Add(Base);
        }
        {
            FSplatLayerDef L; L.Name = TEXT("crystalline_growth"); L.bHasChannel = true; L.Channel = 'R';
            L.Rules.bHasMinSlope = true; L.Rules.MinSlope = 0.4f; Group.Layers.Add(L);
        }
        {
            FSplatLayerDef L; L.Name = TEXT("resonance_dust"); L.bHasChannel = true; L.Channel = 'G';
            L.Rules.bHasMaxSlope = true; L.Rules.MaxSlope = 0.1f; Group.Layers.Add(L);
        }
        D.Splat.OutputGroups.Add(Group);
    }
}

static void FillMutatedSwamp(FProcTerrainPresetDefinition& D)
{
    D.Width = 4097;
    D.Height = 4097;
    D.Seed = 65000000;
    D.DefaultXYScale = 100.f;
    D.DefaultZScale = 8000.f;

    D.Fbm.bUseSimplex = true; // simplex
    D.Fbm.Scale = 1800.f;
    D.Fbm.Octaves = 6;
    D.Fbm.Persistence = 0.35f;
    D.Fbm.Lacunarity = 2.f;
    D.RedistributionExp = 1.7f;

    D.bHydraulicEnabled = true;
    D.Hydraulic.NumDroplets = 1200000;
    D.Hydraulic.ErosionRate = 0.1f;
    D.Hydraulic.DepositionRate = 0.35f;

    D.Splat.BlendDistance = 0.03f;
    D.Splat.bExportChannelsSeparately = true;
    {
        FSplatMapGroupDefinition Group; Group.GroupName = TEXT("swamp_layers");
        {
            FSplatLayerDef Base; Base.Name = TEXT("dense_jungle_floor"); Base.bIsBaseLayer = true; Group.Layers.Add(Base);
        }
        {
            FSplatLayerDef L; L.Name = TEXT("deep_mud"); L.bHasChannel = true; L.Channel = 'R';
            L.Rules.bHasMaxAltitude = true; L.Rules.MaxAltitude = 0.61f; Group.Layers.Add(L);
        }
        {
            FSplatLayerDef L; L.Name = TEXT("algae_scum"); L.bHasChannel = true; L.Channel = 'G';
            L.Rules.bHasMaxAltitude = true; L.Rules.MaxAltitude = 0.60f;
            L.Rules.bHasMaxSlope = true; L.Rules.MaxSlope = 0.05f; Group.Layers.Add(L);
        }
        D.Splat.OutputGroups.Add(Group);
    }
}

static void FillBadlands(FProcTerrainPresetDefinition& D)
{
    D.Width = 4097;
    D.Height = 4097;
    D.Seed = 1986;
    D.DefaultXYScale = 100.f;
    D.DefaultZScale = 40000.f;

    D.Fbm.bUseSimplex = false;
    D.Fbm.Scale = 1200.f;
    D.Fbm.Octaves = 7;
    D.Fbm.Persistence = 0.45f;
    D.Fbm.Lacunarity = 2.1f;

    D.bThermalEnabled = true;
    D.Thermal.Iterations = 12;
    D.Thermal.DiffusionRate = 0.008f;

    D.bHydraulicEnabled = true;
    D.Hydraulic.NumDroplets = 250000;
    D.Hydraulic.ErosionRate = 0.2f;

    D.Splat.BlendDistance = 0.04f;
    D.Splat.bExportChannelsSeparately = true;
    {
        FSplatMapGroupDefinition Group; Group.GroupName = TEXT("badlands_strata");
        {
            FSplatLayerDef Base; Base.Name = TEXT("gravel_base"); Base.bIsBaseLayer = true; Group.Layers.Add(Base);
        }
        {
            FSplatLayerDef L; L.Name = TEXT("irradiated_glass"); L.bHasChannel = true; L.Channel = 'R';
            L.Rules.bHasMinAltitude = true; L.Rules.MinAltitude = 0.85f;
            L.Rules.bHasMinSlope = true; L.Rules.MinSlope = 0.3f; Group.Layers.Add(L);
        }
        {
            FSplatLayerDef L; L.Name = TEXT("red_clay"); L.bHasChannel = true; L.Channel = 'G';
            L.Rules.bHasMinAltitude = true; L.Rules.MinAltitude = 0.4f;
            L.Rules.bHasMaxAltitude = true; L.Rules.MaxAltitude = 0.7f; Group.Layers.Add(L);
        }
        {
            FSplatLayerDef L; L.Name = TEXT("yellow_sulfur"); L.bHasChannel = true; L.Channel = 'B';
            L.Rules.bHasMaxAltitude = true; L.Rules.MaxAltitude = 0.4f;
            L.Rules.bHasMaxSlope = true; L.Rules.MaxSlope = 0.2f; Group.Layers.Add(L);
        }
        D.Splat.OutputGroups.Add(Group);
    }
}

static void FillOldWorld(FProcTerrainPresetDefinition& D)
{
    D.Width = 2049;
    D.Height = 2049;
    D.Seed = 1066;
    D.DefaultXYScale = 100.f;
    D.DefaultZScale = 15000.f;

    D.Fbm.bUseSimplex = true;
    D.Fbm.Scale = 1400.f;
    D.Fbm.Octaves = 8;
    D.Fbm.Persistence = 0.4f;
    D.Fbm.Lacunarity = 2.f;
    D.Fbm.WarpStrength = 40.f;
    D.Fbm.WarpScale = 200.f;

    D.bThermalEnabled = true;
    D.Thermal.Iterations = 15;
    D.Thermal.DiffusionRate = 0.02f;

    D.bHydraulicEnabled = true;
    D.Hydraulic.NumDroplets = 300000;
    D.Hydraulic.ErosionRate = 0.05f;

    D.Splat.BlendDistance = 0.12f;
    D.Splat.bExportChannelsSeparately = true;
    {
        FSplatMapGroupDefinition Group; Group.GroupName = TEXT("temperate_forest");
        {
            FSplatLayerDef Base; Base.Name = TEXT("meadow_grass"); Base.bIsBaseLayer = true; Group.Layers.Add(Base);
        }
        {
            FSplatLayerDef L; L.Name = TEXT("forest_loam"); L.bHasChannel = true; L.Channel = 'R';
            L.Rules.bHasMaxSlope = true; L.Rules.MaxSlope = 0.2f; Group.Layers.Add(L);
        }
        {
            FSplatLayerDef L; L.Name = TEXT("exposed_rock"); L.bHasChannel = true; L.Channel = 'G';
            L.Rules.bHasMinSlope = true; L.Rules.MinSlope = 0.5f; Group.Layers.Add(L);
        }
        {
            FSplatLayerDef L; L.Name = TEXT("ancient_cobblestone"); L.bHasChannel = true; L.Channel = 'B';
            L.Rules.bHasMinSlope = true; L.Rules.MinSlope = 0.1f;
            L.Rules.bHasMaxSlope = true; L.Rules.MaxSlope = 0.25f; Group.Layers.Add(L);
        }
        D.Splat.OutputGroups.Add(Group);
    }
}

static void FillGothic(FProcTerrainPresetDefinition& D)
{
    D.Width = 2049;
    D.Height = 2049;
    D.Seed = 1888;
    D.DefaultXYScale = 100.f;
    D.DefaultZScale = 35000.f;

    D.Fbm.bUseSimplex = false;
    D.Fbm.Scale = 700.f;
    D.Fbm.Octaves = 9;
    D.Fbm.Persistence = 0.6f;
    D.Fbm.Lacunarity = 2.3f;
    D.RedistributionExp = 2.2f;

    D.bThermalEnabled = true;
    D.Thermal.Iterations = 3;
    D.bHydraulicEnabled = false;

    D.Splat.BlendDistance = 0.05f;
    D.Splat.bExportChannelsSeparately = true;
    {
        FSplatMapGroupDefinition Group; Group.GroupName = TEXT("main");
        {
            FSplatLayerDef Base; Base.Name = TEXT("corrupted_earth"); Base.bIsBaseLayer = true; Group.Layers.Add(Base);
        }
        {
            FSplatLayerDef L; L.Name = TEXT("sharp_shale"); L.bHasChannel = true; L.Channel = 'R';
            L.Rules.bHasMinSlope = true; L.Rules.MinSlope = 0.4f; Group.Layers.Add(L);
        }
        {
            FSplatLayerDef L; L.Name = TEXT("grave_dust"); L.bHasChannel = true; L.Channel = 'G';
            L.Rules.bHasMaxSlope = true; L.Rules.MaxSlope = 0.1f; Group.Layers.Add(L);
        }
        D.Splat.OutputGroups.Add(Group);
    }
}

static void FillMangrove(FProcTerrainPresetDefinition& D)
{
    D.Width = 4097;
    D.Height = 4097;
    D.Seed = 1619;
    D.DefaultXYScale = 100.f;
    D.DefaultZScale = 6000.f;

    D.Fbm.bUseSimplex = true;
    D.Fbm.Scale = 1500.f;
    D.Fbm.Octaves = 5;
    D.Fbm.Persistence = 0.3f;
    D.RedistributionExp = 1.8f;

    D.bThermalEnabled = false;
    D.bHydraulicEnabled = true;
    D.Hydraulic.NumDroplets = 1000000;
    D.Hydraulic.ErosionRate = 0.1f;
    D.Hydraulic.DepositionRate = 0.3f;

    D.Splat.BlendDistance = 0.02f;
    D.Splat.bExportChannelsSeparately = true;
    {
        FSplatMapGroupDefinition Group; Group.GroupName = TEXT("main");
        {
            FSplatLayerDef Base; Base.Name = TEXT("wet_jungle_floor"); Base.bIsBaseLayer = true; Group.Layers.Add(Base);
        }
        {
            FSplatLayerDef L; L.Name = TEXT("sandbar"); L.bHasChannel = true; L.Channel = 'R';
            L.Rules.bHasMinAltitude = true; L.Rules.MinAltitude = 0.66f;
            L.Rules.bHasMaxAltitude = true; L.Rules.MaxAltitude = 0.68f; Group.Layers.Add(L);
        }
        {
            FSplatLayerDef L; L.Name = TEXT("mudflats"); L.bHasChannel = true; L.Channel = 'G';
            L.Rules.bHasMaxAltitude = true; L.Rules.MaxAltitude = 0.66f; Group.Layers.Add(L);
        }
        D.Splat.OutputGroups.Add(Group);
    }
}

static void FillProving(FProcTerrainPresetDefinition& D)
{
    D.Width = 1025;
    D.Height = 1025;
    D.Seed = 2025;
    D.DefaultXYScale = 100.f;
    D.DefaultZScale = 10000.f;

    D.Fbm.bUseSimplex = true;
    D.Fbm.Scale = 800.f;
    D.Fbm.Octaves = 6;
    D.Fbm.Persistence = 0.45f;
    D.Fbm.WarpStrength = 15.f;
    D.Fbm.WarpScale = 100.f;

    D.bThermalEnabled = true;
    D.Thermal.Iterations = 5;
    D.Thermal.DiffusionRate = 0.01f;

    D.bHydraulicEnabled = true;
    D.Hydraulic.NumDroplets = 50000;
    D.Hydraulic.ErosionRate = 0.1f;
    D.Hydraulic.DepositionRate = 0.2f;

    D.Splat.BlendDistance = 0.10f;
    D.Splat.bExportChannelsSeparately = true;
    {
        FSplatMapGroupDefinition Group; Group.GroupName = TEXT("main");
        {
            FSplatLayerDef Base; Base.Name = TEXT("compacted_dirt"); Base.bIsBaseLayer = true; Group.Layers.Add(Base);
        }
        {
            FSplatLayerDef L; L.Name = TEXT("loose_gravel"); L.bHasChannel = true; L.Channel = 'R';
            L.Rules.bHasMinSlope = true; L.Rules.MinSlope = 0.2f; Group.Layers.Add(L);
        }
        D.Splat.OutputGroups.Add(Group);
    }
}

static void FillArena(FProcTerrainPresetDefinition& D)
{
    D.Width = 513;
    D.Height = 513;
    D.Seed = 1111;
    D.DefaultXYScale = 50.f;
    D.DefaultZScale = 2000.f;

    D.Fbm.bUseSimplex = true;
    D.Fbm.Scale = 400.f;
    D.Fbm.Octaves = 5;
    D.Fbm.Persistence = 0.5f;
    D.Fbm.WarpStrength = 10.f;
    D.Fbm.WarpScale = 80.f;

    D.bThermalEnabled = true;
    D.Thermal.Iterations = 3;

    D.bHydraulicEnabled = true;
    D.Hydraulic.NumDroplets = 15000;

    D.Splat.BlendDistance = 0.10f;
    D.Splat.bExportChannelsSeparately = true;
    {
        FSplatMapGroupDefinition Group; Group.GroupName = TEXT("main");
        {
            FSplatLayerDef Base; Base.Name = TEXT("sand"); Base.bIsBaseLayer = true; Group.Layers.Add(Base);
        }
        {
            FSplatLayerDef L; L.Name = TEXT("packed_earth"); L.bHasChannel = true; L.Channel = 'R';
            L.Rules.bHasMinSlope = true; L.Rules.MinSlope = 0.15f; Group.Layers.Add(L);
        }
        D.Splat.OutputGroups.Add(Group);
    }
}

bool ProcTerrainPresets::GetPreset(ETerrainPreset Preset, FProcTerrainPresetDefinition& OutDef)
{
    switch (Preset)
    {
        case ETerrainPreset::DowntownRuins:             FillDowntownRuins(OutDef); return true;
        case ETerrainPreset::CrystallineBloomfallZone:  FillCrystalline(OutDef);   return true;
        case ETerrainPreset::MutatedSwamplands:         FillMutatedSwamp(OutDef);  return true;
        case ETerrainPreset::IrradiatedBadlands:        FillBadlands(OutDef);      return true;
        case ETerrainPreset::OldWorldAnomaly:           FillOldWorld(OutDef);      return true;
        case ETerrainPreset::GothicCathedralApproach:   FillGothic(OutDef);        return true;
        case ETerrainPreset::MangroveDeltaFull:         FillMangrove(OutDef);      return true;
        case ETerrainPreset::ProvingGroundsSmall:       FillProving(OutDef);       return true;
        case ETerrainPreset::ArenaTiny513:              FillArena(OutDef);         return true;
        default: break;
    }
    return false;
}
