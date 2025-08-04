#include "ProcTerrainPreset.h"

using namespace ProcTerrainPresets;

static void FillDowntownRuins(FProcTerrainPresetDefinition& D)
{
    D.Width = 2049;
    D.Height = 2049;
    D.Seed = 2077;

    D.Fbm.bUseSimplex = false; // perlin
    D.Fbm.Scale = 600.f;
    D.Fbm.Octaves = 9;
    D.Fbm.Persistence = 0.55f;
    D.Fbm.Lacunarity = 2.1f;

    D.bThermalEnabled = true;
    D.Thermal.Iterations = 4;
    D.Thermal.DiffusionRate = 0.01f;

    D.bHydraulicEnabled = true;
    D.Hydraulic.NumDroplets = 150000;
    D.Hydraulic.ErosionRate = 0.05f;
    // remaining parameters keep defaults
}

static void FillCrystalline(FProcTerrainPresetDefinition& D)
{
    D.Width = 4097;
    D.Height = 4097;
    D.Seed = 8008;

    D.Fbm.bUseSimplex = false; // perlin
    D.Fbm.Scale = 800.f;
    D.Fbm.Octaves = 8;
    D.Fbm.Persistence = 0.7f;
    D.Fbm.Lacunarity = 2.2f;

    D.bThermalEnabled = false;
    D.bHydraulicEnabled = true;
    D.Hydraulic.NumDroplets = 200000;
    D.Hydraulic.ErosionRate = 0.3f;
    D.Hydraulic.DepositionRate = 0.0f;
}

static void FillMutatedSwamp(FProcTerrainPresetDefinition& D)
{
    D.Width = 4097;
    D.Height = 4097;
    D.Seed = 65000000;

    D.Fbm.bUseSimplex = true; // simplex
    D.Fbm.Scale = 1800.f;
    D.Fbm.Octaves = 6;
    D.Fbm.Persistence = 0.35f;
    D.Fbm.Lacunarity = 2.f;

    D.bHydraulicEnabled = true;
    D.Hydraulic.NumDroplets = 1200000;
    D.Hydraulic.ErosionRate = 0.1f;
    D.Hydraulic.DepositionRate = 0.35f;
}

static void FillBadlands(FProcTerrainPresetDefinition& D)
{
    D.Width = 4097;
    D.Height = 4097;
    D.Seed = 1986;

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
}

static void FillOldWorld(FProcTerrainPresetDefinition& D)
{
    D.Width = 2049;
    D.Height = 2049;
    D.Seed = 1066;

    D.Fbm.bUseSimplex = true;
    D.Fbm.Scale = 1400.f;
    D.Fbm.Octaves = 8;
    D.Fbm.Persistence = 0.4f;
    D.Fbm.Lacunarity = 2.f;

    D.bThermalEnabled = true;
    D.Thermal.Iterations = 15;
    D.Thermal.DiffusionRate = 0.02f;

    D.bHydraulicEnabled = true;
    D.Hydraulic.NumDroplets = 300000;
    D.Hydraulic.ErosionRate = 0.05f;
}

static void FillGothic(FProcTerrainPresetDefinition& D)
{
    D.Width = 2049;
    D.Height = 2049;
    D.Seed = 1888;

    D.Fbm.bUseSimplex = false;
    D.Fbm.Scale = 700.f;
    D.Fbm.Octaves = 9;
    D.Fbm.Persistence = 0.6f;
    D.Fbm.Lacunarity = 2.3f;

    D.bThermalEnabled = true;
    D.Thermal.Iterations = 3;
    D.bHydraulicEnabled = false;
}

static void FillMangrove(FProcTerrainPresetDefinition& D)
{
    D.Width = 4097;
    D.Height = 4097;
    D.Seed = 1619;

    D.Fbm.bUseSimplex = true;
    D.Fbm.Scale = 1500.f;
    D.Fbm.Octaves = 5;
    D.Fbm.Persistence = 0.3f;

    D.bThermalEnabled = false;
    D.bHydraulicEnabled = true;
    D.Hydraulic.NumDroplets = 1000000;
    D.Hydraulic.ErosionRate = 0.1f;
    D.Hydraulic.DepositionRate = 0.3f;
}

static void FillProving(FProcTerrainPresetDefinition& D)
{
    D.Width = 1025;
    D.Height = 1025;
    D.Seed = 2025;

    D.Fbm.bUseSimplex = true;
    D.Fbm.Scale = 800.f;
    D.Fbm.Octaves = 6;
    D.Fbm.Persistence = 0.45f;

    D.bThermalEnabled = true;
    D.Thermal.Iterations = 5;
    D.Thermal.DiffusionRate = 0.01f;

    D.bHydraulicEnabled = true;
    D.Hydraulic.NumDroplets = 50000;
    D.Hydraulic.ErosionRate = 0.1f;
    D.Hydraulic.DepositionRate = 0.2f;
}

static void FillArena(FProcTerrainPresetDefinition& D)
{
    D.Width = 513;
    D.Height = 513;
    D.Seed = 1111;

    D.Fbm.bUseSimplex = true;
    D.Fbm.Scale = 400.f;
    D.Fbm.Octaves = 5;
    D.Fbm.Persistence = 0.5f;

    D.bThermalEnabled = true;
    D.Thermal.Iterations = 3;

    D.bHydraulicEnabled = true;
    D.Hydraulic.NumDroplets = 15000;
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
