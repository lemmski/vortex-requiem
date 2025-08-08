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

namespace
{
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

    static ConstructorHelpers::FObjectFinder<UMaterialInterface> DefaultMaterial(TEXT("Material'/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial'"));
    if (DefaultMaterial.Succeeded())
    {
        TerrainMaterial = DefaultMaterial.Object;
        Mesh->SetMaterial(0, TerrainMaterial);
    }

    PngPath = TEXT("Content/Levels/OldWorldAnomalyLvl/old_world_anomaly_2k.png");
    HeightmapTexture = nullptr;
    XYScale = 10.f;
    ZScale  = 10.f;
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
		if(HasAuthority())
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
                Triangles.Append({ i, i + HeightmapWidth + 1, i + 1,  i, i + HeightmapWidth, i + HeightmapWidth + 1 });
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

    UStaticMeshDescription* Desc = GeneratedMesh->CreateStaticMeshDescription();
    
    TArray<FVertexInstanceID> VertexInstanceIDs;
    VertexInstanceIDs.SetNum(Vertices.Num());

    for(int32 i = 0; i < Vertices.Num(); ++i)
    {
        FVertexID VertexID = Desc->CreateVertex();
        Desc->SetVertexPosition(VertexID, Vertices[i]);
        FVertexInstanceID VI = Desc->CreateVertexInstance(VertexID);
        Desc->SetVertexInstanceUV(VI, UVs[i]);
        VertexInstanceIDs[i] = VI;
    }
    
    FPolygonGroupID PolygonGroup = Desc->CreatePolygonGroup();
    for (int32 i = 0; i < Triangles.Num(); i += 3)
    {
        FVertexInstanceID V0 = VertexInstanceIDs[Triangles[i]];
        FVertexInstanceID V1 = VertexInstanceIDs[Triangles[i+1]];
        FVertexInstanceID V2 = VertexInstanceIDs[Triangles[i+2]];
        TArray<FEdgeID> NewEdges;
        Desc->CreateTriangle(PolygonGroup, {V0, V1, V2}, NewEdges);
    }
    

    
    GeneratedMesh->BuildFromStaticMeshDescriptions({Desc});
    
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

    if (TerrainMaterial)
    {
        GeneratedMesh->GetStaticMaterials().Add(FStaticMaterial(TerrainMaterial, FName("TerrainMaterial")));
    }

    Mesh->SetStaticMesh(GeneratedMesh);
    
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
        OutWidth  = Wrapper->GetWidth();
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
    if (!RuntimeMID)
    {
        RuntimeMID = Mesh->CreateAndSetMaterialInstanceDynamic(0);
    }
    if (!RuntimeMID)
    {
        return;
    }

    // Base material assignment if provided
    if (TerrainMaterial)
    {
        Mesh->SetMaterial(0, RuntimeMID);
    }

    if (bApplySplatToMaterial)
    {
        for (const TPair<FName, UTexture2D*>& Pair : SplatGroupTextures)
        {
            const FName ParamName(*FString::Printf(TEXT("Splat_%s"), *Pair.Key.ToString()));
            RuntimeMID->SetTextureParameterValue(ParamName, Pair.Value);
        }
    }

    // Bind per-layer textures extracted from assigned layer material instances (grouped only)
    // Flat map already: AllPresetLayerMaterials is (Preset.Layer -> MI)
    TMap<FName, UMaterialInstance*> CombinedLayers = AllPresetLayerMaterials;

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
                const FString ParamName = FString::Printf(TEXT("Layer_%s_%s"), *LayerName.ToString(), DstSuffix);
                RuntimeMID->SetTextureParameterValue(FName(*ParamName), Tex);
            }
        };

        // Standard parameter names to fetch from layer MI
        SetTexFromMI(TEXT("BaseColor"), TEXT("BaseColor"));
        SetTexFromMI(TEXT("Normal"),    TEXT("Normal"));
        SetTexFromMI(TEXT("ORM"),       TEXT("ORM"));
        // Optional common alternates
        SetTexFromMI(TEXT("Albedo"),    TEXT("BaseColor"));
        SetTexFromMI(TEXT("RMA"),       TEXT("ORM"));
        SetTexFromMI(TEXT("RoughnessMetallicAO"), TEXT("ORM"));
    }
}

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
            if(Def.bThermalEnabled) PT.ApplyThermal(Def.Thermal);
            if(Def.bHydraulicEnabled) PT.ApplyHydraulic(Def.Hydraulic);
            LocalHeightData.SetNumUninitialized(W*H);
            for(int i = 0; i < W*H; ++i) LocalHeightData[i] = (uint8)FMath::Clamp(PT.HeightMap[i]*255.f, 0.f, 255.f);
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
    
    if(!bLoaded) return;
    
    TArray<FVector> LocalVertices;
    TArray<int32> LocalTriangles;
    TArray<FVector2D> LocalUVs;
    
    const float Scale = ZScale / 255.f;
    const float HalfWidth = (W - 1) * XYScale * 0.5f;
    const float HalfHeight = (H - 1) * XYScale * 0.5f;
    
    LocalVertices.SetNumUninitialized(W*H);
    LocalUVs.SetNumUninitialized(W*H);
    for(int32 y = 0; y < H; ++y)
    {
        for(int32 x = 0; x < W; ++x)
        {
            int32 idx = y * W + x;
            float h = LocalHeightData[idx] * Scale;
            LocalVertices[idx] = FVector(x*XYScale - HalfWidth, y*XYScale - HalfHeight, h);
            LocalUVs[idx] = FVector2D((float)x/(W-1), (float)y/(H-1));
        }
    }
    
    LocalTriangles.Reserve((W-1)*(H-1)*6);
    for(int32 y = 0; y < H-1; ++y)
    {
        for(int32 x = 0; x < W-1; ++x)
        {
            int32 i = y*W+x;
            LocalTriangles.Append({i, i+W+1, i+1, i, i+W, i+W+1});
        }
    }
    
    GeneratedMesh = NewObject<UStaticMesh>(this, TEXT("EditorGeneratedTerrainMesh"));
    GeneratedMesh->InitResources();
    GeneratedMesh->bAllowCPUAccess = true;
    GeneratedMesh->SetLightingGuid();

    UStaticMeshDescription* Desc = GeneratedMesh->CreateStaticMeshDescription();

    TArray<FVertexInstanceID> VertexInstanceIDs;
    VertexInstanceIDs.SetNum(LocalVertices.Num());

    for(int32 i = 0; i < LocalVertices.Num(); ++i)
    {
        FVertexID VertexID = Desc->CreateVertex();
        Desc->SetVertexPosition(VertexID, LocalVertices[i]);
        FVertexInstanceID VI = Desc->CreateVertexInstance(VertexID);
        Desc->SetVertexInstanceUV(VI, LocalUVs[i]);
        VertexInstanceIDs[i] = VI;
    }
    
    FPolygonGroupID PolygonGroup = Desc->CreatePolygonGroup();
    for (int32 i = 0; i < LocalTriangles.Num(); i += 3)
    {
        FVertexInstanceID V0 = VertexInstanceIDs[LocalTriangles[i]];
        FVertexInstanceID V1 = VertexInstanceIDs[LocalTriangles[i+1]];
        FVertexInstanceID V2 = VertexInstanceIDs[LocalTriangles[i+2]];
        TArray<FEdgeID> NewEdges;
        Desc->CreateTriangle(PolygonGroup, {V0, V1, V2}, NewEdges);
    }
    

    
    GeneratedMesh->BuildFromStaticMeshDescriptions({Desc});
    GeneratedMesh->PostEditChange();
    
    if (GeneratedMesh->GetBodySetup())
    {
        GeneratedMesh->GetBodySetup()->CollisionTraceFlag = CTF_UseComplexAsSimple;
    }
    
    if (TerrainMaterial)
    {
        GeneratedMesh->GetStaticMaterials().Add(FStaticMaterial(TerrainMaterial, FName("TerrainMaterial")));
    }

    Mesh->SetStaticMesh(GeneratedMesh);

    // Generate splat maps in editor for material previews
    {
        FProcTerrainPresetDefinition Def;
        FProcTerrainPresetDefinition* DefPtr = nullptr;
        if (Preset != ETerrainPreset::None && ProcTerrainPresets::GetPreset(Preset, Def))
        {
            DefPtr = &Def;
        }
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
