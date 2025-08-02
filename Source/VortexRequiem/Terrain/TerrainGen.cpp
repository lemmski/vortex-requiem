#include "TerrainGen.h"
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

ATerrainGen::ATerrainGen()
{
    PrimaryActorTick.bCanEverTick = false;

    Mesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("ProcMesh"));
    SetRootComponent(Mesh);
    Mesh->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
    Mesh->bUseAsyncCooking = true;

    // Reasonable defaults
    PngPath = TEXT("Content/Levels/OldWorldAnomalyLvl/old_world_anomaly_2k.png");
    HeightmapTexture = nullptr;
    XYScale = 10.f;
    ZScale  = 10.f;
    TileQuads = 127;
    HeightTolerance = 5.f;

#if WITH_EDITORONLY_DATA
    Mesh->bUseComplexAsSimpleCollision = true;
#endif
}

void ATerrainGen::OnConstruction(const FTransform& Transform)
{
    Super::OnConstruction(Transform);
    GenerateTerrain();
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
    int32 W = 0, H = 0; TArray<uint8> HeightData;

    bool bLoaded = false;

    // 1) Load from texture asset if provided
    if (HeightmapTexture)
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
    else // 2) load from PNG file path
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
        Mesh->SetCanEverAffectNavigation(true);
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
        Mesh->SetCanEverAffectNavigation(true);
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
}
