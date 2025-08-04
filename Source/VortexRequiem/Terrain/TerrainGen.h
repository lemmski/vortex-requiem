#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ProceduralMeshComponent.h"
#include "Engine/Texture2D.h"
#include "ProcTerrain.h"

UENUM(BlueprintType)
enum class ETerrainPreset : uint8
{
    None                     UMETA(DisplayName="None"),
    DowntownRuins            UMETA(DisplayName="Downtown Ruins"),
    CrystallineBloomfallZone UMETA(DisplayName="Crystalline Bloomfall Zone"),
    MutatedSwamplands        UMETA(DisplayName="Mutated Swamplands"),
    IrradiatedBadlands       UMETA(DisplayName="Irradiated Badlands"),
    OldWorldAnomaly          UMETA(DisplayName="Old World Anomaly"),
    GothicCathedralApproach  UMETA(DisplayName="Gothic Cathedral Approach"),
    MangroveDeltaFull        UMETA(DisplayName="Mangrove Delta Full"),
    ProvingGroundsSmall      UMETA(DisplayName="Proving Grounds Small"),
    ArenaTiny513             UMETA(DisplayName="Arena Tiny 513")
};
#include "TerrainGen.generated.h"

/**
 * Runtime height-map terrain generated from a grayscale PNG.
 * Drop the actor in the level, set PngPath and scales and it will build a collision-enabled mesh.
 */
UCLASS()
class VORTEXREQUIEM_API ATerrainGen : public AActor
{
    GENERATED_BODY()

public:
    ATerrainGen();

    // Path to the grayscale height-map (absolute or relative to project root)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Terrain")
    FString PngPath;

    // Alternatively reference a texture asset directly
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Terrain")
    UTexture2D* HeightmapTexture;

    // Selected procedural terrain preset (ignored if PngPath/HeightmapTexture provided)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Terrain")
    ETerrainPreset Preset;

    // XY size of a single pixel (world units)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Terrain")
    float XYScale;

    // World height corresponding to white (255)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Terrain")
    float ZScale;

    // Maximum quads per tile (127 keeps vertices under 128×128 = 16k)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Terrain")
    int32 TileQuads;

    // Height variation tolerance (world units) to merge rows/cols
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Terrain")
    float HeightTolerance;

    // Optional loading widget displayed during terrain rebuild (use a full-screen widget with the desired texture)
    UPROPERTY(EditAnywhere, Category="Terrain|UI")
    TSubclassOf<class UUserWidget> LoadingWidgetClass;

    virtual void OnConstruction(const FTransform& Transform) override;

protected:
    virtual void BeginPlay() override;

private:
    // Procedural mesh should never be serialized to disk – it is rebuilt on demand
    UPROPERTY(VisibleAnywhere, Transient)
    UProceduralMeshComponent* Mesh;

    UPROPERTY(Transient)
    class UUserWidget* ActiveLoadingWidget;

    UFUNCTION(CallInEditor, Category="Terrain")
    void Regenerate();

    void GenerateTerrain();
    static bool LoadHeightMapRaw(const FString& FilePath, int32& OutWidth, int32& OutHeight, TArray<uint8>& OutData);
};
