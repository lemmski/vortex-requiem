#include "TerrainGen.h"
#include "StaticMeshDescription.h"
#include "MeshDescription.h"
#include "UObject/ConstructorHelpers.h"
#include "VortexRequiemGameInstance.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Engine/StaticMesh.h"
#include "Engine/CollisionProfile.h"
#include "PhysicsEngine/BodySetup.h"
#include "NavigationSystem.h"
#include "NavigationSystemTypes.h"
#include "ProcTerrain.h"
#include "ProcTerrainPreset.h"
#include "Blueprint/UserWidget.h"
#include "Kismet/GameplayStatics.h"
#include "GameFramework/Character.h"
#include "Components/CapsuleComponent.h"
#include "TextureResource.h"
#include "EngineUtils.h"
#include "TimerManager.h"
#include "DrawDebugHelpers.h"
#include "Engine/EngineTypes.h"
#include "Engine/OverlapResult.h"
#include "Net/UnrealNetwork.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionTextureSampleParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionAppendVector.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionClamp.h"
#include "Materials/MaterialExpressionConstant2Vector.h"
#include "Materials/MaterialExpressionSine.h"
#include "Materials/MaterialExpressionCosine.h"
#if WITH_EDITOR
#include "Materials/MaterialInstanceConstant.h"
#include "UObject/Package.h"
#include "AssetRegistry/AssetRegistryModule.h"
#endif
#if WITH_EDITOR
#include "AssetRegistry/AssetRegistryModule.h"
#endif
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionNormalize.h"
#include "Materials/MaterialExpressionAbs.h"
#include "Materials/MaterialExpressionConstant.h"
namespace
{
    static UMaterialInterface* GetDefaultSurfaceMaterial()
    {
        if (UMaterialInterface* M = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial")))
        {
            return M;
        }
        if (UMaterialInterface* Grid = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial")))
        {
            return Grid;
        }
        if (UMaterialInterface* Fallback = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineMaterials/DefaultTextMaterialOpaque.DefaultTextMaterialOpaque")))
        {
            return Fallback;
        }
        return nullptr;
    }
    struct FProcMeshCache
    {
        TArray<FVector> Vertices;
        TArray<int32> Triangles;
        TArray<FVector2D> UVs;
        bool bValid = false;
        FString Key;
    };
    static FProcMeshCache GTerrainCache;
}

ATerrainGen::ATerrainGen()
{
    PrimaryActorTick.bCanEverTick = false;
    bReplicates = true;
    bAlwaysRelevant = true; // Ensure terrain is always relevant for replication

    Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("ProcMesh"));
    Mesh->SetMobility(EComponentMobility::Static);
    SetRootComponent(Mesh);

    // No default base material; we'll auto-generate one in-editor if needed
    TerrainMaterial = nullptr;

    PngPath = TEXT("Content/Levels/OldWorldAnomalyLvl/old_world_anomaly_2k.png");
    HeightmapTexture = nullptr;
    XYScale = 10.f;
    ZScale = 10.f;
    TileQuads = 127;
    HeightTolerance = 5.f;

    Preset = ETerrainPreset::None;
    bGenerateOnBeginPlay = false;

    NumPlayerStarts = 10;
    MaxSpawnSlopeInDegrees = 25.0f;
    MinSpawnSeparation = 1000.0f;
    SpawnClearanceRadius = 100.0f;
    bUseLargeSpawnSpheres = false;

    CurrentState = EGenerationState::Idle;
    bTerrainReady = false;

    bApplySplatToMaterial = true;
    RuntimeMID = nullptr;

    // Preseed grouped materials map with known presets (empty layer maps; filled later)
    UpdateAllPresetLayerSlots();
}

void ATerrainGen::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);

    DOREPLIFETIME(ATerrainGen, Preset);
    DOREPLIFETIME(ATerrainGen, Seed);
    DOREPLIFETIME(ATerrainGen, XYScale);
    DOREPLIFETIME(ATerrainGen, ZScale);
    DOREPLIFETIME(ATerrainGen, SpawnPoints);
    DOREPLIFETIME(ATerrainGen, bTerrainReady);
}

void ATerrainGen::OnRep_Preset()
{
    // If a preset is replicated in a runtime session, clients should build the same terrain locally.
    if (!HasAuthority())
    {
        UE_LOG(LogTemp, Warning, TEXT("[CLIENT] TerrainGen: OnRep_Preset starting generation for replicated preset %d"), (int32)Preset);
        StartAsyncGeneration();
    }
}

void ATerrainGen::OnRep_Seed()
{
    UE_LOG(LogTemp, Warning, TEXT("[CLIENT] TerrainGen: Replicated Seed %d received. Starting generation."), Seed);
    if (bGenerateOnBeginPlay)
    {
        StartAsyncGeneration();
    }
}

void ATerrainGen::OnRep_SpawnPoints()
{
    UE_LOG(LogTemp, Warning, TEXT("[CLIENT] TerrainGen: Received %d spawn points from server"), SpawnPoints.Num());
    for (int32 i = 0; i < SpawnPoints.Num(); i++)
    {
        UE_LOG(LogTemp, VeryVerbose, TEXT("[CLIENT] TerrainGen: Spawn point %d: %s"), i, *SpawnPoints[i].ToString());
    }
}

void ATerrainGen::OnRep_TerrainReady()
{
    UE_LOG(LogTemp, Warning, TEXT("[CLIENT] TerrainGen: Terrain ready status changed to %s"), bTerrainReady ? TEXT("true") : TEXT("false"));
    // Late joiners: if server says ready but we don't have a mesh yet, generate now so local collision matches server
    if (!HasAuthority() && bTerrainReady)
    {
        const bool bHasMesh = Mesh && Mesh->GetStaticMesh() != nullptr;
        if (!bHasMesh && CurrentState == EGenerationState::Idle)
        {
            UE_LOG(LogTemp, Warning, TEXT("[CLIENT] TerrainGen: OnRep_TerrainReady detected no mesh. Generating now."));
            StartAsyncGeneration();
        }
    }
}

void ATerrainGen::GenerateTerrainFromPreset(ETerrainPreset NewPreset)
{
    // Prevent regeneration if we're already generating or if terrain is already ready with the same preset
    if (CurrentState != EGenerationState::Idle)
    {
        UE_LOG(LogTemp, Warning, TEXT("[%s] TerrainGen::GenerateTerrainFromPreset - Already generating terrain, ignoring request"),
            HasAuthority() ? TEXT("SERVER") : TEXT("CLIENT"));
        return;
    }

    if (bTerrainReady && Preset == NewPreset)
    {
        UE_LOG(LogTemp, Warning, TEXT("[%s] TerrainGen::GenerateTerrainFromPreset - Terrain already ready with same preset, ignoring request"),
            HasAuthority() ? TEXT("SERVER") : TEXT("CLIENT"));
        return;
    }

    Preset = NewPreset;
    Regenerate();
}

void ATerrainGen::Regenerate()
{
    if (GetWorld() && GetWorld()->IsGameWorld())
    {
        StartAsyncGeneration();
    }
    else
    {
        GenerateTerrain_Editor();
    }
}

void ATerrainGen::OnConstruction(const FTransform& Transform)
{
    Super::OnConstruction(Transform);
    if (GetWorld() && !GetWorld()->IsGameWorld())
    {
        // Prefill layer slots from preset in editor and expose all presets
        FProcTerrainPresetDefinition Def;
        if (Preset != ETerrainPreset::None && ProcTerrainPresets::GetPreset(Preset, Def))
        {
            UpdateLayerSlotsFromPreset(Def);
        }
        UpdateAllPresetLayerSlots();
        // Avoid regenerating the static mesh on every property change that doesn't affect geometry
        const FString NewKey = FString::Printf(TEXT("Preset:%d_%f_%f_%f"), static_cast<int32>(Preset), XYScale, ZScale, HeightTolerance);
        if (EditorLastCacheKey != NewKey)
        {
            EditorLastCacheKey = NewKey;
            GenerateTerrain_Editor();
        }
        else
        {
            // Only update material bindings and splats without rebuilding geometry
            GenerateSplatMaps(&Def);
            ApplyMaterialBindings(&Def);
        }
    }
}

void ATerrainGen::BeginPlay()
{
    Super::BeginPlay();

    UE_LOG(LogTemp, Warning, TEXT("[%s] TerrainGen::BeginPlay - bGenerateOnBeginPlay=%s, HasAuthority=%s"),
        HasAuthority() ? TEXT("SERVER") : TEXT("CLIENT"),
        bGenerateOnBeginPlay ? TEXT("true") : TEXT("false"),
        HasAuthority() ? TEXT("true") : TEXT("false"));

    if (bGenerateOnBeginPlay)
    {
        if (HasAuthority())
        {
            Seed = FMath::Rand();
            UE_LOG(LogTemp, Warning, TEXT("[SERVER] TerrainGen: Generated Seed %d. Starting generation."), Seed);
            if (Preset != ETerrainPreset::None)
            {
                FProcTerrainPresetDefinition Def;
                if (ProcTerrainPresets::GetPreset(Preset, Def))
                {
                    UpdateLayerSlotsFromPreset(Def);
                }
            }
            UpdateAllPresetLayerSlots();
            StartAsyncGeneration();
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("[CLIENT] TerrainGen: Waiting for replicated seed from server..."));
        }
    }
}

void ATerrainGen::StartAsyncGeneration()
{
    if (CurrentState != EGenerationState::Idle)
    {
        UE_LOG(LogTemp, Warning, TEXT("[%s] TerrainGen::StartAsyncGeneration called while already busy."),
            HasAuthority() ? TEXT("SERVER") : TEXT("CLIENT"));
        return;
    }

    UE_LOG(LogTemp, Warning, TEXT("[%s] TerrainGen::StartAsyncGeneration - Beginning terrain generation"),
        HasAuthority() ? TEXT("SERVER") : TEXT("CLIENT"));

    OnGenerationProgress.Broadcast(FText::FromString("Starting terrain generation..."));

    ensure(Mesh != nullptr);

    DisableActorPhysicsTemporarily();

    // Build cache key
    if (HeightmapTexture)
    {
        CurrentCacheKey = FString::Printf(TEXT("Tex:%s_%f_%f_%f"), *HeightmapTexture->GetPathName(), XYScale, ZScale, HeightTolerance);
    }
    else if (Preset != ETerrainPreset::None)
    {
        CurrentCacheKey = FString::Printf(TEXT("Preset:%d_%f_%f_%f"), static_cast<int32>(Preset), XYScale, ZScale, HeightTolerance);
    }
    else
    {
        CurrentCacheKey = FString::Printf(TEXT("File:%s_%f_%f_%f"), *PngPath, XYScale, ZScale, HeightTolerance);
    }

    if (GTerrainCache.bValid && GTerrainCache.Key == CurrentCacheKey)
    {
        OnGenerationProgress.Broadcast(FText::FromString("Using cached mesh..."));
        Vertices = GTerrainCache.Vertices;
        Triangles = GTerrainCache.Triangles;
        UVs = GTerrainCache.UVs;
        CurrentState = EGenerationState::UploadMesh;
    }
    else
    {
        CurrentState = EGenerationState::LoadHeightmap;
    }

    GetWorldTimerManager().SetTimer(GenerationProcessTimer, this, &ATerrainGen::ProcessGenerationStep, 0.01f, false);
}

void ATerrainGen::ProcessGenerationStep()
{
    switch (CurrentState)
    {
    case EGenerationState::LoadHeightmap:
        Step_LoadHeightmap();
        break;
    case EGenerationState::GenerateProcedural:
        Step_GenerateProcedural();
        break;
    case EGenerationState::CreateMesh:
        Step_CreateMesh();
        break;
    case EGenerationState::UploadMesh:
        Step_UploadMesh();
        break;
    case EGenerationState::WaitForCollision:
        Step_WaitForCollision();
        break;
    case EGenerationState::CalculateSpawnPoints:
        Step_CalculateSpawnPoints();
        break;
    case EGenerationState::BuildNavigation:
        Step_BuildNavigation();
        break;
    case EGenerationState::Finalize:
        Step_Finalize();
        break;
    default:
        CurrentState = EGenerationState::Idle;
        GetWorldTimerManager().ClearTimer(GenerationProcessTimer);
        break;
    }

    // Continue processing on the next frame if not idle
    if (CurrentState != EGenerationState::Idle)
    {
        GetWorldTimerManager().SetTimer(GenerationProcessTimer, this, &ATerrainGen::ProcessGenerationStep, 0.01f, false);
    }
}

void ATerrainGen::Step_LoadHeightmap()
{
    OnGenerationProgress.Broadcast(FText::FromString("Loading heightmap..."));

    bool bLoaded = false;

    if (Preset != ETerrainPreset::None)
    {
        CurrentState = EGenerationState::GenerateProcedural;
        return;
    }

    if (HeightmapTexture)
    {
        if (HeightmapTexture->GetPlatformData() && HeightmapTexture->GetPlatformData()->Mips.Num() > 0)
        {
            FTexture2DMipMap& Mip = HeightmapTexture->GetPlatformData()->Mips[0];
            HeightmapWidth = Mip.SizeX;
            HeightmapHeight = Mip.SizeY;
            HeightData.SetNumUninitialized(HeightmapWidth * HeightmapHeight);

            const FColor* Src = static_cast<const FColor*>(Mip.BulkData.LockReadOnly());
            for (int32 i = 0; i < HeightmapWidth * HeightmapHeight; ++i)
            {
                HeightData[i] = Src[i].R;
            }
            Mip.BulkData.Unlock();
            bLoaded = true;
        }
    }
    else
    {
        FString FullPath = PngPath;
        if (FPaths::IsRelative(PngPath))
        {
            FullPath = FPaths::Combine(FPaths::ProjectDir(), PngPath);
        }
        bLoaded = LoadHeightMapRaw(FullPath, HeightmapWidth, HeightmapHeight, HeightData);
    }

    if (!bLoaded || HeightmapWidth == 0 || HeightmapHeight == 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("Heightmap load failed, switching to procedural fallback."));
        CurrentState = EGenerationState::GenerateProcedural;
    }
    else
    {
        CurrentState = EGenerationState::CreateMesh;
    }
}

void ATerrainGen::Step_GenerateProcedural()
{
    OnGenerationProgress.Broadcast(FText::FromString("Generating procedural terrain..."));

    FProcTerrainPresetDefinition Def;
    if (Preset != ETerrainPreset::None && ProcTerrainPresets::GetPreset(Preset, Def))
    {
        HeightmapWidth = Def.Width;
        HeightmapHeight = Def.Height;
        FProcTerrain PT(HeightmapWidth, HeightmapHeight, Def.Seed);
        PT.GenerateFBM(Def.Fbm);
        if (Def.bThermalEnabled)  PT.ApplyThermal(Def.Thermal);
        if (Def.bHydraulicEnabled) PT.ApplyHydraulic(Def.Hydraulic);

        HeightData.SetNumUninitialized(HeightmapWidth * HeightmapHeight);
        for (int32 i = 0; i < HeightmapWidth * HeightmapHeight; ++i)
        {
            HeightData[i] = static_cast<uint8>(FMath::Clamp(PT.HeightMap[i] * 255.0f, 0.0f, 255.0f));
        }
    }
    else
    {
        // Fallback if preset is None or fails
        HeightmapWidth = 1024;
        HeightmapHeight = 1024;
        FProcTerrain PT(HeightmapWidth, HeightmapHeight, Seed);
        PT.GenerateFBM(FFBMSettings());
        PT.ApplyThermal(FThermalSettings());
        PT.ApplyHydraulic(FHydraulicSettings());

        HeightData.SetNumUninitialized(HeightmapWidth * HeightmapHeight);
        for (int32 i = 0; i < HeightmapWidth * HeightmapHeight; ++i)
        {
            HeightData[i] = static_cast<uint8>(FMath::Clamp(PT.HeightMap[i] * 255.0f, 0.0f, 255.0f));
        }
    }

    CurrentState = EGenerationState::CreateMesh;
}

void ATerrainGen::Step_CreateMesh()
{
    OnGenerationProgress.Broadcast(FText::FromString("Creating mesh geometry..."));

    Vertices.Empty();
    Triangles.Empty();
    UVs.Empty();

    if (HeightTolerance <= 0.f)
    {
        Vertices.SetNumUninitialized(HeightmapWidth * HeightmapHeight);
        UVs.SetNumUninitialized(HeightmapWidth * HeightmapHeight);
        const float Scale = ZScale / 255.f;
        const float HalfWidth = (HeightmapWidth - 1) * XYScale * 0.5f;
        const float HalfHeight = (HeightmapHeight - 1) * XYScale * 0.5f;

        for (int32 y = 0; y < HeightmapHeight; ++y)
        {
            for (int32 x = 0; x < HeightmapWidth; ++x)
            {
                int32 idx = y * HeightmapWidth + x;
                float h = HeightData[idx] * Scale;
                Vertices[idx] = FVector(x * XYScale - HalfWidth, y * XYScale - HalfHeight, h);
                UVs[idx] = FVector2D((float)x / (HeightmapWidth - 1), (float)y / (HeightmapHeight - 1));
            }
        }

        Triangles.Reserve((HeightmapWidth - 1) * (HeightmapHeight - 1) * 6);
        for (int32 y = 0; y < HeightmapHeight - 1; ++y)
        {
            for (int32 x = 0; x < HeightmapWidth - 1; ++x)
            {
                int32 i = y * HeightmapWidth + x;
                Triangles.Append({ i, i + 1, i + HeightmapWidth + 1,  i, i + HeightmapWidth + 1, i + HeightmapWidth });
            }
        }
    }
    else
    {
        const float Tol = HeightTolerance;
        TBitArray<> KeepRow(false, HeightmapHeight);
        TBitArray<> KeepCol(false, HeightmapWidth);
        const float Scale = ZScale / 255.f;
        for (int32 y = 0; y < HeightmapHeight; ++y)
        {
            uint8 minv = 255, maxv = 0;
            for (int32 x = 0; x < HeightmapWidth; ++x)
            {
                uint8 v = HeightData[y * HeightmapWidth + x];
                minv = FMath::Min(minv, v);
                maxv = FMath::Max(maxv, v);
            }
            if ((maxv - minv) * Scale > Tol) KeepRow[y] = true;
        }
        for (int32 x = 0; x < HeightmapWidth; ++x)
        {
            uint8 minv = 255, maxv = 0;
            for (int32 y = 0; y < HeightmapHeight; ++y)
            {
                uint8 v = HeightData[y * HeightmapWidth + x];
                minv = FMath::Min(minv, v);
                maxv = FMath::Max(maxv, v);
            }
            if ((maxv - minv) * Scale > Tol) KeepCol[x] = true;
        }
        KeepRow[0] = true; KeepRow[HeightmapHeight - 1] = true;
        KeepCol[0] = true; KeepCol[HeightmapWidth - 1] = true;

        TArray<int32> Rows, Cols;
        for (int32 y = 0; y < HeightmapHeight; ++y) if (KeepRow[y]) Rows.Add(y);
        for (int32 x = 0; x < HeightmapWidth; ++x) if (KeepCol[x]) Cols.Add(x);

        int32 NewH = Rows.Num();
        int32 NewW = Cols.Num();

        const float HalfWidth = (HeightmapWidth - 1) * XYScale * 0.5f;
        const float HalfHeight = (HeightmapHeight - 1) * XYScale * 0.5f;

        Vertices.SetNumUninitialized(NewW * NewH);
        UVs.SetNumUninitialized(NewW * NewH);
        for (int32 yi = 0; yi < NewH; ++yi)
        {
            for (int32 xi = 0; xi < NewW; ++xi)
            {
                int32 gy = Rows[yi], gx = Cols[xi];
                float h = HeightData[gy * HeightmapWidth + gx] * Scale;
                int32 idx = yi * NewW + xi;
                Vertices[idx] = FVector(gx * XYScale - HalfWidth, gy * XYScale - HalfHeight, h);
                UVs[idx] = FVector2D((float)gx / (HeightmapWidth - 1), (float)gy / (HeightmapHeight - 1));
            }
        }

        Triangles.Reserve((NewW - 1) * (NewH - 1) * 6);
        for (int32 y = 0; y < NewH - 1; ++y)
        {
            for (int32 x = 0; x < NewW - 1; ++x)
            {
                int32 i = y * NewW + x;
                Triangles.Append({ i, i + NewW + 1, i + 1,  i, i + NewW, i + NewW + 1 });
            }
        }
    }

    // Precompute splat maps if a preset with rules is active
    {
        FProcTerrainPresetDefinition Def;
        FProcTerrainPresetDefinition* DefPtr = nullptr;
        if (Preset != ETerrainPreset::None && ProcTerrainPresets::GetPreset(Preset, Def))
        {
            DefPtr = &Def;
        }
        GenerateSplatMaps(DefPtr);
    }

    // Compute per-vertex normals from triangle faces to avoid degenerate tangent bases
    Normals.SetNumZeroed(Vertices.Num());
    for (int32 ti = 0; ti + 2 < Triangles.Num(); ti += 3)
    {
        const int32 i0 = Triangles[ti + 0];
        const int32 i1 = Triangles[ti + 1];
        const int32 i2 = Triangles[ti + 2];
        const FVector& P0 = Vertices[i0];
        const FVector& P1 = Vertices[i1];
        const FVector& P2 = Vertices[i2];
        const FVector FaceN = FVector::CrossProduct(P2 - P0, P1 - P0);
        if (!FaceN.IsNearlyZero())
        {
            Normals[i0] += FaceN;
            Normals[i1] += FaceN;
            Normals[i2] += FaceN;
        }
    }
    for (FVector& N : Normals)
    {
        if (!N.Normalize())
        {
            N = FVector::UpVector;
        }
    }

    CurrentState = EGenerationState::UploadMesh;
}

void ATerrainGen::Step_UploadMesh()
{
    OnGenerationProgress.Broadcast(FText::FromString("Uploading mesh to GPU..."));

    GeneratedMesh = NewObject<UStaticMesh>(this, TEXT("GeneratedTerrainMesh"));
    GeneratedMesh->InitResources();
    GeneratedMesh->bAllowCPUAccess = true;

    // Set mobility to movable to allow for runtime updates
    Mesh->SetMobility(EComponentMobility::Movable);

    GeneratedMesh->SetLightingGuid();

    // Ensure at least one material slot exists BEFORE building, so LOD sections have a valid material index
    {
        UMaterialInterface* BaseMatForBuild = TerrainMaterial ? TerrainMaterial : GetDefaultSurfaceMaterial();
        FStaticMaterial BuildMat(BaseMatForBuild, FName("TerrainMaterial"));
        BuildMat.UVChannelData.bInitialized = true;
        GeneratedMesh->GetStaticMaterials().Reset();
        GeneratedMesh->GetStaticMaterials().Add(BuildMat);
    }

    UStaticMeshDescription* Desc = GeneratedMesh->CreateStaticMeshDescription();
    Desc->GetVertexInstanceUVs().SetNumChannels(1);
    // 1) Luo vertexit kerran
    TArray<FVertexID> VertexIDs;
    // vertices
    VertexIDs.SetNum(Vertices.Num());
    for (int32 i = 0; i < Vertices.Num(); ++i)
    {
        const FVertexID Vid = Desc->CreateVertex();
        Desc->SetVertexPosition(Vid, Vertices[i]);
        VertexIDs[i] = Vid;
    }

    // one polygon group
    FPolygonGroupID Pgid = Desc->CreatePolygonGroup();

    // triangles (per-corner vertex instances + UVs)
    for (int32 i = 0; i < Triangles.Num(); i += 3)
    {
        const int32 i0 = Triangles[i + 0];
        const int32 i1 = Triangles[i + 1];
        const int32 i2 = Triangles[i + 2];

        const FVertexInstanceID VI0 = Desc->CreateVertexInstance(VertexIDs[i0]);
        const FVertexInstanceID VI1 = Desc->CreateVertexInstance(VertexIDs[i1]);
        const FVertexInstanceID VI2 = Desc->CreateVertexInstance(VertexIDs[i2]);

        Desc->SetVertexInstanceUV(VI0, UVs[i0], 0);
        Desc->SetVertexInstanceUV(VI1, UVs[i1], 0);
        Desc->SetVertexInstanceUV(VI2, UVs[i2], 0);

        TArray<FEdgeID> NewEdges;
        Desc->CreateTriangle(Pgid, { VI0, VI1, VI2 }, NewEdges);
    }

#if WITH_EDITORONLY_DATA
    GeneratedMesh->SetNumSourceModels(1);
    FStaticMeshSourceModel& Src = GeneratedMesh->GetSourceModel(0);
    // Let the builder compute normals and tangents without MikkTSpace to reduce degeneracy issues
    Src.BuildSettings.bRecomputeNormals = true;
    Src.BuildSettings.bRecomputeTangents = true;
    Src.BuildSettings.bUseMikkTSpace = false;
    Src.BuildSettings.bRemoveDegenerates = true;
    // Valinnaiset valotuskartta‑UV:t
    // Src.BuildSettings.bGenerateLightmapUVs = true;
    // GeneratedMesh->LightMapCoordinateIndex = 1;
    // GeneratedMesh->LightMapResolution      = 256;
#endif

    GeneratedMesh->BuildFromStaticMeshDescriptions({ Desc });

    GeneratedMesh->PostEditChange();

    // Force bounds calculation after PostEditChange
    GeneratedMesh->CalculateExtendedBounds();

    if (GeneratedMesh->GetBodySetup())
    {
        GeneratedMesh->GetBodySetup()->CollisionTraceFlag = CTF_UseComplexAsSimple;
        GeneratedMesh->GetBodySetup()->bNeverNeedsCookedCollisionData = false;
        GeneratedMesh->GetBodySetup()->InvalidatePhysicsData();
        GeneratedMesh->GetBodySetup()->CreatePhysicsMeshes();
    }

    // Static material slot was already added before build; leave as-is

    Mesh->SetStaticMesh(GeneratedMesh);
    Mesh->UpdateBounds();
    Mesh->MarkRenderStateDirty();
    // Cache the generated mesh
    GTerrainCache.Vertices = Vertices;
    GTerrainCache.Triangles = Triangles;
    GTerrainCache.UVs = UVs;
    GTerrainCache.Key = CurrentCacheKey;
    GTerrainCache.bValid = true;
    Mesh->SetCanEverAffectNavigation(false); // Disable for now

    // Bind material and splat textures using the preset definition if present
    {
        FProcTerrainPresetDefinition Def;
        FProcTerrainPresetDefinition* DefPtr = nullptr;
        if (Preset != ETerrainPreset::None && ProcTerrainPresets::GetPreset(Preset, Def))
        {
            DefPtr = &Def;
        }
        ApplyMaterialBindings(DefPtr);
    }

    CurrentState = EGenerationState::WaitForCollision;
}

void ATerrainGen::Step_WaitForCollision()
{
    OnGenerationProgress.Broadcast(FText::FromString("Waiting for collision..."));

    if (GeneratedMesh && GeneratedMesh->GetBodySetup() && GeneratedMesh->GetBodySetup()->bHasCookedCollisionData)
    {
        CurrentState = EGenerationState::CalculateSpawnPoints;
    }
}

void ATerrainGen::Step_CalculateSpawnPoints()
{
    OnGenerationProgress.Broadcast(FText::FromString("Calculating spawn points..."));

    // This is generally fast, so we do it in one go.
    CalculateSpawnPoints();

    CurrentState = EGenerationState::BuildNavigation;
}

void ATerrainGen::Step_BuildNavigation()
{
    OnGenerationProgress.Broadcast(FText::FromString("Building navigation data..."));

    if (UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld()))
    {
        NavSys->Build();
    }

    CurrentState = EGenerationState::Finalize;
}

void ATerrainGen::Step_Finalize()
{
    OnGenerationProgress.Broadcast(FText::FromString("Finalizing..."));

    if (Mesh->GetStaticMesh())
    {
        RestoreActorPhysics();
        Mesh->SetMobility(EComponentMobility::Static);
    }

    // Mark terrain as ready on server
    if (HasAuthority())
    {
        bTerrainReady = true;
        UE_LOG(LogTemp, Warning, TEXT("[SERVER] TerrainGen: Terrain generation complete. bTerrainReady set to true"));

        // Notify all clients that the terrain is ready
        Multicast_NotifyClientsReady();
    }

    OnGenerationComplete.Broadcast();
    CurrentState = EGenerationState::Idle;
    GetWorldTimerManager().ClearTimer(GenerationProcessTimer);
}

void ATerrainGen::Multicast_NotifyClientsReady_Implementation()
{
    UE_LOG(LogTemp, Warning, TEXT("[%s] TerrainGen::Multicast_NotifyClientsReady_Implementation - Broadcasting OnAllClientsReady"),
        GetWorld()->GetNetMode() == NM_Client ? TEXT("CLIENT") : TEXT("SERVER"));
    OnAllClientsReady.Broadcast();
}

bool ATerrainGen::LoadHeightMapRaw(const FString& FilePath, int32& OutWidth, int32& OutHeight, TArray<uint8>& OutData)
{
    TArray<uint8> FileData;
    if (!FFileHelper::LoadFileToArray(FileData, *FilePath))
    {
        UE_LOG(LogTemp, Warning, TEXT("Failed to load file: %s"), *FilePath);
        return false;
    }

    IImageWrapperModule& ImgModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
    TSharedPtr<IImageWrapper> Wrapper = ImgModule.CreateImageWrapper(EImageFormat::PNG);

    if (Wrapper.IsValid() && Wrapper->SetCompressed(FileData.GetData(), FileData.Num()))
    {
        OutWidth = Wrapper->GetWidth();
        OutHeight = Wrapper->GetHeight();
        return Wrapper->GetRaw(ERGBFormat::Gray, 8, OutData);
    }

    UE_LOG(LogTemp, Warning, TEXT("Failed to decode png: %s"), *FilePath);
    return false;
}

// --------------------------------------------------------------------------------------
// Splat map generation and material binding
// --------------------------------------------------------------------------------------

static FORCEINLINE float SmoothStep(float Edge0, float Edge1, float X)
{
    const float T = FMath::Clamp((X - Edge0) / FMath::Max(Edge1 - Edge0, KINDA_SMALL_NUMBER), 0.0f, 1.0f);
    return T * T * (3.0f - 2.0f * T);
}

void ATerrainGen::GenerateSplatMaps(const FProcTerrainPresetDefinition* OptionalPresetDef)
{
    SplatGroupTextures.Empty();
    AvailableSplatGroups.Empty();
    AvailableSplatLayers.Empty();

    // Use preset rules if available, else fall back to a simple default rule set
    FSplatMapRulesDefinition Rules;
    if (OptionalPresetDef && OptionalPresetDef->Splat.OutputGroups.Num() > 0)
    {
        Rules = OptionalPresetDef->Splat;
    }
    else
    {
        Rules.BlendDistance = 0.05f;
        Rules.bExportChannelsSeparately = false;
        FSplatMapGroupDefinition Group; Group.GroupName = TEXT("base");
        {
            FSplatLayerDef Base; Base.Name = TEXT("dirt"); Base.bIsBaseLayer = true; Group.Layers.Add(Base);
        }
        {
            FSplatLayerDef L; L.Name = TEXT("grass"); L.bHasChannel = true; L.Channel = 'R';
            L.Rules.bHasMaxSlope = true; L.Rules.MaxSlope = 0.35f; Group.Layers.Add(L);
        }
        {
            FSplatLayerDef L; L.Name = TEXT("rock"); L.bHasChannel = true; L.Channel = 'G';
            L.Rules.bHasMinSlope = true; L.Rules.MinSlope = 0.5f; Group.Layers.Add(L);
        }
        Rules.OutputGroups.Add(Group);
    }

    const int32 W = HeightmapWidth;
    const int32 H = HeightmapHeight;
    if (W <= 0 || H <= 0 || HeightData.Num() != W * H)
    {
        return;
    }

    // Altitude [0,1]
    TArray<float> Altitude; Altitude.SetNumUninitialized(W * H);
    for (int32 i = 0; i < W * H; ++i)
    {
        Altitude[i] = static_cast<float>(HeightData[i]) / 255.0f;
    }

    // Slope estimation (norm of gradient) then normalise to 0..1
    TArray<float> Slope; Slope.SetNumZeroed(W * H);
    float MaxSlope = 0.0f;
    auto SampleAlt = [&](int32 X, int32 Y) -> float
        {
            X = FMath::Clamp(X, 0, W - 1);
            Y = FMath::Clamp(Y, 0, H - 1);
            return Altitude[Y * W + X];
        };
    for (int32 y = 0; y < H; ++y)
    {
        for (int32 x = 0; x < W; ++x)
        {
            const float Dzdx = (SampleAlt(x + 1, y) - SampleAlt(x - 1, y)) * 0.5f;
            const float Dzdy = (SampleAlt(x, y + 1) - SampleAlt(x, y - 1)) * 0.5f;
            const float G = FMath::Sqrt(Dzdx * Dzdx + Dzdy * Dzdy);
            Slope[y * W + x] = G;
            MaxSlope = FMath::Max(MaxSlope, G);
        }
    }
    if (MaxSlope > SMALL_NUMBER)
    {
        const float Inv = 1.0f / MaxSlope;
        for (float& V : Slope) V *= Inv;
    }

    const float Blend = Rules.BlendDistance;

    // For each output group, compute RGBA texture
    for (const FSplatMapGroupDefinition& Group : Rules.OutputGroups)
    {
        // Separate base vs explicit
        TArray<const FSplatLayerDef*> ExplicitLayers;
        const FSplatLayerDef* BaseLayer = nullptr;
        for (const FSplatLayerDef& L : Group.Layers)
        {
            if (L.bIsBaseLayer)
            {
                if (!BaseLayer) BaseLayer = &L; else { BaseLayer = nullptr; break; }
            }
            else
            {
                ExplicitLayers.Add(&L);
            }
        }
        if (!BaseLayer)
        {
            UE_LOG(LogTemp, Warning, TEXT("Splat group '%s' must have exactly one base layer. Skipping."), *Group.GroupName.ToString());
            continue;
        }

        // Track groups and layers for editor visibility
        AvailableSplatGroups.AddUnique(Group.GroupName);
        for (const FSplatLayerDef& L : Group.Layers)
        {
            AvailableSplatLayers.AddUnique(L.Name);
        }

        // Compute explicit layer weights
        const int32 NumExp = ExplicitLayers.Num();
        TArray<TArray<float>> LayerWeights; LayerWeights.SetNum(NumExp);
        for (int32 li = 0; li < NumExp; ++li)
        {
            LayerWeights[li].SetNumUninitialized(W * H);
        }
        TArray<float> SumExplicit; SumExplicit.SetNumZeroed(W * H);

        for (int32 li = 0; li < NumExp; ++li)
        {
            const FSplatLayerDef& L = *ExplicitLayers[li];
            const FSplatLayerRuleDef& R = L.Rules;
            float* RESTRICT LW = LayerWeights[li].GetData();
            for (int32 i = 0; i < W * H; ++i)
            {
                float Influence = 1.0f;
                const float A = Altitude[i];
                const float S = Slope[i];
                if (R.bHasMinAltitude)
                {
                    Influence *= SmoothStep(R.MinAltitude - Blend, R.MinAltitude + Blend, A);
                }
                if (R.bHasMaxAltitude)
                {
                    Influence *= (1.0f - SmoothStep(R.MaxAltitude - Blend, R.MaxAltitude + Blend, A));
                }
                if (R.bHasMinSlope)
                {
                    Influence *= SmoothStep(R.MinSlope - Blend, R.MinSlope + Blend, S);
                }
                if (R.bHasMaxSlope)
                {
                    Influence *= (1.0f - SmoothStep(R.MaxSlope - Blend, R.MaxSlope + Blend, S));
                }
                LW[i] = Influence;
                SumExplicit[i] += Influence;
            }
        }

        // Normalise explicit to sum<=1
        for (int32 i = 0; i < W * H; ++i)
        {
            const float Den = FMath::Max(1.0f, SumExplicit[i]);
            if (Den > 1.0f + KINDA_SMALL_NUMBER)
            {
                SumExplicit[i] = 1.0f; // will be used for base below
            }
            for (int32 li = 0; li < NumExp; ++li)
            {
                LayerWeights[li][i] /= Den;
            }
        }

        // Base weights
        TArray<float> BaseW; BaseW.SetNumUninitialized(W * H);
        for (int32 i = 0; i < W * H; ++i)
        {
            float FinalExp = 0.0f;
            for (int32 li = 0; li < NumExp; ++li) FinalExp += LayerWeights[li][i];
            BaseW[i] = FMath::Clamp(1.0f - FinalExp, 0.0f, 1.0f);
        }

        // Pack RGBA
        TArray<FColor> Pixels; Pixels.SetNumZeroed(W * H);
        bool Used[4] = { false, false, false, false };
        TMap<FName, int32> LayerToChannel;

        auto ChannelToIndex = [](TCHAR C) -> int32
            {
                switch (C)
                {
                case 'R': case 'r': return 0;
                case 'G': case 'g': return 1;
                case 'B': case 'b': return 2;
                case 'A': case 'a': return 3;
                default: return -1;
                }
            };

        for (int32 li = 0; li < NumExp; ++li)
        {
            const FSplatLayerDef& L = *ExplicitLayers[li];
            const int32 Channel = L.bHasChannel ? ChannelToIndex(L.Channel) : -1;
            if (Channel < 0 || Channel > 3)
            {
                UE_LOG(LogTemp, Warning, TEXT("Layer '%s' in group '%s' has invalid channel. Skipping."), *L.Name.ToString(), *Group.GroupName.ToString());
                continue;
            }
            Used[Channel] = true;
            LayerToChannel.Add(L.Name, Channel);
            const float* LW = LayerWeights[li].GetData();
            for (int32 i = 0; i < W * H; ++i)
            {
                const uint8 V = static_cast<uint8>(FMath::Clamp(LW[i] * 255.0f, 0.0f, 255.0f));
                FColor& P = Pixels[i];
                switch (Channel)
                {
                case 0: P.R = V; break;
                case 1: P.G = V; break;
                case 2: P.B = V; break;
                case 3: P.A = V; break;
                }
            }
        }

        // Assign base to first available channel
        int32 BaseChannel = 0;
        while (BaseChannel < 4 && Used[BaseChannel]) ++BaseChannel;
        if (BaseChannel >= 4)
        {
            UE_LOG(LogTemp, Warning, TEXT("Splat group '%s' had no free channel for base layer '%s'. Overwriting alpha."), *Group.GroupName.ToString(), *BaseLayer->Name.ToString());
            BaseChannel = 3;
        }
        for (int32 i = 0; i < W * H; ++i)
        {
            const uint8 V = static_cast<uint8>(FMath::Clamp(BaseW[i] * 255.0f, 0.0f, 255.0f));
            FColor& P = Pixels[i];
            switch (BaseChannel)
            {
            case 0: P.R = V; break;
            case 1: P.G = V; break;
            case 2: P.B = V; break;
            case 3: P.A = V; break;
            }
        }
        LayerToChannel.Add(BaseLayer->Name, BaseChannel);

        // Create texture
        UTexture2D* Tex = CreateTextureRGBA8(W, H, Pixels, FString::Printf(TEXT("Splat_%s"), *Group.GroupName.ToString()))
            ;
        if (Tex)
        {
            SplatGroupTextures.Add(Group.GroupName, Tex);
            SplatGroupChannelMap.Add(Group.GroupName, LayerToChannel);
        }
    }
}

UTexture2D* ATerrainGen::CreateTextureRGBA8(int32 InWidth, int32 InHeight, const TArray<FColor>& Pixels, const FString& DebugName)
{
    if (InWidth <= 0 || InHeight <= 0 || Pixels.Num() != InWidth * InHeight)
    {
        return nullptr;
    }
    UTexture2D* NewTex = UTexture2D::CreateTransient(InWidth, InHeight, EPixelFormat::PF_B8G8R8A8, DebugName.IsEmpty() ? NAME_None : FName(*DebugName));
    if (!NewTex || !NewTex->GetPlatformData() || NewTex->GetPlatformData()->Mips.Num() == 0)
    {
        return nullptr;
    }
    FTexture2DMipMap& Mip = NewTex->GetPlatformData()->Mips[0];
    // Must lock for write BEFORE realloc per BulkData contract
    void* Data = Mip.BulkData.Lock(LOCK_READ_WRITE);
    Data = Mip.BulkData.Realloc(static_cast<int64>(Pixels.Num() * sizeof(FColor)));
    FMemory::Memcpy(Data, Pixels.GetData(), static_cast<SIZE_T>(Pixels.Num() * sizeof(FColor)));
    Mip.BulkData.Unlock();
    NewTex->SRGB = false; // masks should be linear
    NewTex->CompressionSettings = TC_Masks;
    NewTex->Filter = TF_Bilinear;
    NewTex->UpdateResource();
    return NewTex;
}

void ATerrainGen::ApplyMaterialBindings(const FProcTerrainPresetDefinition* OptionalPresetDef)
{
    if (!Mesh)
    {
        return;
    }
    // Ensure the component has at least one material slot
    if (UStaticMesh* SM = Mesh->GetStaticMesh())
    {
        if (SM->GetStaticMaterials().Num() == 0)
        {
            FStaticMaterial StaticMat(GetDefaultSurfaceMaterial(), FName(TEXT("AutoSlot0")));
            StaticMat.UVChannelData.bInitialized = true;
            SM->GetStaticMaterials().Add(StaticMat);
            Mesh->MarkRenderStateDirty();
        }
    }

    // Create or update the runtime MID from a valid base material
    if (TerrainMaterial)
    {
        RuntimeMID = Mesh->CreateAndSetMaterialInstanceDynamicFromMaterial(0, TerrainMaterial);
        UE_LOG(LogTemp, Warning, TEXT("[TerrainGen] Using provided TerrainMaterial: %s"), *TerrainMaterial->GetName());
    }
#if WITH_EDITOR
    else
    {
        // Auto-create a minimal blend material in-editor if none provided
        if (UMaterial* AutoMat = CreateAutoBlendMaterialTransient())
        {
            TerrainMaterial = AutoMat;
            RuntimeMID = Mesh->CreateAndSetMaterialInstanceDynamicFromMaterial(0, TerrainMaterial);
            UE_LOG(LogTemp, Warning, TEXT("[TerrainGen] Auto-created terrain blend material: %s"), *TerrainMaterial->GetName());
        }
    }
#else
    else
    {
        // Fallback to default engine material at runtime if none provided
        UMaterialInterface* DefaultMat = GetDefaultSurfaceMaterial();
        RuntimeMID = Mesh->CreateAndSetMaterialInstanceDynamicFromMaterial(0, DefaultMat);
        UE_LOG(LogTemp, Warning, TEXT("[TerrainGen] Using default engine material for terrain"));
    }
#endif

    if (!RuntimeMID)
    {
        UE_LOG(LogTemp, Error, TEXT("[TerrainGen] Failed to create RuntimeMID at slot 0"));
        return;
    }

#if WITH_EDITOR
    // If the current base material doesn't expose expected parameters, fall back to auto-blend material
    if (RuntimeMID)
    {
        TArray<FMaterialParameterInfo> ParamInfos;
        TArray<FGuid> ParamIds;
        RuntimeMID->GetAllTextureParameterInfo(ParamInfos, ParamIds);
        auto HasParam = [&](const TCHAR* Name)
            {
                for (const FMaterialParameterInfo& Info : ParamInfos)
                {
                    if (Info.Name == Name) return true;
                }
                return false;
            };
        const bool bLooksCompatible = HasParam(TEXT("Splat_Any")) || HasParam(TEXT("Layer0_BaseColor"));
        if (!bLooksCompatible)
        {
            UE_LOG(LogTemp, Warning, TEXT("[TerrainGen] TerrainMaterial has no expected params (Splat_Any/Layer0_BaseColor). Using auto-blend."));
            if (UMaterial* AutoMat = CreateAutoBlendMaterialTransient())
            {
                TerrainMaterial = AutoMat;
                RuntimeMID = Mesh->CreateAndSetMaterialInstanceDynamicFromMaterial(0, TerrainMaterial);
                UE_LOG(LogTemp, Warning, TEXT("[TerrainGen] Switched to auto-blend material: %s"), *TerrainMaterial->GetName());
            }
        }
    }
#endif

    if (bApplySplatToMaterial)
    {
        for (const TPair<FName, UTexture2D*>& Pair : SplatGroupTextures)
        {
            const FName ParamName(*FString::Printf(TEXT("Splat_%s"), *Pair.Key.ToString()));
            RuntimeMID->SetTextureParameterValue(ParamName, Pair.Value);
            UE_LOG(LogTemp, Warning, TEXT("[TerrainGen] Set splat param %s -> %s (%dx%d)"), *ParamName.ToString(), *Pair.Value->GetName(), Pair.Value->GetSizeX(), Pair.Value->GetSizeY());
        }
        // Also set a generic splat parameter for master materials that don't know group names
        if (SplatGroupTextures.Num() > 0)
        {
            const UTexture2D* AnySplat = SplatGroupTextures.CreateConstIterator()->Value;
            RuntimeMID->SetTextureParameterValue(TEXT("Splat_Any"), const_cast<UTexture2D*>(AnySplat));
            UE_LOG(LogTemp, Warning, TEXT("[TerrainGen] Set generic splat param Splat_Any -> %s"), *AnySplat->GetName());
        }
    }

    // Bind per-layer textures extracted from assigned layer material instances (grouped only)
    // Flat map already: AllPresetLayerMaterials is (Preset.Layer -> MI)
    TMap<FName, UMaterialInstance*> CombinedLayers = AllPresetLayerMaterials;

    auto MakeLayerSlug = [](const FName& FlatKey) -> FString
        {
            FString Key = FlatKey.ToString();
            int32 DotIndex;
            FString LayerPart = Key;
            if (Key.FindLastChar('.', DotIndex))
            {
                LayerPart = Key.Mid(DotIndex + 1);
            }
            // Replace spaces and non-alnum with underscore
            for (int32 i = 0; i < LayerPart.Len(); ++i)
            {
                TCHAR& C = LayerPart[i];
                const bool bAlnum = (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') || (C >= '0' && C <= '9');
                if (!bAlnum && C != '_')
                {
                    C = '_';
                }
            }
            return LayerPart;
        };

    for (const TPair<FName, UMaterialInstance*>& LM : CombinedLayers)
    {
        const FName& LayerName = LM.Key;
        if (!LM.Value) continue;
        const UMaterialInstance* AsMI = LM.Value;

        auto SetTexFromMI = [&](const TCHAR* SrcParam, const TCHAR* DstSuffix)
            {
                UTexture* Tex = nullptr;
                FMaterialParameterInfo Info(SrcParam);
                if (AsMI->GetTextureParameterValue(Info, Tex) && Tex)
                {
                    const FString ParamName = FString::Printf(TEXT("Layer_%s_%s"), *MakeLayerSlug(LayerName), DstSuffix);
                    RuntimeMID->SetTextureParameterValue(FName(*ParamName), Tex);
                    UE_LOG(LogTemp, Warning, TEXT("[TerrainGen] Bound %s.%s -> %s"), *LM.Value->GetName(), SrcParam, *ParamName);
                }
            };

        // Only bind common Megascans params
        SetTexFromMI(TEXT("Albedo"), TEXT("BaseColor"));
        SetTexFromMI(TEXT("Normal"), TEXT("Normal"));
        SetTexFromMI(TEXT("ARD"), TEXT("ORM"));
    }

    // Populate generic 4-slot layer parameters (Layer0_*, Layer1_*, Layer2_*, Layer3_*) based on the first available splat group
    if (SplatGroupTextures.Num() > 0 && SplatGroupChannelMap.Num() > 0)
    {
        const FName FirstGroup = SplatGroupTextures.CreateConstIterator()->Key;

        // Helper: find MI for a layer name ".<Layer>"
        auto FindMIForLayer = [&](const FName& Layer) -> const UMaterialInstance*
            {
                const FString Target = FString::Printf(TEXT(".%s"), *Layer.ToString());
                for (const TPair<FName, UMaterialInstance*>& P : AllPresetLayerMaterials)
                {
                    if (P.Value && P.Key.ToString().EndsWith(Target))
                    {
                        return P.Value;
                    }
                }
                return nullptr;
            };

        // Determine semantics from the preset definition (preferred).
        const FSplatMapGroupDefinition* GroupDef = nullptr;
        if (OptionalPresetDef)
        {
            for (const FSplatMapGroupDefinition& G : OptionalPresetDef->Splat.OutputGroups)
            {
                if (G.GroupName == FirstGroup) { GroupDef = &G; break; }
            }
        }

        FName BaseName, RName, GName, BName;

        if (GroupDef)
        {
            for (const FSplatLayerDef& L : GroupDef->Layers)
            {
                if (L.bIsBaseLayer) { BaseName = L.Name; continue; }
                if (L.bHasChannel)
                {
                    switch (L.Channel)
                    {
                    case 'R': case 'r': RName = L.Name; break;
                    case 'G': case 'g': GName = L.Name; break;
                    case 'B': case 'b': BName = L.Name; break;
                        // NOTE: Ignore 'A' on purpose; base becomes explicit (Layer0).
                    }
                }
            }
        }
        else
        {
            // Fallback: try to infer from the channel map if no preset def was provided.
            if (const TMap<FName, int32>* LayerToChan = SplatGroupChannelMap.Find(FirstGroup))
            {
                for (const TPair<FName, int32>& P : *LayerToChan)
                {
                    switch (P.Value)
                    {
                    case 0: RName = P.Key; break;
                    case 1: GName = P.Key; break;
                    case 2: BName = P.Key; break;
                    case 3: /* alpha: assume base */ BaseName = P.Key; break;
                    }
                }
            }
        }

        auto SetSlotFromMI = [&](int32 Slot, const UMaterialInstance* AsMI)
            {
                if (!AsMI) return;
                auto Set = [&](const TCHAR* Src, const TCHAR* Suffix)
                    {
                        UTexture* Tex = nullptr;
                        if (AsMI->GetTextureParameterValue(FMaterialParameterInfo(Src), Tex) && Tex)
                        {
                            RuntimeMID->SetTextureParameterValue(
                                FName(*FString::Printf(TEXT("Layer%d_%s"), Slot, Suffix)), Tex);
                        }
                    };
                Set(TEXT("BaseColor"), TEXT("BaseColor"));
                Set(TEXT("Albedo"), TEXT("BaseColor"));
                Set(TEXT("Normal"), TEXT("Normal"));
                Set(TEXT("ORM"), TEXT("ORM"));
                Set(TEXT("RMA"), TEXT("ORM"));
                Set(TEXT("RoughnessMetallicAO"), TEXT("ORM"));
            };

        // Map to slots: 0=Base, 1=R, 2=G, 3=B; also propagate UV and strength params from MI if present
        auto ApplyMIParamsToSlot = [&](int32 Slot, const UMaterialInstance* AsMI)
            {
                if (!AsMI) return;
                auto ParamNameFor = [&](const TCHAR* Suffix) -> FName
                    {
                        const FString Name = FString::Printf(TEXT("Layer%d_"), Slot) + Suffix;
                        return FName(*Name);
                    };

                // Megascans common names
                float RotationDeg = 0.f; AsMI->GetScalarParameterValue(FMaterialParameterInfo(TEXT("Rotation Angle")), RotationDeg);
                RuntimeMID->SetScalarParameterValue(ParamNameFor(TEXT("UVRotationDeg")), RotationDeg);

                float NS = 1.f; AsMI->GetScalarParameterValue(FMaterialParameterInfo(TEXT("Normal Strength")), NS);
                RuntimeMID->SetScalarParameterValue(ParamNameFor(TEXT("NormalStrength")), NS);

                float AOS = 1.f; AsMI->GetScalarParameterValue(FMaterialParameterInfo(TEXT("AO Strength")), AOS);
                RuntimeMID->SetScalarParameterValue(ParamNameFor(TEXT("AOStrength")), AOS);

                float MinR = 0.f, MaxR = 1.f;
                AsMI->GetScalarParameterValue(FMaterialParameterInfo(TEXT("Min Roughness")), MinR);
                AsMI->GetScalarParameterValue(FMaterialParameterInfo(TEXT("Max Roughness")), MaxR);
                RuntimeMID->SetVectorParameterValue(ParamNameFor(TEXT("MinMaxRoughness")), FLinearColor(MinR, MaxR, 0, 0));

                // Packed mode: if ARD is used and name suggests ORDp, set mode=1
                UTexture* Packed = nullptr; AsMI->GetTextureParameterValue(FMaterialParameterInfo(TEXT("ARD")), Packed);
                const bool bORDp = Packed && Packed->GetName().Contains(TEXT("ORDp"));
                RuntimeMID->SetScalarParameterValue(ParamNameFor(TEXT("PackedMode")), bORDp ? 1.f : 0.f);

                // Split Tiling/Offset into two params (scale, offset)
                FLinearColor TO(1, 1, 0, 0);
                AsMI->GetVectorParameterValue(FMaterialParameterInfo(TEXT("Tiling/Offset")), TO);
                RuntimeMID->SetVectorParameterValue(ParamNameFor(TEXT("UVScale")), FLinearColor(TO.R, TO.G, 0, 0));
                RuntimeMID->SetVectorParameterValue(ParamNameFor(TEXT("UVOffset")), FLinearColor(TO.B, TO.A, 0, 0));
            };

        SetSlotFromMI(0, FindMIForLayer(BaseName)); ApplyMIParamsToSlot(0, FindMIForLayer(BaseName));
        SetSlotFromMI(1, FindMIForLayer(RName));   ApplyMIParamsToSlot(1, FindMIForLayer(RName));
        SetSlotFromMI(2, FindMIForLayer(GName));   ApplyMIParamsToSlot(2, FindMIForLayer(GName));
        SetSlotFromMI(3, FindMIForLayer(BName));   ApplyMIParamsToSlot(3, FindMIForLayer(BName));

        UE_LOG(LogTemp, Warning, TEXT("[TerrainGen] Semantic slot map: L0(Base)=%s L1(R)=%s L2(G)=%s L3(B)=%s"),
            *BaseName.ToString(), *RName.ToString(), *GName.ToString(), *BName.ToString());
    }
}
#if WITH_EDITOR
UMaterial* ATerrainGen::CreateAutoBlendMaterialTransient()
{
    UMaterial* Mat = NewObject<UMaterial>(GetTransientPackage(), UMaterial::StaticClass(),
        TEXT("M_TerrainAutoBlend"), RF_Public | RF_Transient);
    Mat->BlendMode = BLEND_Opaque;
    Mat->SetShadingModel(MSM_DefaultLit);
    Mat->TwoSided = true;

    auto& Coll = Mat->GetEditorOnlyData()->ExpressionCollection;

    // Load engine defaults
    UTexture2D* DefaultColor = LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"));
    UTexture2D* DefaultNormal = LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EngineMaterials/DefaultNormal.DefaultNormal"));

    // Optional: make a tiny TC_Masks default for packed ORM
    UTexture2D* DefaultMask = UTexture2D::CreateTransient(1, 1, PF_B8G8R8A8);
    {
        // AO=1 (R=255), Roughness=0.5 (G=128), Metallic=0 (B=0)
        FColor Pixel(255, 128, 0, 255);
        FTexture2DMipMap& Mip = DefaultMask->GetPlatformData()->Mips[0];
        void* Data = Mip.BulkData.Lock(LOCK_READ_WRITE);
        FMemory::Memcpy(Data, &Pixel, sizeof(FColor));
        Mip.BulkData.Unlock();
        DefaultMask->CompressionSettings = TC_Masks;
        DefaultMask->SRGB = false;
        DefaultMask->UpdateResource();
    }

    // Splat mask
    auto* Splat = NewObject<UMaterialExpressionTextureSampleParameter2D>(Mat);
    Splat->ParameterName = TEXT("Splat_Any");
    Splat->SamplerType = SAMPLERTYPE_Masks; // mask in linear/color domain
    Splat->Texture = DefaultMask;
    Coll.Expressions.Add(Splat);

    // Component masks for R,G,B
    auto* MaskR = NewObject<UMaterialExpressionComponentMask>(Mat); MaskR->R = true; MaskR->Input.Expression = Splat; Coll.Expressions.Add(MaskR);
    auto* MaskG = NewObject<UMaterialExpressionComponentMask>(Mat); MaskG->G = true; MaskG->Input.Expression = Splat; Coll.Expressions.Add(MaskG);
    auto* MaskB = NewObject<UMaterialExpressionComponentMask>(Mat); MaskB->B = true; MaskB->Input.Expression = Splat; Coll.Expressions.Add(MaskB);

    // coverage = saturate(r + g + b)
    auto* AddRG = NewObject<UMaterialExpressionAdd>(Mat);    AddRG->A.Expression = MaskR; AddRG->B.Expression = MaskG; Coll.Expressions.Add(AddRG);
    auto* AddRGB = NewObject<UMaterialExpressionAdd>(Mat);    AddRGB->A.Expression = AddRG; AddRGB->B.Expression = MaskB; Coll.Expressions.Add(AddRGB);
    auto* Zero = NewObject<UMaterialExpressionConstant>(Mat); Zero->R = 0.f; Coll.Expressions.Add(Zero);
    auto* One = NewObject<UMaterialExpressionConstant>(Mat); One->R = 1.f; Coll.Expressions.Add(One);

    // baseW = 1 - saturate(coverage). We rely on the material clamp; subtraction is enough here.
    auto* BaseW = NewObject<UMaterialExpressionSubtract>(Mat);
    BaseW->A.Expression = One;
    BaseW->B.Expression = AddRGB;
    Coll.Expressions.Add(BaseW);

    // Create per-layer texture params
    TArray<UMaterialExpressionTextureSampleParameter2D*> Col, Nor, ORM;
    Col.SetNum(4); Nor.SetNum(4); ORM.SetNum(4);

    for (int32 i = 0; i < 4; ++i)
    {
        auto* C = NewObject<UMaterialExpressionTextureSampleParameter2D>(Mat);
        C->ParameterName = FName(*FString::Printf(TEXT("Layer%d_BaseColor"), i));
        C->SamplerType = SAMPLERTYPE_Color;
        C->Texture = DefaultColor;
        Coll.Expressions.Add(C);

        auto* N = NewObject<UMaterialExpressionTextureSampleParameter2D>(Mat);
        N->ParameterName = FName(*FString::Printf(TEXT("Layer%d_Normal"), i));
        N->SamplerType = SAMPLERTYPE_Normal;
        N->Texture = DefaultNormal;
        Coll.Expressions.Add(N);

        auto* O = NewObject<UMaterialExpressionTextureSampleParameter2D>(Mat);
        O->ParameterName = FName(*FString::Printf(TEXT("Layer%d_ORM"), i));
        O->SamplerType = SAMPLERTYPE_Masks;
        O->Texture = DefaultMask;
        Coll.Expressions.Add(O);

        // 🔧 Store them so Col/Nor/ORM[i] aren’t null later
        Col[i] = C;
        Nor[i] = N;
        ORM[i] = O;
    }

    // Per-layer UV transform: Scale (Sx,Sy), Offset (Ox,Oy), Rotation (deg)
    // We expose scalar/vector params per slot: Layer{n}_UVScale, Layer{n}_UVOffset, Layer{n}_UVRotationDeg
    auto BuildUV = [&](int32 Slot) -> UMaterialExpression* {
        auto* TexCoord = NewObject<UMaterialExpressionTextureCoordinate>(Mat);
        TexCoord->CoordinateIndex = 0;
        Coll.Expressions.Add(TexCoord);

        auto* ScaleParam = NewObject<UMaterialExpressionVectorParameter>(Mat);
        ScaleParam->ParameterName = *FString::Printf(TEXT("Layer%d_UVScale"), Slot);
        ScaleParam->DefaultValue = FLinearColor(1, 1, 0, 0);
        Coll.Expressions.Add(ScaleParam);

        auto* ScaleRG = NewObject<UMaterialExpressionComponentMask>(Mat); ScaleRG->R = true; ScaleRG->G = true; ScaleRG->Input.Expression = ScaleParam; Coll.Expressions.Add(ScaleRG);

        auto* OffsetParam = NewObject<UMaterialExpressionVectorParameter>(Mat);
        OffsetParam->ParameterName = *FString::Printf(TEXT("Layer%d_UVOffset"), Slot);
        OffsetParam->DefaultValue = FLinearColor(0, 0, 0, 0);
        Coll.Expressions.Add(OffsetParam);

        auto* OffsetRG = NewObject<UMaterialExpressionComponentMask>(Mat); OffsetRG->R = true; OffsetRG->G = true; OffsetRG->Input.Expression = OffsetParam; Coll.Expressions.Add(OffsetRG);

        auto* RotDegParam = NewObject<UMaterialExpressionScalarParameter>(Mat);
        RotDegParam->ParameterName = *FString::Printf(TEXT("Layer%d_UVRotationDeg"), Slot);
        RotDegParam->DefaultValue = 0.0f;
        Coll.Expressions.Add(RotDegParam);

        auto* Pi = NewObject<UMaterialExpressionConstant>(Mat); Pi->R = PI / 180.f; Coll.Expressions.Add(Pi);
        auto* RotRad = NewObject<UMaterialExpressionMultiply>(Mat); RotRad->A.Expression = RotDegParam; RotRad->B.Expression = Pi; Coll.Expressions.Add(RotRad);
        auto* CosR = NewObject<UMaterialExpressionCosine>(Mat); CosR->Input.Expression = RotRad; Coll.Expressions.Add(CosR);
        auto* SinR = NewObject<UMaterialExpressionSine>(Mat);   SinR->Input.Expression = RotRad; Coll.Expressions.Add(SinR);

        // Scale UV
        auto* Scaled = NewObject<UMaterialExpressionMultiply>(Mat); Scaled->A.Expression = TexCoord; Scaled->B.Expression = ScaleRG; Coll.Expressions.Add(Scaled);

        // Rotate: [u v] * [[c -s],[s c]]
        auto* UMask = NewObject<UMaterialExpressionComponentMask>(Mat); UMask->R = true; UMask->Input.Expression = Scaled; Coll.Expressions.Add(UMask);
        auto* VMask = NewObject<UMaterialExpressionComponentMask>(Mat); VMask->G = true; VMask->Input.Expression = Scaled; Coll.Expressions.Add(VMask);

        auto* Uc = NewObject<UMaterialExpressionMultiply>(Mat); Uc->A.Expression = UMask; Uc->B.Expression = CosR; Coll.Expressions.Add(Uc);
        auto* Vs = NewObject<UMaterialExpressionMultiply>(Mat); Vs->A.Expression = VMask; Vs->B.Expression = SinR; Coll.Expressions.Add(Vs);
        auto* Uc_minus_Vs = NewObject<UMaterialExpressionSubtract>(Mat); Uc_minus_Vs->A.Expression = Uc; Uc_minus_Vs->B.Expression = Vs; Coll.Expressions.Add(Uc_minus_Vs);

        auto* Us = NewObject<UMaterialExpressionMultiply>(Mat); Us->A.Expression = UMask; Us->B.Expression = SinR; Coll.Expressions.Add(Us);
        auto* Vc = NewObject<UMaterialExpressionMultiply>(Mat); Vc->A.Expression = VMask; Vc->B.Expression = CosR; Coll.Expressions.Add(Vc);
        auto* Us_plus_Vc = NewObject<UMaterialExpressionAdd>(Mat); Us_plus_Vc->A.Expression = Us; Us_plus_Vc->B.Expression = Vc; Coll.Expressions.Add(Us_plus_Vc);

        auto* RotUV = NewObject<UMaterialExpressionAppendVector>(Mat); RotUV->A.Expression = Uc_minus_Vs; RotUV->B.Expression = Us_plus_Vc; Coll.Expressions.Add(RotUV);

        auto* OffsetUV = NewObject<UMaterialExpressionAdd>(Mat); OffsetUV->A.Expression = RotUV; OffsetUV->B.Expression = OffsetRG; Coll.Expressions.Add(OffsetUV);
        return OffsetUV;
        };

    // UVs per slot
    TArray<UMaterialExpression*> SlotUVs; SlotUVs.SetNum(4);
    for (int32 i = 0; i < 4; ++i) SlotUVs[i] = BuildUV(i);

    // Hook UVs into samplers
    for (int32 i = 0; i < 4; ++i) {
        Col[i]->Coordinates.Expression = SlotUVs[i];
        Nor[i]->Coordinates.Expression = SlotUVs[i];
        ORM[i]->Coordinates.Expression = SlotUVs[i];
    }

    // Helper: multiply vector by scalar (weight)
    auto MulVS = [&](UMaterialExpression* Vec, UMaterialExpression* Scalar) -> UMaterialExpressionMultiply*
        {
            auto* M = NewObject<UMaterialExpressionMultiply>(Mat);
            M->A.Expression = Vec;
            M->B.Expression = Scalar;
            Coll.Expressions.Add(M);
            return M;
        };

    // Strength scalars: NormalStrength, AOStrength, MinMaxRoughness (x=min,y=max)
    TArray<UMaterialExpressionScalarParameter*> NormalStrengthP, AOStrengthP; NormalStrengthP.SetNum(4); AOStrengthP.SetNum(4);
    TArray<UMaterialExpressionVectorParameter*> MinMaxRghP; MinMaxRghP.SetNum(4);
    for (int32 i = 0; i < 4; ++i)
    {
        auto* NS = NewObject<UMaterialExpressionScalarParameter>(Mat); NS->ParameterName = FName(*FString::Printf(TEXT("Layer%d_NormalStrength"), i)); NS->DefaultValue = 1.f; Coll.Expressions.Add(NS); NormalStrengthP[i] = NS;
        auto* AS = NewObject<UMaterialExpressionScalarParameter>(Mat); AS->ParameterName = FName(*FString::Printf(TEXT("Layer%d_AOStrength"), i)); AS->DefaultValue = 1.f; Coll.Expressions.Add(AS); AOStrengthP[i] = AS;
        auto* RR = NewObject<UMaterialExpressionVectorParameter>(Mat); RR->ParameterName = FName(*FString::Printf(TEXT("Layer%d_MinMaxRoughness"), i)); RR->DefaultValue = FLinearColor(0, 1, 0, 0); Coll.Expressions.Add(RR); MinMaxRghP[i] = RR;
    }

    // BaseColor = L0*baseW + L1*r + L2*g + L3*b
    auto* C0 = MulVS(Col[0], BaseW);
    auto* C1 = MulVS(Col[1], MaskR);
    auto* C2 = MulVS(Col[2], MaskG);
    auto* C3 = MulVS(Col[3], MaskB);
    auto* AddC01 = NewObject<UMaterialExpressionAdd>(Mat); AddC01->A.Expression = C0; AddC01->B.Expression = C1; Coll.Expressions.Add(AddC01);
    auto* AddC012 = NewObject<UMaterialExpressionAdd>(Mat); AddC012->A.Expression = AddC01; AddC012->B.Expression = C2; Coll.Expressions.Add(AddC012);
    auto* AddC = NewObject<UMaterialExpressionAdd>(Mat); AddC->A.Expression = AddC012; AddC->B.Expression = C3; Coll.Expressions.Add(AddC);
    Mat->GetEditorOnlyData()->BaseColor.Expression = AddC;

    // Normal = normalize((L0N*NS0)*baseW + (L1N*NS1)*r + (L2N*NS2)*g + (L3N*NS3)*b)
    auto* N0s = NewObject<UMaterialExpressionMultiply>(Mat); N0s->A.Expression = Nor[0]; N0s->B.Expression = NormalStrengthP[0]; Coll.Expressions.Add(N0s);
    auto* N1s = NewObject<UMaterialExpressionMultiply>(Mat); N1s->A.Expression = Nor[1]; N1s->B.Expression = NormalStrengthP[1]; Coll.Expressions.Add(N1s);
    auto* N2s = NewObject<UMaterialExpressionMultiply>(Mat); N2s->A.Expression = Nor[2]; N2s->B.Expression = NormalStrengthP[2]; Coll.Expressions.Add(N2s);
    auto* N3s = NewObject<UMaterialExpressionMultiply>(Mat); N3s->A.Expression = Nor[3]; N3s->B.Expression = NormalStrengthP[3]; Coll.Expressions.Add(N3s);
    auto* N0 = MulVS(N0s, BaseW);
    auto* N1 = MulVS(N1s, MaskR);
    auto* N2 = MulVS(N2s, MaskG);
    auto* N3 = MulVS(N3s, MaskB);
    auto* AddN01 = NewObject<UMaterialExpressionAdd>(Mat); AddN01->A.Expression = N0; AddN01->B.Expression = N1; Coll.Expressions.Add(AddN01);
    auto* AddN012 = NewObject<UMaterialExpressionAdd>(Mat); AddN012->A.Expression = AddN01; AddN012->B.Expression = N2; Coll.Expressions.Add(AddN012);
    auto* AddN = NewObject<UMaterialExpressionAdd>(Mat); AddN->A.Expression = AddN012; AddN->B.Expression = N3; Coll.Expressions.Add(AddN);
    auto* Normed = NewObject<UMaterialExpressionNormalize>(Mat); Normed->VectorInput.Expression = AddN; Coll.Expressions.Add(Normed);
    Mat->GetEditorOnlyData()->Normal.Expression = Normed;

    // ORM channels: default R->AO, G->Roughness, B->Metallic, but allow ORDp (B=Displacement, Metallic=0)
    // Per-slot switch: Layer{n}_PackedMode (0=ORM, 1=ORDp)
    TArray<UMaterialExpressionScalarParameter*> PackedMode; PackedMode.SetNum(4);
    for (int32 i = 0; i < 4; ++i)
    {
        auto* P = NewObject<UMaterialExpressionScalarParameter>(Mat);
        P->ParameterName = FName(*FString::Printf(TEXT("Layer%d_PackedMode"), i));
        P->DefaultValue = 0.0f; // 0=ORM, 1=ORDp
        Coll.Expressions.Add(P);
        PackedMode[i] = P;
    }
    auto MaskAO = [&](UMaterialExpression* O) -> UMaterialExpression* {
        auto* M = NewObject<UMaterialExpressionComponentMask>(Mat);
        M->R = true;
        M->Input.Expression = O;
        Coll.Expressions.Add(M);
        return M; // OK: returns UMaterialExpressionComponentMask*, upcasts to UMaterialExpression*
        };

    auto MaskRgh = [&](UMaterialExpression* O) -> UMaterialExpression* {
        auto* M = NewObject<UMaterialExpressionComponentMask>(Mat);
        M->G = true;
        M->Input.Expression = O;
        Coll.Expressions.Add(M);
        return M;
        };

    auto MaskMet = [&](UMaterialExpression* O, UMaterialExpressionScalarParameter* Mode) -> UMaterialExpression* {
        // Metallic = (Mode==0 ? B : 0)
        auto* BMask = NewObject<UMaterialExpressionComponentMask>(Mat); BMask->B = true; BMask->Input.Expression = O; Coll.Expressions.Add(BMask);
        auto* Zero = NewObject<UMaterialExpressionConstant>(Mat); Zero->R = 0.f; Coll.Expressions.Add(Zero);
        // Lerp(Zero, BMask, 1-abs(sign(0.5-Mode))) ~ choose between 0 and B
        auto* Half = NewObject<UMaterialExpressionConstant>(Mat); Half->R = 0.5f; Coll.Expressions.Add(Half);
        auto* Diff = NewObject<UMaterialExpressionSubtract>(Mat); Diff->A.Expression = Half; Diff->B.Expression = Mode; Coll.Expressions.Add(Diff);
        auto* Abs = NewObject<UMaterialExpressionAbs>(Mat); Abs->Input.Expression = Diff; Coll.Expressions.Add(Abs);
        auto* One = NewObject<UMaterialExpressionConstant>(Mat); One->R = 1.f; Coll.Expressions.Add(One);
        auto* Inv = NewObject<UMaterialExpressionSubtract>(Mat); Inv->A.Expression = One; Inv->B.Expression = Abs; Coll.Expressions.Add(Inv);
        auto* Lerp = NewObject<UMaterialExpressionLinearInterpolate>(Mat); Lerp->A.Expression = Zero; Lerp->B.Expression = BMask; Lerp->Alpha.Expression = Inv; Coll.Expressions.Add(Lerp);
        return Lerp;
        };

    // AO (apply AOStrength per slot)
    auto* AO0s = NewObject<UMaterialExpressionMultiply>(Mat); AO0s->A.Expression = MaskAO(ORM[0]); AO0s->B.Expression = AOStrengthP[0]; Coll.Expressions.Add(AO0s);
    auto* AO1s = NewObject<UMaterialExpressionMultiply>(Mat); AO1s->A.Expression = MaskAO(ORM[1]); AO1s->B.Expression = AOStrengthP[1]; Coll.Expressions.Add(AO1s);
    auto* AO2s = NewObject<UMaterialExpressionMultiply>(Mat); AO2s->A.Expression = MaskAO(ORM[2]); AO2s->B.Expression = AOStrengthP[2]; Coll.Expressions.Add(AO2s);
    auto* AO3s = NewObject<UMaterialExpressionMultiply>(Mat); AO3s->A.Expression = MaskAO(ORM[3]); AO3s->B.Expression = AOStrengthP[3]; Coll.Expressions.Add(AO3s);
    auto* AO0 = MulVS(AO0s, BaseW);
    auto* AO1 = MulVS(AO1s, MaskR);
    auto* AO2 = MulVS(AO2s, MaskG);
    auto* AO3 = MulVS(AO3s, MaskB);
    auto* AddAO01 = NewObject<UMaterialExpressionAdd>(Mat); AddAO01->A.Expression = AO0; AddAO01->B.Expression = AO1; Coll.Expressions.Add(AddAO01);
    auto* AddAO012 = NewObject<UMaterialExpressionAdd>(Mat); AddAO012->A.Expression = AddAO01; AddAO012->B.Expression = AO2; Coll.Expressions.Add(AddAO012);
    auto* AddAO = NewObject<UMaterialExpressionAdd>(Mat); AddAO->A.Expression = AddAO012; AddAO->B.Expression = AO3; Coll.Expressions.Add(AddAO);
    Mat->GetEditorOnlyData()->AmbientOcclusion.Expression = AddAO;

    // Roughness (clamp to per-layer min/max)
    auto ClampR = [&](UMaterialExpression* In, UMaterialExpressionVectorParameter* MinMax) -> UMaterialExpression* {
        auto* MinMask = NewObject<UMaterialExpressionComponentMask>(Mat); MinMask->R = true; MinMask->Input.Expression = MinMax; Coll.Expressions.Add(MinMask);
        auto* MaxMask = NewObject<UMaterialExpressionComponentMask>(Mat); MaxMask->G = true; MaxMask->Input.Expression = MinMax; Coll.Expressions.Add(MaxMask);
        auto* C = NewObject<UMaterialExpressionClamp>(Mat); C->Input.Expression = In; C->Min.Expression = MinMask; C->Max.Expression = MaxMask; Coll.Expressions.Add(C); return C; };
    auto* R0c = ClampR(MaskRgh(ORM[0]), MinMaxRghP[0]);
    auto* R1c = ClampR(MaskRgh(ORM[1]), MinMaxRghP[1]);
    auto* R2c = ClampR(MaskRgh(ORM[2]), MinMaxRghP[2]);
    auto* R3c = ClampR(MaskRgh(ORM[3]), MinMaxRghP[3]);
    auto* R0 = MulVS(R0c, BaseW);
    auto* R1 = MulVS(R1c, MaskR);
    auto* R2 = MulVS(R2c, MaskG);
    auto* R3 = MulVS(R3c, MaskB);
    auto* AddR01 = NewObject<UMaterialExpressionAdd>(Mat); AddR01->A.Expression = R0; AddR01->B.Expression = R1; Coll.Expressions.Add(AddR01);
    auto* AddR012 = NewObject<UMaterialExpressionAdd>(Mat); AddR012->A.Expression = AddR01; AddR012->B.Expression = R2; Coll.Expressions.Add(AddR012);
    auto* AddR = NewObject<UMaterialExpressionAdd>(Mat); AddR->A.Expression = AddR012; AddR->B.Expression = R3; Coll.Expressions.Add(AddR);
    Mat->GetEditorOnlyData()->Roughness.Expression = AddR;

    // Metallic
    auto* M0 = MulVS(MaskMet(ORM[0], PackedMode[0]), BaseW);
    auto* M1 = MulVS(MaskMet(ORM[1], PackedMode[1]), MaskR);
    auto* M2 = MulVS(MaskMet(ORM[2], PackedMode[2]), MaskG);
    auto* M3 = MulVS(MaskMet(ORM[3], PackedMode[3]), MaskB);
    auto* AddM01 = NewObject<UMaterialExpressionAdd>(Mat); AddM01->A.Expression = M0; AddM01->B.Expression = M1; Coll.Expressions.Add(AddM01);
    auto* AddM012 = NewObject<UMaterialExpressionAdd>(Mat); AddM012->A.Expression = AddM01; AddM012->B.Expression = M2; Coll.Expressions.Add(AddM012);
    auto* AddM = NewObject<UMaterialExpressionAdd>(Mat); AddM->A.Expression = AddM012; AddM->B.Expression = M3; Coll.Expressions.Add(AddM);
    Mat->GetEditorOnlyData()->Metallic.Expression = AddM;

    Mat->PostEditChange();
    Mat->MarkPackageDirty();
    Mat->ForceRecompileForRendering();
    return Mat;
}
#endif

#if WITH_EDITOR
void ATerrainGen::SnapshotCurrentMIDToAsset()
{
    if (!RuntimeMID)
    {
        UE_LOG(LogTemp, Warning, TEXT("[TerrainGen] SnapshotCurrentMIDToAsset: RuntimeMID is null"));
        return;
    }

    // Choose a path in the level folder
    FString PackagePath = FPaths::GetPath(GetOutermost()->GetPathName());
    if (PackagePath.IsEmpty())
    {
        PackagePath = TEXT("/Game");
    }
    const FString AssetName = FString::Printf(TEXT("Snapshot_%s_%s"), *GetName(), *FGuid::NewGuid().ToString(EGuidFormats::Short));
    const FString PackageName = PackagePath + TEXT("/") + AssetName;
    UPackage* Package = CreatePackage(*PackageName);
    UMaterialInstanceConstant* NewMIC = NewObject<UMaterialInstanceConstant>(Package, *AssetName, RF_Public | RF_Standalone);
    if (!NewMIC)
    {
        UE_LOG(LogTemp, Warning, TEXT("[TerrainGen] SnapshotCurrentMIDToAsset: Failed to create MIC"));
        return;
    }

    // Set parent to the current base material
    if (UMaterialInterface* ParentMat = RuntimeMID->GetMaterial())
    {
        NewMIC->SetParentEditorOnly(ParentMat);
    }

    // Copy texture parameter values we set
    TArray<FMaterialParameterInfo> ParamInfos; TArray<FGuid> ParamIds;
    RuntimeMID->GetAllTextureParameterInfo(ParamInfos, ParamIds);
    for (const FMaterialParameterInfo& Info : ParamInfos)
    {
        UTexture* Tex = nullptr;
        if (RuntimeMID->GetTextureParameterValue(Info, Tex) && Tex)
        {
            NewMIC->SetTextureParameterValueEditorOnly(Info.Name, Tex);
        }
    }
    NewMIC->PostEditChange();
    Package->MarkPackageDirty();
    FAssetRegistryModule::AssetCreated(NewMIC);
    UE_LOG(LogTemp, Warning, TEXT("[TerrainGen] SnapshotCurrentMIDToAsset: Created %s"), *NewMIC->GetPathName());
}
#endif

void ATerrainGen::UpdateLayerSlotsFromPreset(const FProcTerrainPresetDefinition& Def)
{
    // Pre-populate LayerMaterials keys with all layer names so users can just assign materials
    TSet<FName> Names;
    for (const FSplatMapGroupDefinition& G : Def.Splat.OutputGroups)
    {
        for (const FSplatLayerDef& L : G.Layers)
        {
            Names.Add(L.Name);
        }
    }
    // Update visible arrays
    AvailableSplatGroups.Empty();
    for (const FSplatMapGroupDefinition& G : Def.Splat.OutputGroups)
    {
        AvailableSplatGroups.AddUnique(G.GroupName);
    }
    AvailableSplatLayers.Empty();
    AvailableSplatLayers.Reserve(Names.Num());
    for (const FName& N : Names) AvailableSplatLayers.Add(N);
}

FName ATerrainGen::GetPresetDisplayName(ETerrainPreset InPreset)
{
    switch (InPreset)
    {
    case ETerrainPreset::DowntownRuins:            return TEXT("Downtown Ruins");
    case ETerrainPreset::CrystallineBloomfallZone: return TEXT("Crystalline Bloomfall Zone");
    case ETerrainPreset::MutatedSwamplands:        return TEXT("Mutated Swamplands");
    case ETerrainPreset::IrradiatedBadlands:       return TEXT("Irradiated Badlands");
    case ETerrainPreset::OldWorldAnomaly:          return TEXT("Old World Anomaly");
    case ETerrainPreset::GothicCathedralApproach:  return TEXT("Gothic Cathedral Approach");
    case ETerrainPreset::MangroveDeltaFull:        return TEXT("Mangrove Delta Full");
    case ETerrainPreset::ProvingGroundsSmall:      return TEXT("Proving Grounds Small");
    case ETerrainPreset::ArenaTiny513:             return TEXT("Arena Tiny 513");
    default:                                       return TEXT("None");
    }
}

void ATerrainGen::UpdateAllPresetLayerSlots()
{
    // Build a grouped map of all presets with their layer names
    TMap<FName, UMaterialInstance*> NewGrouped;
    auto AddPreset = [&](ETerrainPreset P)
        {
            FProcTerrainPresetDefinition Def;
            if (!ProcTerrainPresets::GetPreset(P, Def)) return;
            for (const FSplatMapGroupDefinition& G : Def.Splat.OutputGroups)
            {
                for (const FSplatLayerDef& L : G.Layers)
                {
                    const FName FlatKey = FName(*FString::Printf(TEXT("%s.%s"), *GetPresetDisplayName(P).ToString(), *L.Name.ToString()));
                    if (!NewGrouped.Contains(FlatKey))
                    {
                        NewGrouped.Add(FlatKey, nullptr);
                    }
                }
            }
        };
    AddPreset(ETerrainPreset::DowntownRuins);
    AddPreset(ETerrainPreset::CrystallineBloomfallZone);
    AddPreset(ETerrainPreset::MutatedSwamplands);
    AddPreset(ETerrainPreset::IrradiatedBadlands);
    AddPreset(ETerrainPreset::OldWorldAnomaly);
    AddPreset(ETerrainPreset::GothicCathedralApproach);
    AddPreset(ETerrainPreset::MangroveDeltaFull);
    AddPreset(ETerrainPreset::ProvingGroundsSmall);
    AddPreset(ETerrainPreset::ArenaTiny513);

    // Preserve any existing user assignments
    // Preserve any existing user assignments
    for (const TPair<FName, UMaterialInstance*>& OldPair : AllPresetLayerMaterials)
    {
        if (NewGrouped.Contains(OldPair.Key) && OldPair.Value)
        {
            NewGrouped[OldPair.Key] = OldPair.Value;
        }
    }
    AllPresetLayerMaterials = MoveTemp(NewGrouped);
}

void ATerrainGen::CalculateSpawnPoints()
{
    UE_LOG(LogTemp, Warning, TEXT("[%s] TerrainGen::CalculateSpawnPoints - Starting spawn point calculation"),
        HasAuthority() ? TEXT("SERVER") : TEXT("CLIENT"));

    SpawnPoints.Empty();
    if (NumPlayerStarts <= 0 || !GeneratedMesh)
    {
        UE_LOG(LogTemp, Warning, TEXT("[%s] TerrainGen::CalculateSpawnPoints - No player starts requested or no mesh"),
            HasAuthority() ? TEXT("SERVER") : TEXT("CLIENT"));
        return;
    }

    UStaticMeshDescription* Desc = GeneratedMesh->GetStaticMeshDescription(0);
    if (!Desc) return;

    const float MaxSlopeCosine = FMath::Cos(FMath::DegreesToRadians(MaxSpawnSlopeInDegrees));
    const FTransform ActorToWorld = GetActorTransform();

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
        UE_LOG(LogTemp, Warning, TEXT("No suitable flat areas found for spawning."));
        return;
    }

    for (int32 i = 0; i < CandidateLocations.Num(); ++i)
    {
        int32 j = FMath::RandRange(i, CandidateLocations.Num() - 1);
        CandidateLocations.Swap(i, j);
    }

    for (const FVector& LocalCandidate : CandidateLocations)
    {
        if (SpawnPoints.Num() >= NumPlayerStarts) break;

        FVector WorldCandidate = ActorToWorld.TransformPosition(LocalCandidate);
        bool bTooClose = false;
        for (const FVector& ExistingSpawn : SpawnPoints)
        {
            if (FVector::DistSquared(WorldCandidate, ExistingSpawn) < FMath::Square(MinSpawnSeparation))
            {
                bTooClose = true;
                break;
            }
        }
        if (!bTooClose)
        {
            SpawnPoints.Add(WorldCandidate);
            UE_LOG(LogTemp, VeryVerbose, TEXT("[%s] TerrainGen: Added spawn point at %s"),
                HasAuthority() ? TEXT("SERVER") : TEXT("CLIENT"), *WorldCandidate.ToString());
        }
    }
    UE_LOG(LogTemp, Warning, TEXT("[%s] TerrainGen: Calculated %d spawn points from %d candidates."),
        HasAuthority() ? TEXT("SERVER") : TEXT("CLIENT"), SpawnPoints.Num(), CandidateLocations.Num());

#if WITH_EDITOR
    FlushPersistentDebugLines(GetWorld());
    const float DebugSphereRadius = bUseLargeSpawnSpheres ? SpawnClearanceRadius * 5.0f : SpawnClearanceRadius;
    for (const FVector& SpawnPoint : SpawnPoints)
    {
        DrawDebugSphere(GetWorld(), SpawnPoint, DebugSphereRadius, 12, FColor::Green, true, -1, 0, 5.f);
    }
#endif
}



void ATerrainGen::DisableActorPhysicsTemporarily()
{
    if (!GetWorld()) return;
    ActorsToReenablePhysics.Empty();
    for (TActorIterator<AActor> It(GetWorld()); It; ++It)
    {
        AActor* Actor = *It;
        if (Actor && Actor != this)
        {
            TArray<UPrimitiveComponent*> PrimitiveComponents;
            Actor->GetComponents<UPrimitiveComponent>(PrimitiveComponents);
            bool bHadPhysics = false;
            for (UPrimitiveComponent* PrimComp : PrimitiveComponents)
            {
                if (PrimComp && PrimComp->IsSimulatingPhysics())
                {
                    PrimComp->SetSimulatePhysics(false);
                    bHadPhysics = true;
                }
            }
            if (bHadPhysics)
            {
                ActorsToReenablePhysics.Add(Actor);
            }
        }
    }
}

void ATerrainGen::RestoreActorPhysics()
{
    for (const auto& WeakActor : ActorsToReenablePhysics)
    {
        if (AActor* Actor = WeakActor.Get())
        {
            TArray<UPrimitiveComponent*> PrimitiveComponents;
            Actor->GetComponents<UPrimitiveComponent>(PrimitiveComponents);
            for (UPrimitiveComponent* PrimComp : PrimitiveComponents)
            {
                if (PrimComp)
                {
                    PrimComp->SetSimulatePhysics(true);
                }
            }
        }
    }
    ActorsToReenablePhysics.Empty();
}

void ATerrainGen::GenerateTerrain_Editor()
{
    // Simplified, blocking version for the editor
    ensure(Mesh != nullptr);

    int32 W = 0, H = 0;
    TArray<uint8> LocalHeightData;
    bool bLoaded = false;

    if (Preset != ETerrainPreset::None)
    {
        FProcTerrainPresetDefinition Def;
        if (ProcTerrainPresets::GetPreset(Preset, Def))
        {
            W = Def.Width; H = Def.Height;
            FProcTerrain PT(W, H, Def.Seed);
            PT.GenerateFBM(Def.Fbm);
            if (Def.bThermalEnabled) PT.ApplyThermal(Def.Thermal);
            if (Def.bHydraulicEnabled) PT.ApplyHydraulic(Def.Hydraulic);
            LocalHeightData.SetNumUninitialized(W * H);
            for (int i = 0; i < W * H; ++i) LocalHeightData[i] = (uint8)FMath::Clamp(PT.HeightMap[i] * 255.f, 0.f, 255.f);
            bLoaded = true;
        }
    }
    else if (HeightmapTexture)
    {
        if (HeightmapTexture->GetPlatformData() && HeightmapTexture->GetPlatformData()->Mips.Num() > 0)
        {
            FTexture2DMipMap& Mip = HeightmapTexture->GetPlatformData()->Mips[0];
            W = Mip.SizeX; H = Mip.SizeY;
            LocalHeightData.SetNumUninitialized(W * H);
            const FColor* Src = static_cast<const FColor*>(Mip.BulkData.LockReadOnly());
            for (int32 i = 0; i < W * H; ++i) LocalHeightData[i] = Src[i].R;
            Mip.BulkData.Unlock();
            bLoaded = true;
        }
    }
    else
    {
        FString FullPath = PngPath;
        if (FPaths::IsRelative(PngPath)) FullPath = FPaths::Combine(FPaths::ProjectDir(), PngPath);
        bLoaded = LoadHeightMapRaw(FullPath, W, H, LocalHeightData);
    }

    if (!bLoaded) return;

    TArray<FVector> LocalVertices;
    TArray<int32> LocalTriangles;
    TArray<FVector2D> LocalUVs;

    const float Scale = ZScale / 255.f;
    const float HalfWidth = (W - 1) * XYScale * 0.5f;
    const float HalfHeight = (H - 1) * XYScale * 0.5f;

    LocalVertices.SetNumUninitialized(W * H);
    LocalUVs.SetNumUninitialized(W * H);
    for (int32 y = 0; y < H; ++y)
    {
        for (int32 x = 0; x < W; ++x)
        {
            int32 idx = y * W + x;
            float h = LocalHeightData[idx] * Scale;
            LocalVertices[idx] = FVector(x * XYScale - HalfWidth, y * XYScale - HalfHeight, h);
            LocalUVs[idx] = FVector2D((float)x / (W - 1), (float)y / (H - 1));
        }
    }

    LocalTriangles.Reserve((W - 1) * (H - 1) * 6);
    for (int32 y = 0; y < H - 1; ++y)
    {
        for (int32 x = 0; x < W - 1; ++x)
        {
            int32 i = y * W + x;
            LocalTriangles.Append({ i, i + 1, i + W + 1,  i, i + W + 1, i + W });
        }
    }

    GeneratedMesh = NewObject<UStaticMesh>(this, TEXT("EditorGeneratedTerrainMesh"));
    GeneratedMesh->InitResources();
    GeneratedMesh->bAllowCPUAccess = true;
    GeneratedMesh->SetLightingGuid();

    // Pre-add a material slot before building so section 0 has a valid material index in editor
    {
        UMaterialInterface* BaseMatForBuild = TerrainMaterial ? TerrainMaterial : GetDefaultSurfaceMaterial();
        FStaticMaterial BuildMat(BaseMatForBuild, FName("TerrainMaterial"));
        BuildMat.UVChannelData.bInitialized = true;
        GeneratedMesh->GetStaticMaterials().Reset();
        GeneratedMesh->GetStaticMaterials().Add(BuildMat);
    }

    UStaticMeshDescription* Desc = GeneratedMesh->CreateStaticMeshDescription();
    Desc->GetVertexInstanceUVs().SetNumChannels(1);
    // 1) Luo vertexit kerran
    TArray<FVertexID> VertexIDs;
    // vertices
    VertexIDs.SetNum(LocalVertices.Num());
    for (int32 i = 0; i < LocalVertices.Num(); ++i)
    {
        const FVertexID Vid = Desc->CreateVertex();
        Desc->SetVertexPosition(Vid, LocalVertices[i]);
        VertexIDs[i] = Vid;
    }

    // one polygon group
    FPolygonGroupID Pgid = Desc->CreatePolygonGroup();

    // triangles (per-corner vertex instances + UVs)
    for (int32 i = 0; i < LocalTriangles.Num(); i += 3)
    {
        const int32 i0 = LocalTriangles[i + 0];
        const int32 i1 = LocalTriangles[i + 1];
        const int32 i2 = LocalTriangles[i + 2];

        const FVertexInstanceID VI0 = Desc->CreateVertexInstance(VertexIDs[i0]);
        const FVertexInstanceID VI1 = Desc->CreateVertexInstance(VertexIDs[i1]);
        const FVertexInstanceID VI2 = Desc->CreateVertexInstance(VertexIDs[i2]);

        Desc->SetVertexInstanceUV(VI0, LocalUVs[i0], 0);
        Desc->SetVertexInstanceUV(VI1, LocalUVs[i1], 0);
        Desc->SetVertexInstanceUV(VI2, LocalUVs[i2], 0);

        TArray<FEdgeID> NewEdges;
        Desc->CreateTriangle(Pgid, { VI0, VI1, VI2 }, NewEdges);
    }


#if WITH_EDITORONLY_DATA
    GeneratedMesh->SetNumSourceModels(1);
    FStaticMeshSourceModel& Src = GeneratedMesh->GetSourceModel(0);
    Src.BuildSettings.bRecomputeNormals = true;
    Src.BuildSettings.bRecomputeTangents = true;
    Src.BuildSettings.bUseMikkTSpace = false;
    Src.BuildSettings.bRemoveDegenerates = true;
    // Valinnaiset valotuskartta‑UV:t
    // Src.BuildSettings.bGenerateLightmapUVs = true;
    // GeneratedMesh->LightMapCoordinateIndex = 1;
    // GeneratedMesh->LightMapResolution      = 256;
#endif
    GeneratedMesh->BuildFromStaticMeshDescriptions({ Desc });
    GeneratedMesh->PostEditChange();
    GeneratedMesh->CalculateExtendedBounds();
    Mesh->SetStaticMesh(GeneratedMesh);
    Mesh->UpdateBounds();
    Mesh->MarkRenderStateDirty();

    if (GeneratedMesh->GetBodySetup())
    {
        GeneratedMesh->GetBodySetup()->CollisionTraceFlag = CTF_UseComplexAsSimple;
    }

    // Keep the pre-added slot

    Mesh->SetStaticMesh(GeneratedMesh);

    // Generate splat maps in editor for material previews
    {
        FProcTerrainPresetDefinition Def;
        FProcTerrainPresetDefinition* DefPtr = nullptr;
        if (Preset != ETerrainPreset::None && ProcTerrainPresets::GetPreset(Preset, Def))
        {
            DefPtr = &Def;
        }
        HeightmapWidth = W;
        HeightmapHeight = H;
        HeightData = LocalHeightData;
        GenerateSplatMaps(DefPtr);
        ApplyMaterialBindings(DefPtr);
    }

    CalculateSpawnPoints();
}

#if WITH_EDITOR
void ATerrainGen::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);

    if (PropertyChangedEvent.Property)
    {
        const FName PropertyName = PropertyChangedEvent.Property->GetFName();
        if (PropertyName == GET_MEMBER_NAME_CHECKED(ATerrainGen, Preset))
        {
            FProcTerrainPresetDefinition Def;
            if (ProcTerrainPresets::GetPreset(Preset, Def))
            {
                XYScale = Def.DefaultXYScale;
                ZScale = Def.DefaultZScale;
            }
        }

        // Regenerate in editor on any relevant property change
        if (PropertyName != GET_MEMBER_NAME_CHECKED(ATerrainGen, bGenerateOnBeginPlay))
        {
            GenerateTerrain_Editor();
        }
    }
}
#endif
