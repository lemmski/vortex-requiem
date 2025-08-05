#include "TerrainGen.h"
#include "VortexRequiemGameInstance.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
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

    Mesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("ProcMesh"));
    Mesh->ClearFlags(RF_Transactional);
    Mesh->SetFlags(RF_Transient | RF_DuplicateTransient);
    SetRootComponent(Mesh);
    Mesh->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
    Mesh->SetMobility(EComponentMobility::Static);
    Mesh->SetCastShadow(false);
    Mesh->bUseAsyncCooking = false;

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

#if WITH_EDITORONLY_DATA
    Mesh->bUseComplexAsSimpleCollision = true;
#endif
}

void ATerrainGen::GenerateTerrainFromPreset(ETerrainPreset NewPreset)
{
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
        GenerateTerrain_Editor();
    }
}

void ATerrainGen::BeginPlay()
{
    Super::BeginPlay();

    if (bGenerateOnBeginPlay)
    {
        StartAsyncGeneration();
    }
}

void ATerrainGen::StartAsyncGeneration()
{
    if (CurrentState != EGenerationState::Idle)
    {
        UE_LOG(LogTemp, Warning, TEXT("ATerrainGen::StartAsyncGeneration called while already busy."));
        return;
    }

    OnGenerationProgress.Broadcast(FText::FromString("Starting terrain generation..."));

    if (!Mesh)
    {
        Mesh = NewObject<UProceduralMeshComponent>(this, TEXT("ProcMeshPIE"));
        Mesh->RegisterComponent();
        SetRootComponent(Mesh);
        Mesh->ClearFlags(RF_Transactional);
        Mesh->SetFlags(RF_Transient | RF_DuplicateTransient);
        Mesh->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
        Mesh->SetMobility(EComponentMobility::Static);
        Mesh->SetCastShadow(false);
        Mesh->bUseAsyncCooking = true;
    }
    
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
        FProcTerrain PT(HeightmapWidth, HeightmapHeight, FMath::Rand());
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
        for (int32 y = 0; y < HeightmapHeight; ++y)
        {
            for (int32 x = 0; x < HeightmapWidth; ++x)
            {
                int32 idx = y * HeightmapWidth + x;
                float h = HeightData[idx] * Scale;
                Vertices[idx] = FVector(x * XYScale, y * XYScale, h);
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

        Vertices.SetNumUninitialized(NewW * NewH);
        UVs.SetNumUninitialized(NewW * NewH);
        for (int32 yi = 0; yi < NewH; ++yi)
        {
            for (int32 xi = 0; xi < NewW; ++xi)
            {
                int32 gy = Rows[yi], gx = Cols[xi];
                float h = HeightData[gy * HeightmapWidth + gx] * Scale;
                int32 idx = yi * NewW + xi;
                Vertices[idx] = FVector(gx * XYScale, gy * XYScale, h);
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
    
    CurrentState = EGenerationState::UploadMesh;
}

void ATerrainGen::Step_UploadMesh()
{
    OnGenerationProgress.Broadcast(FText::FromString("Uploading mesh to GPU..."));

    Mesh->ClearAllMeshSections();
    Mesh->CreateMeshSection_LinearColor(0, Vertices, Triangles, {}, UVs, {}, {}, true);
    
    // Cache the generated mesh
    GTerrainCache.Vertices = Vertices;
    GTerrainCache.Triangles = Triangles;
    GTerrainCache.UVs = UVs;
    GTerrainCache.Key = CurrentCacheKey;
    GTerrainCache.bValid = true;

    Mesh->SetCanEverAffectNavigation(false); // Disable for now
    
    CurrentState = EGenerationState::CalculateSpawnPoints;
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

    APawn* PlayerPawn = UGameplayStatics::GetPlayerPawn(GetWorld(), 0);
    if (PlayerPawn && SpawnPoints.Num() > 0)
    {
        int32 SpawnIndex = FMath::RandRange(0, SpawnPoints.Num() - 1);
        FVector WorldLocation = SpawnPoints[SpawnIndex];

        float Safety = 10.0f;
        float PawnHalfHeight = 0.f;
        if (ACharacter* Character = Cast<ACharacter>(PlayerPawn))
        {
            PawnHalfHeight = Character->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
        }
        WorldLocation.Z += PawnHalfHeight + Safety;
        PlayerPawn->SetActorLocation(WorldLocation);
    }

    if (Mesh->bUseAsyncCooking)
    {
        DisableActorPhysicsTemporarily();
        GetWorldTimerManager().SetTimer(CollisionReadyTimer, this, &ATerrainGen::CheckCollisionReady, 0.1f, true);
    }

    OnGenerationComplete.Broadcast();
    CurrentState = EGenerationState::Idle;
    GetWorldTimerManager().ClearTimer(GenerationProcessTimer);
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

void ATerrainGen::CalculateSpawnPoints()
{
    SpawnPoints.Empty();
    if (NumPlayerStarts <= 0 || !Mesh) return;

    const FProcMeshSection* Section = Mesh->GetProcMeshSection(0);
    if (!Section || Section->ProcIndexBuffer.Num() == 0) return;

    const float MaxSlopeCosine = FMath::Cos(FMath::DegreesToRadians(MaxSpawnSlopeInDegrees));
    const FTransform ActorToWorld = GetActorTransform();

    TArray<FVector> CandidateLocations;
    for (int32 i = 0; i < Section->ProcIndexBuffer.Num(); i += 3)
    {
        FVector V0 = Section->ProcVertexBuffer[Section->ProcIndexBuffer[i]].Position;
        FVector V1 = Section->ProcVertexBuffer[Section->ProcIndexBuffer[i + 1]].Position;
        FVector V2 = Section->ProcVertexBuffer[Section->ProcIndexBuffer[i + 2]].Position;

        FVector LocalNormal = FVector::CrossProduct(V2 - V0, V1 - V0).GetSafeNormal();
        FVector WorldNormal = ActorToWorld.TransformVectorNoScale(LocalNormal).GetSafeNormal();
        if (WorldNormal.Z < 0) WorldNormal = -WorldNormal;

        if (WorldNormal.Z >= MaxSlopeCosine)
        {
            CandidateLocations.Add((V0 + V1 + V2) / 3.0f);
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
        }
    }

#if WITH_EDITOR
    FlushPersistentDebugLines(GetWorld());
    const float DebugSphereRadius = bUseLargeSpawnSpheres ? SpawnClearanceRadius * 5.0f : SpawnClearanceRadius;
    for (const FVector& SpawnPoint : SpawnPoints)
    {
        DrawDebugSphere(GetWorld(), SpawnPoint, DebugSphereRadius, 12, FColor::Green, true, -1, 0, 5.f);
    }
#endif
}

void ATerrainGen::CheckCollisionReady()
{
    if (Mesh && Mesh->GetBodySetup() && Mesh->GetBodySetup()->bHasCookedCollisionData)
    {
        GetWorldTimerManager().ClearTimer(CollisionReadyTimer);
        RestoreActorPhysics();
        UE_LOG(LogTemp, Log, TEXT("ATerrainGen: Collision is ready."));
    }
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
    if (!Mesh)
    {
        Mesh = NewObject<UProceduralMeshComponent>(this, TEXT("ProcMeshEditor"));
        Mesh->RegisterComponent();
        SetRootComponent(Mesh);
    }

    int32 W, H;
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
    LocalVertices.SetNumUninitialized(W*H);
    LocalUVs.SetNumUninitialized(W*H);
    for(int32 y = 0; y < H; ++y)
    {
        for(int32 x = 0; x < W; ++x)
        {
            int32 idx = y * W + x;
            float h = LocalHeightData[idx] * Scale;
            LocalVertices[idx] = FVector(x*XYScale, y*XYScale, h);
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
    
    Mesh->ClearAllMeshSections();
    Mesh->CreateMeshSection_LinearColor(0, LocalVertices, LocalTriangles, {}, LocalUVs, {}, {}, true);
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