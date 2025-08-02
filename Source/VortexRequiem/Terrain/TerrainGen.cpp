#include "TerrainGen.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Engine/CollisionProfile.h"

ATerrainGen::ATerrainGen()
{
    PrimaryActorTick.bCanEverTick = false;

    Mesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("ProcMesh"));
    SetRootComponent(Mesh);
    Mesh->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);

    // Reasonable defaults
    PngPath = TEXT("Content/Levels/OldWorldAnomalyLvl/old_world_anomaly_2k.png");
    HeightmapTexture = nullptr;
    XYScale = 10.f;
    ZScale  = 10.f;
    TileQuads = 127;

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
        UE_LOG(LogTemp, Warning, TEXT("ATerrainGen: Heightmap load failed"));
        return;
    }

    // Clear previous mesh sections
    Mesh->ClearAllMeshSections();

    const int32 QuadsPerTile = FMath::Max(1, TileQuads);
    int32 SectionIdx = 0;

    for (int32 TileY = 0; TileY < H - 1; TileY += QuadsPerTile)
    {
        for (int32 TileX = 0; TileX < W - 1; TileX += QuadsPerTile)
        {
            const int32 LocalW = FMath::Min(QuadsPerTile, (W - 1) - TileX) + 1;
            const int32 LocalH = FMath::Min(QuadsPerTile, (H - 1) - TileY) + 1;
            const int32 LocalVertsCount = LocalW * LocalH;

            TArray<FVector> Verts;        Verts.SetNumUninitialized(LocalVertsCount);
            TArray<FVector2D> UVs;        UVs.SetNumUninitialized(LocalVertsCount);

            for (int32 y = 0; y < LocalH; ++y)
            {
                for (int32 x = 0; x < LocalW; ++x)
                {
                    const int32 GlobalX = TileX + x;
                    const int32 GlobalY = TileY + y;
                    const int32 HeightIdx = GlobalY * W + GlobalX;

                    const float h = (HeightData[HeightIdx] / 255.f) * ZScale;
                    const int32 vi = y * LocalW + x;

                    Verts[vi] = FVector(GlobalX * XYScale, GlobalY * XYScale, h);
                    UVs[vi]   = FVector2D((float)GlobalX / (W - 1), (float)GlobalY / (H - 1));
                }
            }

            TArray<int32> Tris;
            Tris.Reserve((LocalW - 1) * (LocalH - 1) * 6);

            for (int32 y = 0; y < LocalH - 1; ++y)
            {
                for (int32 x = 0; x < LocalW - 1; ++x)
                {
                    const int32 i  =  y      * LocalW + x;
                    const int32 iR =  i + 1;
                    const int32 iD = (y + 1) * LocalW + x;
                    const int32 iDR= iD + 1;

                    Tris.Append({ i, iDR, iR,  i, iD, iDR });
                }
            }

            Mesh->CreateMeshSection_LinearColor(SectionIdx++, Verts, Tris, {}, UVs, {}, {}, true);
        }
    }

    Mesh->ContainsPhysicsTriMeshData(true);
}
