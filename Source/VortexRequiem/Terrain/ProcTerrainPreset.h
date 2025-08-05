#pragma once

#include "CoreMinimal.h"
#include "ProcTerrain.h"
#include "TerrainTypes.h" // for ETerrainPreset

struct FProcTerrainPresetDefinition
{
    int32 Width = 1025;
    int32 Height = 1025;
    int32 Seed = 1337;
    float SeaLevel = 0.5f; // reserved for future use

    // Recommended world scaling for this preset
    float DefaultXYScale = 100.f;
    float DefaultZScale = 10000.f;

    FFBMSettings Fbm;
    // Optional post-process redistribution of heights
    float RedistributionExp = 1.f; // 1 = disabled

    bool bThermalEnabled = true;
    FThermalSettings Thermal;
    bool bHydraulicEnabled = true;
    FHydraulicSettings Hydraulic;
};

/** Utility to fetch algorithm settings for the various hard-coded terrain presets. */
namespace ProcTerrainPresets
{
    /** Returns true if the preset exists and fills out OutDef with the settings. */
    bool GetPreset(ETerrainPreset Preset, FProcTerrainPresetDefinition& OutDef);
}
