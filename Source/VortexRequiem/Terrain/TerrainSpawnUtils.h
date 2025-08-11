#pragma once

#include "CoreMinimal.h"

class UStaticMesh;

namespace TerrainSpawnUtils
{
    // Computes spawn locations on a mesh given slope and separation constraints.
    // Returns world-space spawn points.
    void CalculateSpawnPoints(
        UStaticMesh* StaticMesh,
        const FTransform& ActorToWorld,
        int32 NumPlayerStarts,
        float MaxSpawnSlopeInDegrees,
        float MinSpawnSeparation,
        UWorld* World,
        TArray<FVector>& OutSpawnPoints,
        bool bDrawDebug = false,
        float DebugSphereRadius = 100.f);
}


