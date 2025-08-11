#pragma once

#include "CoreMinimal.h"
#include "ProcTerrainPreset.h"

class UTexture2D;

namespace TerrainSplatUtils
{
    // Creates a PF_B8G8R8A8 transient texture from RGBA pixels (Mask-friendly: SRGB=false, TC_Masks)
    UTexture2D* CreateTextureRGBA8(int32 InWidth, int32 InHeight, const TArray<FColor>& Pixels, const FString& DebugName);

    // Generate splat map textures and metadata from height data and optional preset rule definition.
    // Outputs:
    // - OutGroupTextures: GroupName -> Generated RGBA texture
    // - OutChannelMap: GroupName -> (LayerName -> ChannelIndex [0..3])
    // - OutGroups: list of detected group names
    // - OutLayers: list of detected layer names
    void GenerateSplatMaps(
        const TArray<uint8>& HeightData,
        int32 Width,
        int32 Height,
        const FProcTerrainPresetDefinition* OptionalPresetDef,
        TMap<FName, UTexture2D*>& OutGroupTextures,
        TMap<FName, TMap<FName, int32>>& OutChannelMap,
        TArray<FName>& OutGroups,
        TArray<FName>& OutLayers);
}


