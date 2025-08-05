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
    // Prevent huge mesh data from being recorded in the editor undo/redo buffer and from being duplicated into PIE
    Mesh->ClearFlags(RF_Transactional);
    Mesh->SetFlags(RF_Transient | RF_DuplicateTransient);
    SetRootComponent(Mesh);
    Mesh->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
    Mesh->SetMobility(EComponentMobility::Static);
    Mesh->SetCastShadow(false);
    Mesh->bUseAsyncCooking = GetWorld() && GetWorld()->IsEditorWorld();

    // Reasonable defaults
    PngPath = TEXT("Content/Levels/OldWorldAnomalyLvl/old_world_anomaly_2k.png");
    HeightmapTexture = nullptr;
    XYScale = 10.f;
    ZScale  = 10.f;
    TileQuads = 127;
    HeightTolerance = 5.f;

    Preset = ETerrainPreset::None;
    bGenerateOnBeginPlay = false;

    // Spawning defaults
    NumPlayerStarts = 10;
    MaxSpawnSlopeInDegrees = 25.0f;
    MinSpawnSeparation = 1000.0f;
    SpawnClearanceRadius = 100.0f;
    bUseLargeSpawnSpheres = false;

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
    // If we have a loading widget class, display it full-screen
    if (LoadingWidgetClass && !ActiveLoadingWidget)
    {
        ActiveLoadingWidget = CreateWidget<UUserWidget>(GetWorld(), LoadingWidgetClass);
        if (ActiveLoadingWidget)
        {
            ActiveLoadingWidget->AddToViewport(9999);
        }
    }

    GenerateTerrain();

    // Remove loading widget when done
    if (ActiveLoadingWidget)
    {
        ActiveLoadingWidget->RemoveFromParent();
        ActiveLoadingWidget = nullptr;
    }
}

void ATerrainGen::OnConstruction(const FTransform& Transform)
{
    Super::OnConstruction(Transform);
    GenerateTerrain();
}

void ATerrainGen::BeginPlay()
{
    Super::BeginPlay();

    if (bGenerateOnBeginPlay)
    {
        GenerateTerrain();
    }
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

void ATerrainGen::GenerateTerrain()
{
    // Ensure we have a valid procedural mesh component (it may be null in duplicated PIE worlds)
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
        Mesh->bUseAsyncCooking = GetWorld() && GetWorld()->IsEditorWorld();
    }

    int32 W = 0, H = 0; TArray<uint8> HeightData;

    // Build cache key from current settings
    FString CacheKey;
    if (HeightmapTexture)
    {
        CacheKey = FString::Printf(TEXT("Tex:%s_%f_%f_%f"), *HeightmapTexture->GetPathName(), XYScale, ZScale, HeightTolerance);
    }
    else if (Preset != ETerrainPreset::None)
    {
        CacheKey = FString::Printf(TEXT("Preset:%d_%f_%f_%f"), static_cast<int32>(Preset), XYScale, ZScale, HeightTolerance);
    }
    else
    {
        CacheKey = FString::Printf(TEXT("File:%s_%f_%f_%f"), *PngPath, XYScale, ZScale, HeightTolerance);
    }

    // If we have a cached mesh for this key, reuse it
    if (GTerrainCache.bValid && GTerrainCache.Key == CacheKey && Mesh)
    {
        Mesh->CreateMeshSection_LinearColor(0, GTerrainCache.Vertices, GTerrainCache.Triangles, {}, GTerrainCache.UVs, {}, {}, true);
        CalculateSpawnPoints();
        return;
    }

    bool bLoaded = false;

    // 0) Generate procedurally from preset if selected
    if (Preset != ETerrainPreset::None)
    {
        FProcTerrainPresetDefinition Def;
        if (ProcTerrainPresets::GetPreset(Preset, Def))
        {
            W = Def.Width;
            H = Def.Height;
            FProcTerrain PT(W, H, Def.Seed);
            PT.GenerateFBM(Def.Fbm);
            if (Def.bThermalEnabled)  PT.ApplyThermal(Def.Thermal);
            if (Def.bHydraulicEnabled) PT.ApplyHydraulic(Def.Hydraulic);

            HeightData.SetNumUninitialized(W * H);
            for (int32 i = 0; i < W * H; ++i)
            {
                HeightData[i] = static_cast<uint8>(FMath::Clamp(PT.HeightMap[i] * 255.0f, 0.0f, 255.0f));
            }
            bLoaded = true;
        }
    }

    // 1) Load from texture asset if provided (only if preset did not already generate)
    if (!bLoaded && HeightmapTexture)
    {
        if (HeightmapTexture->GetPlatformData() && HeightmapTexture->GetPlatformData()->Mips.Num() > 0)
        {
            FTexture2DMipMap& Mip = HeightmapTexture->GetPlatformData()->Mips[0];
            W = Mip.SizeX;
            H = Mip.SizeY;
            HeightData.SetNumUninitialized(W * H);

            const FColor* Src = static_cast<const FColor*>(Mip.BulkData.LockReadOnly());
            for (int32 i = 0; i < W * H; ++i)
            {
                HeightData[i] = Src[i].R; // red channel assumed height
            }
            Mip.BulkData.Unlock();
            bLoaded = true;
        }
    }
    else if (!bLoaded) // 2) load from PNG file path
    {
        FString FullPath = PngPath;
        if (FPaths::IsRelative(PngPath))
        {
            FullPath = FPaths::Combine(FPaths::ProjectDir(), PngPath);
        }
        bLoaded = LoadHeightMapRaw(FullPath, W, H, HeightData);
    }

    if (!bLoaded || W == 0 || H == 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("ATerrainGen: Heightmap load failed – generating procedural fallback"));

        // --- Procedural fallback ---
        W = 1024;
        H = 1024;
        const int32 Seed = FMath::Rand();

        FProcTerrain PT(W, H, Seed);
        FFBMSettings FBMSettings;
        FThermalSettings ThermalSettings;
        FHydraulicSettings HydraulicSettings;

        PT.GenerateFBM(FBMSettings);
        PT.ApplyThermal(ThermalSettings);
        PT.ApplyHydraulic(HydraulicSettings);

        HeightData.SetNumUninitialized(W * H);
        for (int32 i = 0; i < W * H; ++i)
        {
            HeightData[i] = static_cast<uint8>(FMath::Clamp(PT.HeightMap[i] * 255.0f, 0.0f, 255.0f));
        }
        bLoaded = true;
    }

    // Acquire nav-system lock to suppress per-section rebuilds
    UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
    FNavigationLockContext NavLock(GetWorld(), ENavigationLockReason::Unknown);

    // Clear and disable nav while rebuilding mesh
    Mesh->ClearAllMeshSections();
    Mesh->SetCanEverAffectNavigation(false);

    // timing variables
    double OverallStart = FPlatformTime::Seconds();
    double MaskTime = 0.0, VertTime = 0.0, TriTime = 0.0, UploadTime = 0.0, NavKickTime = 0.0;

    if (HeightTolerance <= 0.f)
    {
        // full resolution path
        UE_LOG(LogTemp, Log, TEXT("Verts total: %d"), W * H);
        TArray<FVector> Verts;        Verts.SetNumUninitialized(W * H);
        TArray<FVector2D> UVs;        UVs.SetNumUninitialized(W * H);
        const float Scale = ZScale / 255.f;
        double MaskStart = FPlatformTime::Seconds();
        // no mask step in full path
        MaskTime = 0.0;
        double VertStart = FPlatformTime::Seconds();
        for (int32 y = 0; y < H; ++y)
        {
            for (int32 x = 0; x < W; ++x)
            {
                int32 idx = y * W + x;
                float h = HeightData[idx] * Scale;
                Verts[idx] = FVector(x * XYScale, y * XYScale, h);
                UVs[idx]   = FVector2D((float)x / (W - 1), (float)y / (H - 1));
            }
        }
        VertTime = FPlatformTime::Seconds() - VertStart;

        double TriStart = FPlatformTime::Seconds();
        TArray<int32> Tris;
        Tris.Reserve((W - 1) * (H - 1) * 6);
        for (int32 y = 0; y < H - 1; ++y)
        {
            for (int32 x = 0; x < W - 1; ++x)
            {
                int32 i = y * W + x;
                Tris.Append({ i, i + W + 1, i + 1,  i, i + W, i + W + 1 });
            }
        }
        TriTime = FPlatformTime::Seconds() - TriStart;

        double UploadStart = FPlatformTime::Seconds();
        Mesh->CreateMeshSection_LinearColor(0, Verts, Tris, {}, UVs, {}, {}, true);
        // cache for reuse in PIE/editor duplication
        GTerrainCache.Vertices = Verts;
        GTerrainCache.Triangles = Tris;
        GTerrainCache.UVs = UVs;
        GTerrainCache.Key = CacheKey;
        GTerrainCache.bValid = true;
        // Disable navmesh generation for this heavy terrain mesh to avoid Recast cache overflow
    Mesh->SetCanEverAffectNavigation(false);
        UploadTime = FPlatformTime::Seconds() - UploadStart;

    }
    else
    {
        const float Tol = HeightTolerance; // world-unit tolerance
        // --- reduction masks ---
        TBitArray<> KeepRow(false, H);
        TBitArray<> KeepCol(false, W);
        const float Scale = ZScale / 255.f;
        for (int32 y = 0; y < H; ++y)
        {
            uint8 minv = 255, maxv = 0;
            for (int32 x = 0; x < W; ++x)
            {
                uint8 v = HeightData[y * W + x];
                minv = FMath::Min(minv, v);
                maxv = FMath::Max(maxv, v);
            }
            if ((maxv - minv) * Scale > Tol) KeepRow[y] = true;
        }
        for (int32 x = 0; x < W; ++x)
        {
            uint8 minv = 255, maxv = 0;
            for (int32 y = 0; y < H; ++y)
            {
                uint8 v = HeightData[y * W + x];
                minv = FMath::Min(minv, v);
                maxv = FMath::Max(maxv, v);
            }
            if ((maxv - minv) * Scale > Tol) KeepCol[x] = true;
        }
        KeepRow[0] = true;
        KeepRow[H - 1] = true;
        KeepCol[0] = true;
        KeepCol[W - 1] = true;

        TArray<int32> Rows, Cols;
        for (int32 y = 0; y < H; ++y) if (KeepRow[y]) Rows.Add(y);
        for (int32 x = 0; x < W; ++x) if (KeepCol[x]) Cols.Add(x);

        int32 NewH = Rows.Num();
        int32 NewW = Cols.Num();
        UE_LOG(LogTemp, Log, TEXT("Verts before: %d after: %d"), W * H, NewW * NewH);

        TArray<FVector> Verts;        Verts.SetNumUninitialized(NewW * NewH);
        TArray<FVector2D> UVs;        UVs.SetNumUninitialized(NewW * NewH);
        for (int32 yi = 0; yi < NewH; ++yi)
        {
            int32 gy = Rows[yi];
            for (int32 xi = 0; xi < NewW; ++xi)
            {
                int32 gx = Cols[xi];
                uint8 v = HeightData[gy * W + gx];
                float h = v * Scale;
                int32 idx = yi * NewW + xi;
                Verts[idx] = FVector(gx * XYScale, gy * XYScale, h);
                UVs[idx] = FVector2D((float)gx / (W - 1), (float)gy / (H - 1));
            }
        }
        TArray<int32> Tris;
        Tris.Reserve((NewW - 1) * (NewH - 1) * 6);
        for (int32 y = 0; y < NewH - 1; ++y)
        {
            for (int32 x = 0; x < NewW - 1; ++x)
            {
                int32 i = y * NewW + x;
                Tris.Append({ i, i + NewW + 1, i + 1,  i, i + NewW, i + NewW + 1 });
            }
        }
        double BuildStart = FPlatformTime::Seconds();
        Mesh->CreateMeshSection_LinearColor(0, Verts, Tris, {}, UVs, {}, {}, true);
        // cache for reuse in PIE/editor duplication
        GTerrainCache.Vertices = Verts;
        GTerrainCache.Triangles = Tris;
        GTerrainCache.UVs = UVs;
        GTerrainCache.Key = CacheKey;
        GTerrainCache.bValid = true;
        // Disable navmesh generation for this heavy terrain mesh to avoid Recast cache overflow
    Mesh->SetCanEverAffectNavigation(false);
        double BuildDone = FPlatformTime::Seconds();
        UploadTime = BuildDone - BuildStart;
        UE_LOG(LogTemp, Log, TEXT("ATerrainGen: geometry upload = %.2f s"), UploadTime);
    }

    // Lock goes out of scope here; start async nav build
    double NavKickStart = FPlatformTime::Seconds();
    if (NavSys)
    {
        NavSys->Build();
    }
    NavKickTime = FPlatformTime::Seconds() - NavKickStart;

    double Total = FPlatformTime::Seconds() - OverallStart;
    UE_LOG(LogTemp, Log, TEXT("Timing ms | Mask creation:%6.1f  Vertex fill:%6.1f  Triangle list:%6.1f  GPU upload:%6.1f  Nav-mesh kick:%6.1f  Total:%6.1f"),
        MaskTime*1000, VertTime*1000, TriTime*1000, UploadTime*1000, NavKickTime*1000, Total*1000);

    CalculateSpawnPoints();

    APawn* PlayerPawn = UGameplayStatics::GetPlayerPawn(GetWorld(), 0);
    if (PlayerPawn && SpawnPoints.Num() > 0)
    {
        int32 SpawnIndex = FMath::RandRange(0, SpawnPoints.Num() - 1);
        FVector WorldLocation = SpawnPoints[SpawnIndex];

        float Safety = 0.1f;
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
}

void ATerrainGen::CalculateSpawnPoints()
{
    SpawnPoints.Empty();
    if (NumPlayerStarts <= 0 || !Mesh)
    {
        return;
    }

    const FProcMeshSection* Section = Mesh->GetProcMeshSection(0);
    if (!Section || Section->ProcIndexBuffer.Num() == 0)
    {
        return;
    }

    const float MaxSlopeCosine = FMath::Cos(FMath::DegreesToRadians(MaxSpawnSlopeInDegrees));
    const FTransform ActorToWorld = GetActorTransform();

    TArray<FVector> CandidateLocations;

    for (int32 i = 0; i < Section->ProcIndexBuffer.Num(); i += 3)
    {
        FVector V0 = Section->ProcVertexBuffer[Section->ProcIndexBuffer[i]].Position;
        FVector V1 = Section->ProcVertexBuffer[Section->ProcIndexBuffer[i + 1]].Position;
        FVector V2 = Section->ProcVertexBuffer[Section->ProcIndexBuffer[i + 2]].Position;

        // Calculate the normal in local space
        FVector LocalNormal = FVector::CrossProduct(V2 - V0, V1 - V0).GetSafeNormal();

        // Transform the normal to world space
        FVector WorldNormal = ActorToWorld.TransformVectorNoScale(LocalNormal);
        WorldNormal.Normalize();

        // Ensure the normal is pointing upwards
        if (WorldNormal.Z < 0)
        {
            WorldNormal = -WorldNormal;
        }

        // Check if the surface is flat enough
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

    // Shuffle candidates to get random distribution
    for (int32 i = 0; i < CandidateLocations.Num(); ++i)
    {
        int32 j = FMath::RandRange(i, CandidateLocations.Num() - 1);
        CandidateLocations.Swap(i, j);
    }

    UWorld* World = GetWorld();
    if (!World) return;

    for (const FVector& LocalCandidate : CandidateLocations)
    {
        if (SpawnPoints.Num() >= NumPlayerStarts)
        {
            break;
        }

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

    UE_LOG(LogTemp, Log, TEXT("Found %d spawn points."), SpawnPoints.Num());

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
        // Body setup is complete, collision is ready
        GetWorldTimerManager().ClearTimer(CollisionReadyTimer);
        RestoreActorPhysics();
        UE_LOG(LogTemp, Log, TEXT("ATerrainGen: Collision is ready, re-enabling actor physics"));
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

#if WITH_EDITOR
void ATerrainGen::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);

    if (PropertyChangedEvent.Property && PropertyChangedEvent.Property->GetFName() == GET_MEMBER_NAME_CHECKED(ATerrainGen, Preset))
    {
        FProcTerrainPresetDefinition Def;
        if (ProcTerrainPresets::GetPreset(Preset, Def))
        {
            XYScale = Def.DefaultXYScale;
            ZScale = Def.DefaultZScale;
        }
    }
}
#endif
