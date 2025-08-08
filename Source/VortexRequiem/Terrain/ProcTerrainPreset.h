#pragma once

#include "CoreMinimal.h"
#include "ProcTerrain.h"
#include "TerrainTypes.h" // for ETerrainPreset

// ----------------------------------------------------------------------------
// Splat map definitions mirroring Python generator rules
// ----------------------------------------------------------------------------

/** Per-layer rule thresholds for generating splat weights */
struct FSplatLayerRuleDef
{
    // Optional thresholds. If bHasX is false, that rule is ignored.
    bool bHasMinAltitude = false;
    float MinAltitude = 0.f;
    bool bHasMaxAltitude = false;
    float MaxAltitude = 1.f;

    bool bHasMinSlope = false;
    float MinSlope = 0.f;
    bool bHasMaxSlope = false;
    float MaxSlope = 1.f;
};

/** One layer inside a splat group */
struct FSplatLayerDef
{
    FName Name;                // e.g. "cracked_asphalt"
    bool bIsBaseLayer = false; // base layer weight = 1 - sum(other weights)
    bool bHasChannel = false;  // whether Channel is explicitly assigned
    TCHAR Channel = 'R';       // R/G/B/A if bHasChannel
    FSplatLayerRuleDef Rules;  // optional thresholds
};

/** A named RGBA splat map output, with multiple layers assigned to channels */
struct FSplatMapGroupDefinition
{
    FName GroupName;           // e.g. "urban_decay"
    TArray<FSplatLayerDef> Layers;
};

/** Top-level splat configuration for a preset */
struct FSplatMapRulesDefinition
{
    float BlendDistance = 0.05f;                  // blend width for thresholds
    bool bExportChannelsSeparately = true;        // for optional per-channel textures
    TArray<FSplatMapGroupDefinition> OutputGroups; // each becomes an RGBA mask
};

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

    // Optional splat map rules; if no groups, splat generation is skipped
    FSplatMapRulesDefinition Splat;
};

/** Utility to fetch algorithm settings for the various hard-coded terrain presets. */
namespace ProcTerrainPresets
{
    /** Returns true if the preset exists and fills out OutDef with the settings. */
    bool GetPreset(ETerrainPreset Preset, FProcTerrainPresetDefinition& OutDef);
}
