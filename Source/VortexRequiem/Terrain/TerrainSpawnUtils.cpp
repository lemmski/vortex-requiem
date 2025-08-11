#include "TerrainSpawnUtils.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshDescription.h"
#include "MeshDescription.h"
#include "DrawDebugHelpers.h"

void TerrainSpawnUtils::CalculateSpawnPoints(
    UStaticMesh* StaticMesh,
    const FTransform& ActorToWorld,
    int32 NumPlayerStarts,
    float MaxSpawnSlopeInDegrees,
    float MinSpawnSeparation,
    UWorld* World,
    TArray<FVector>& OutSpawnPoints,
    bool bDrawDebug,
    float DebugSphereRadius)
{
    OutSpawnPoints.Empty();
    if (!StaticMesh || NumPlayerStarts <= 0)
    {
        return;
    }

    UStaticMeshDescription* Desc = StaticMesh->GetStaticMeshDescription(0);
    if (!Desc) return;

    const float MaxSlopeCosine = FMath::Cos(FMath::DegreesToRadians(MaxSpawnSlopeInDegrees));

    TArray<FVector> CandidateLocations;
    for (const FPolygonID PolygonID : Desc->Polygons().GetElementIDs())
    {
        TArray<FTriangleID> PolygonTriangles;
        Desc->GetPolygonTriangles(PolygonID, PolygonTriangles);
        for (const FTriangleID& TriID : PolygonTriangles)
        {
            TArray<FVertexInstanceID> VertexInstances;
            Desc->GetTriangleVertexInstances(TriID, VertexInstances);
            FVector V0 = Desc->GetVertexPosition(Desc->GetVertexInstanceVertex(VertexInstances[0]));
            FVector V1 = Desc->GetVertexPosition(Desc->GetVertexInstanceVertex(VertexInstances[1]));
            FVector V2 = Desc->GetVertexPosition(Desc->GetVertexInstanceVertex(VertexInstances[2]));

            FVector LocalNormal = FVector::CrossProduct(V2 - V0, V1 - V0).GetSafeNormal();
            FVector WorldNormal = ActorToWorld.TransformVectorNoScale(LocalNormal).GetSafeNormal();
            if (WorldNormal.Z < 0) WorldNormal = -WorldNormal;

            if (WorldNormal.Z >= MaxSlopeCosine)
            {
                CandidateLocations.Add((V0 + V1 + V2) / 3.0f);
            }
        }
    }

    if (CandidateLocations.Num() == 0)
    {
        return;
    }

    for (int32 i = 0; i < CandidateLocations.Num(); ++i)
    {
        int32 j = FMath::RandRange(i, CandidateLocations.Num() - 1);
        CandidateLocations.Swap(i, j);
    }

    for (const FVector& LocalCandidate : CandidateLocations)
    {
        if (OutSpawnPoints.Num() >= NumPlayerStarts) break;
        const FVector WorldCandidate = ActorToWorld.TransformPosition(LocalCandidate);
        bool bTooClose = false;
        for (const FVector& ExistingSpawn : OutSpawnPoints)
        {
            if (FVector::DistSquared(WorldCandidate, ExistingSpawn) < FMath::Square(MinSpawnSeparation))
            {
                bTooClose = true; break;
            }
        }
        if (!bTooClose)
        {
            OutSpawnPoints.Add(WorldCandidate);
            if (bDrawDebug && World)
            {
                DrawDebugSphere(World, WorldCandidate, DebugSphereRadius, 12, FColor::Green, true, -1, 0, 5.f);
            }
        }
    }
}


