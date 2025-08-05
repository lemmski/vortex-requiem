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

UCLASS()
class VORTEXREQUIEM_API ATerrainGen : public AActor
{
    GENERATED_BODY()

public:
    ATerrainGen();

    // Regenerate the terrain with the specified preset
    UFUNCTION(BlueprintCallable, Category="Terrain")
    void GenerateTerrainFromPreset(ETerrainPreset NewPreset);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Terrain")
    bool bGenerateOnBeginPlay;

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

    // The number of player starts to create
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawning")
    int32 NumPlayerStarts;
    
    // Maximum slope in degrees for a valid spawn point
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawning", meta = (ClampMin = "0.0", ClampMax = "90.0"))
    float MaxSpawnSlopeInDegrees;
    
    // Minimum distance between spawn points
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawning", meta = (ClampMin = "0.0"))
    float MinSpawnSeparation;
    
    // Radius around a potential spawn point to check for obstructions
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawning", meta = (ClampMin = "0.0"))
    float SpawnClearanceRadius;
    
    // The locations of the calculated spawn points
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Spawning")
    TArray<FVector> SpawnPoints;

    // If true, the debug spheres for spawn points will be drawn larger
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawning")
    bool bUseLargeSpawnSpheres;

#if WITH_EDITOR
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

    virtual void OnConstruction(const FTransform& Transform) override;

protected:
    virtual void BeginPlay() override;

private:
    // Procedural mesh should never be serialized to disk – it is rebuilt on demand
    UPROPERTY(VisibleAnywhere, Transient)
    UProceduralMeshComponent* Mesh;

    UPROPERTY(Transient)
    class UUserWidget* ActiveLoadingWidget;

    // Timer for polling collision readiness
    FTimerHandle CollisionReadyTimer;

    // List of actors to re-enable physics on
    TArray<TWeakObjectPtr<AActor>> ActorsToReenablePhysics;

    UFUNCTION(CallInEditor, Category="Terrain")
    void Regenerate();

    void GenerateTerrain();
    void CalculateSpawnPoints();
    void CheckCollisionReady();
    void DisableActorPhysicsTemporarily();
    void RestoreActorPhysics();
    static bool LoadHeightMapRaw(const FString& FilePath, int32& OutWidth, int32& OutHeight, TArray<uint8>& OutData);
};
